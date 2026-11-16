"""Qt 6 + KDE Kirigami GUI backend and KDE Plasma System Tray integration for Kalm."""

from __future__ import annotations

import os
import shutil
import signal
import subprocess
import sys
import threading
from pathlib import Path
from typing import Callable, List, Optional

os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "org.kde.desktop")

from PySide6.QtCore import Property, QObject, QRectF, Qt, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import (
    QAction,
    QActionGroup,
    QColor,
    QFont,
    QIcon,
    QPainter,
    QPen,
    QPixmap,
    QWindow,
)
from PySide6.QtQml import QQmlComponent, QQmlEngine
from PySide6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

from . import APP_ID, APP_NAME
from .client import IpcServer, KalmSession, ipc_request
from .protocol import (
    AUDIO_EFFECT_PRESETS,
    NOISE_CYCLE_PRESETS,
    BatteryReading,
    EarbudState,
    NoiseMode,
)
from .registry import DeviceRegistry
from .transport import connect_bluez_device, list_bluez_devices

WEAR_LABELS = {
    "worn": "В ухе",
    "in_case": "В кейсе",
    "removed": "Снят",
}

EQ_LIST = [{"text": label, "value": int(code)} for code, label in AUDIO_EFFECT_PRESETS.items()]
CYCLE_LIST = [{"text": label, "value": int(code)} for code, label in NOISE_CYCLE_PRESETS.items()]


def make_kalm_icon(connected: bool, mode: Optional[NoiseMode]) -> QIcon:
    """Render a crisp 64x64 vector tray/window icon reflecting current ANC mode."""
    px = QPixmap(64, 64)
    px.fill(Qt.GlobalColor.transparent)

    painter = QPainter(px)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

    if not connected:
        ring_color = QColor("#64748B")
        fill_color = QColor(100, 116, 139, 42)
        badge_color = QColor("#475569")
    elif mode == NoiseMode.ANC:
        ring_color = QColor("#38BDF8")
        fill_color = QColor(56, 189, 248, 55)
        badge_color = QColor("#2563EB")
    elif mode == NoiseMode.TRANSPARENCY:
        ring_color = QColor("#34D399")
        fill_color = QColor(52, 211, 153, 55)
        badge_color = QColor("#059669")
    else:
        ring_color = QColor("#CBD5E1")
        fill_color = QColor(203, 213, 225, 40)
        badge_color = QColor("#64748B")

    painter.setPen(QPen(ring_color, 3.5))
    painter.setBrush(fill_color)
    painter.drawEllipse(QRectF(5, 5, 54, 54))

    fg = QColor("#F8FAFC") if connected else QColor("#94A3B8")
    ear_pen = QPen(fg, 3.2)
    ear_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    painter.setPen(ear_pen)
    painter.setBrush(fg)

    painter.drawEllipse(QRectF(16, 18, 12, 11))
    painter.drawLine(20, 29, 20, 44)
    painter.drawEllipse(QRectF(36, 18, 12, 11))
    painter.drawLine(44, 29, 44, 44)

    painter.setPen(QPen(QColor("#0F172A"), 2.5))
    painter.setBrush(badge_color)
    painter.drawEllipse(QRectF(42, 42, 16, 16))

    if connected and mode is not None:
        painter.setPen(QColor("#FFFFFF"))
        painter.setFont(QFont("Sans", 8, QFont.Weight.Bold))
        letter = {NoiseMode.ANC: "A", NoiseMode.TRANSPARENCY: "T", NoiseMode.OFF: "O"}[mode]
        painter.drawText(QRectF(42, 42, 16, 16), Qt.AlignmentFlag.AlignCenter, letter)

    painter.end()
    return QIcon(px)


def _format_batt_value(reading: BatteryReading) -> str:
    if reading.level is None:
        return "—"
    bolt = " ⚡" if reading.charging else ""
    return f"{reading.level}%{bolt}"


class _ThreadBridge(QObject):
    state_pushed = Signal(object)
    devices_pushed = Signal(object)
    discovered_pushed = Signal(object)
    banner_pushed = Signal(str, int)
    show_pushed = Signal()


class KalmBackend(QObject):
    """Reactive QML ViewModel driving Kirigami FormCard UI and System Tray."""

    stateChanged = Signal()
    devicesChanged = Signal()
    discoveredChanged = Signal()
    bannerChanged = Signal()
    ringingChanged = Signal()
    quittingChanged = Signal()
    windowShowRequested = Signal()

    def __init__(self, session: KalmSession, registry: DeviceRegistry) -> None:
        super().__init__()
        self._session = session
        self._registry = registry
        self._state = session.snapshot
        self._saved_list: List[dict] = []
        self._discovered_list: List[dict] = []
        self._banner_text = ""
        self._banner_type = 0
        self._ringing = False
        self._quitting = False
        self._window: Optional[QWindow] = None

        self._bridge = _ThreadBridge()
        self._bridge.state_pushed.connect(self._apply_state_main)
        self._bridge.devices_pushed.connect(self._apply_devices_main)
        self._bridge.discovered_pushed.connect(self._apply_discovered_main)
        self._bridge.banner_pushed.connect(self._apply_banner_main)
        self._bridge.show_pushed.connect(self._on_show_requested)

        self.refresh_saved_devices_async()

    def bind_window(self, win: QWindow) -> None:
        self._window = win
        win.setIcon(make_kalm_icon(self._state.connected, self._state.noise_mode))

    # ---- Cross-thread callbacks ----

    def push_state_from_worker(self, st: EarbudState) -> None:
        self._bridge.state_pushed.emit(st)

    def push_devices_refresh_from_worker(self) -> None:
        self.refresh_saved_devices_async()

    def push_error_from_worker(self, msg: str) -> None:
        self._bridge.banner_pushed.emit(msg, 2)

    def push_show_from_worker(self) -> None:
        self._bridge.show_pushed.emit()

    def _on_show_requested(self) -> None:
        self.windowShowRequested.emit()

    def _apply_state_main(self, st: EarbudState) -> None:
        self._state = st
        if not st.connected and self._ringing:
            self._ringing = False
            self.ringingChanged.emit()
        if self._window is not None:
            self._window.setIcon(make_kalm_icon(st.connected, st.noise_mode))
        self.stateChanged.emit()

    def _apply_devices_main(self, items: list) -> None:
        self._saved_list = items
        self.devicesChanged.emit()

    def _apply_discovered_main(self, items: list) -> None:
        self._discovered_list = items
        self.discoveredChanged.emit()

    def _apply_banner_main(self, text: str, btype: int) -> None:
        self._banner_text = text
        self._banner_type = btype
        self.bannerChanged.emit()

    # ---- QML Properties ----

    @Property(bool, notify=quittingChanged)
    def quitting(self) -> bool:
        return self._quitting

    @Property(bool, notify=stateChanged)
    def connected(self) -> bool:
        return self._state.connected

    @Property(str, notify=stateChanged)
    def deviceName(self) -> str:
        return self._state.device_name or APP_NAME

    @Property(str, notify=stateChanged)
    def deviceMac(self) -> str:
        return self._state.device_mac or ""

    @Property(str, notify=stateChanged)
    def firmware(self) -> str:
        return self._state.firmware or ""

    @Property(str, notify=stateChanged)
    def statusSummary(self) -> str:
        if self._state.connected:
            parts = ["Подключено"]
            if self._state.firmware:
                parts.append(f"FW {self._state.firmware}")
            if self._state.device_mac:
                parts.append(self._state.device_mac)
            return " · ".join(parts)
        if self._state.device_mac:
            return f"Не подключено · {self._state.device_mac}"
        return "Добавьте или выберите наушники"

    @Property(int, notify=stateChanged)
    def noiseModeIndex(self) -> int:
        if not self._state.connected or self._state.noise_mode is None:
            return -1
        return int(self._state.noise_mode)

    @Property(str, notify=stateChanged)
    def leftBatteryText(self) -> str:
        return _format_batt_value(self._state.left_battery)

    @Property(str, notify=stateChanged)
    def leftBatterySub(self) -> str:
        w = WEAR_LABELS.get(self._state.left_wear or "", "")
        return f"Левый · {w}" if w else "Левый наушник"

    @Property(str, notify=stateChanged)
    def rightBatteryText(self) -> str:
        return _format_batt_value(self._state.right_battery)

    @Property(str, notify=stateChanged)
    def rightBatterySub(self) -> str:
        w = WEAR_LABELS.get(self._state.right_wear or "", "")
        return f"Правый · {w}" if w else "Правый наушник"

    @Property(str, notify=stateChanged)
    def caseBatteryText(self) -> str:
        return _format_batt_value(self._state.case_battery)

    @Property(str, notify=stateChanged)
    def caseBatterySub(self) -> str:
        if self._state.case_battery.level is not None and self._state.case_battery.stale:
            return "Кейс (кэш)"
        return "Зарядный кейс"

    @Property("QVariantList", constant=True)
    def eqModel(self) -> list:
        return EQ_LIST

    @Property(int, notify=stateChanged)
    def eqIndex(self) -> int:
        eff = self._state.audio_effect
        if eff is None:
            return 0
        for idx, item in enumerate(EQ_LIST):
            if item["value"] == eff:
                return idx
        return 0

    @Property("QVariantList", constant=True)
    def cycleModel(self) -> list:
        return CYCLE_LIST

    @Property(int, notify=stateChanged)
    def cycleIndex(self) -> int:
        cyc = self._state.noise_cycle_left
        if cyc is None:
            return 0
        for idx, item in enumerate(CYCLE_LIST):
            if item["value"] == cyc:
                return idx
        return 0

    @Property(bool, notify=stateChanged)
    def gameMode(self) -> bool:
        return bool(self._state.low_latency)

    @Property(bool, notify=stateChanged)
    def wearDetection(self) -> bool:
        return bool(self._state.wear_detection)

    @Property(bool, notify=ringingChanged)
    def isRinging(self) -> bool:
        return self._ringing

    @Property("QVariantList", notify=stateChanged)
    def multipointDevices(self) -> list:
        return [
            {
                "mac": d.mac_str,
                "name": d.name or d.mac_str,
                "statusText": d.status_ru,
                "connected": d.connected,
                "active": d.active,
            }
            for d in self._state.paired_devices
        ]

    @Property("QVariantList", notify=devicesChanged)
    def savedDevices(self) -> list:
        return self._saved_list

    @Property("QVariantList", notify=discoveredChanged)
    def discoveredDevices(self) -> list:
        return self._discovered_list

    @Property(bool, notify=devicesChanged)
    def autoSwitch(self) -> bool:
        return self._registry.auto_switch

    @Property(str, notify=bannerChanged)
    def bannerText(self) -> str:
        return self._banner_text

    @Property(int, notify=bannerChanged)
    def bannerType(self) -> int:
        return self._banner_type

    # ---- Background helpers ----

    def _run_bg(self, fn: Callable[[], object]) -> None:
        def _worker() -> None:
            try:
                fn()
            except Exception as exc:
                self._bridge.banner_pushed.emit(str(exc), 3)

        threading.Thread(target=_worker, daemon=True).start()

    def refresh_saved_devices_async(self) -> None:
        def _load() -> None:
            try:
                bmap = {d.mac: d for d in list_bluez_devices(only_vivo=False)}
            except Exception:
                bmap = {}
            active_mac = self._registry.active_mac
            items = []
            for d in self._registry.devices:
                b = bmap.get(d.mac)
                items.append(
                    {
                        "mac": d.mac,
                        "displayName": d.display_name,
                        "rawName": d.name,
                        "alias": d.alias,
                        "active": d.mac == active_mac,
                        "btConnected": bool(b and b.connected),
                        "btPaired": bool(b and b.paired),
                    }
                )
            self._bridge.devices_pushed.emit(items)

        threading.Thread(target=_load, daemon=True).start()

    # ---- QML Invokable Slots ----

    @Slot(int)
    def setNoiseByIndex(self, index: int) -> None:
        if index not in (0, 1, 2):
            return
        mode = NoiseMode(index)
        # Optimistic UI update
        self._state.noise_mode = mode
        self.stateChanged.emit()
        self._run_bg(lambda: self._session.set_noise_mode(mode))

    @Slot(int)
    def setEqByIndex(self, index: int) -> None:
        if not (0 <= index < len(EQ_LIST)):
            return
        code = EQ_LIST[index]["value"]
        self._run_bg(lambda: self._session.set_audio_effect(code))

    @Slot(int)
    def setCycleByIndex(self, index: int) -> None:
        if not (0 <= index < len(CYCLE_LIST)):
            return
        code = CYCLE_LIST[index]["value"]
        self._run_bg(lambda: self._session.set_noise_cycle(code))

    @Slot(bool)
    def setGameMode(self, enabled: bool) -> None:
        self._run_bg(lambda: self._session.set_low_latency(bool(enabled)))

    @Slot(bool)
    def setWearDetection(self, enabled: bool) -> None:
        self._run_bg(lambda: self._session.set_wear_detection(bool(enabled)))

    @Slot(str, bool)
    def toggleMultipointDevice(self, mac: str, connect: bool) -> None:
        self._run_bg(lambda: self._session.set_device_connection(mac, connect))

    @Slot()
    def startFindEarbuds(self) -> None:
        self._ringing = True
        self.ringingChanged.emit()
        self._run_bg(lambda: self._session.find_earbuds(True))

    @Slot()
    def stopFindEarbuds(self) -> None:
        self._ringing = False
        self.ringingChanged.emit()
        self._run_bg(lambda: self._session.find_earbuds(False))

    @Slot()
    def refreshNow(self) -> None:
        if self._state.connected:
            self._run_bg(self._session.refresh)
        else:
            self._session.force_reconnect()
        self.refresh_saved_devices_async()

    @Slot()
    def scanBluetoothDevices(self) -> None:
        def _scan() -> None:
            try:
                devs = list_bluez_devices(only_audio=True)
            except Exception:
                devs = []
            saved_macs = {d.mac for d in self._registry.devices}
            out = []
            for b in devs:
                parts = [b.mac]
                if b.is_vivo_tws:
                    parts.append("vivo/iQOO GAIA")
                parts.append("Подключено" if b.connected else "Сопряжено")
                if b.mac in saved_macs:
                    parts.append("В списке")
                out.append(
                    {
                        "mac": b.mac,
                        "name": b.name,
                        "isVivo": b.is_vivo_tws,
                        "connected": b.connected,
                        "alreadySaved": b.mac in saved_macs,
                        "subtitle": " · ".join(parts),
                    }
                )
            self._bridge.discovered_pushed.emit(out)

        threading.Thread(target=_scan, daemon=True).start()

    @Slot(str, str, str, result=bool)
    def addSavedDevice(self, mac: str, name: str, alias: str) -> bool:
        try:
            entry = self._registry.add(mac, name=name, alias=alias, make_active=True)
        except ValueError as exc:
            self._apply_banner_main(str(exc), 3)
            return False
        self._session.switch_to_device(entry.mac)
        self.refresh_saved_devices_async()
        self._apply_banner_main(f"Выбраны наушники: {entry.display_name}", 1)
        return True

    @Slot(str)
    def removeSavedDevice(self, mac: str) -> None:
        self._registry.remove(mac)
        self._session.notify_registry_updated()
        self.refresh_saved_devices_async()

    @Slot(str, str)
    def renameSavedDevice(self, mac: str, alias: str) -> None:
        self._registry.rename(mac, alias)
        self._session.notify_registry_updated()
        self.refresh_saved_devices_async()

    @Slot(str)
    def selectSavedDevice(self, mac: str) -> None:
        self._session.switch_to_device(mac)
        self.refresh_saved_devices_async()

    @Slot(str)
    def connectBluetoothDevice(self, mac: str) -> None:
        self._apply_banner_main(f"Подключение по Bluetooth к {mac}…", 0)

        def _do() -> None:
            ok = connect_bluez_device(mac)
            if ok:
                self._session.force_reconnect()
                self._bridge.banner_pushed.emit("Bluetooth-соединение установлено", 1)
            else:
                self._bridge.banner_pushed.emit(
                    "Не удалось подключиться по Bluetooth. Проверьте, открыт ли кейс.", 2
                )
            self.refresh_saved_devices_async()

        threading.Thread(target=_do, daemon=True).start()

    @Slot(bool)
    def setAutoSwitch(self, enabled: bool) -> None:
        self._registry.set_auto_switch(bool(enabled))
        self.devicesChanged.emit()

    @Slot()
    def openSystemBluetoothSettings(self) -> None:
        for cmd in (
            ["systemsettings", "kcm_bluetooth"],
            ["kcmshell6", "kcm_bluetooth"],
            ["bluedevil-wizard"],
        ):
            if shutil.which(cmd[0]):
                subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return

    @Slot()
    def clearBanner(self) -> None:
        self._banner_text = ""
        self.bannerChanged.emit()

    @Slot()
    def quitApp(self) -> None:
        self._quitting = True
        self.quittingChanged.emit()
        app = QApplication.instance()
        if app is not None:
            app.quit()


class KalmTrayController(QObject):
    """KDE Plasma StatusNotifierItem / QSystemTrayIcon controller."""

    def __init__(self, session: KalmSession, backend: KalmBackend) -> None:
        super().__init__()
        self._session = session
        self._backend = backend
        self._window: Optional[QWindow] = None

        self.tray = QSystemTrayIcon(make_kalm_icon(False, None), self)
        self.tray.setToolTip(f"{APP_NAME} — Ожидание подключения")

        self.menu = QMenu()
        self.header_act = QAction(APP_NAME, self.menu)
        self.header_act.setEnabled(False)
        self.menu.addAction(self.header_act)

        self.batt_act = QAction("Заряд: —", self.menu)
        self.batt_act.setEnabled(False)
        self.menu.addAction(self.batt_act)
        self.menu.addSeparator()

        # Noise Modes
        mode_group = QActionGroup(self.menu)
        mode_group.setExclusive(True)
        self.act_anc = QAction("Шумоподавление (ANC)", self.menu, checkable=True)
        self.act_trans = QAction("Прозрачность", self.menu, checkable=True)
        self.act_off = QAction("Выключено", self.menu, checkable=True)
        for act, idx in ((self.act_anc, 0), (self.act_trans, 2), (self.act_off, 1)):
            mode_group.addAction(act)
            self.menu.addAction(act)
            act.triggered.connect(lambda _c=False, i=idx: self._backend.setNoiseByIndex(i))

        self.menu.addSeparator()

        # Saved Earbuds Submenu
        self.devs_menu = self.menu.addMenu("Мои наушники")
        self.devs_group = QActionGroup(self.devs_menu)
        self.devs_group.setExclusive(True)

        # EQ Submenu
        eq_menu = self.menu.addMenu("Звук (DeepX)")
        eq_group = QActionGroup(eq_menu)
        eq_group.setExclusive(True)
        self.eq_actions = {}
        for idx, item in enumerate(EQ_LIST):
            act = QAction(item["text"], eq_menu, checkable=True)
            eq_group.addAction(act)
            eq_menu.addAction(act)
            act.triggered.connect(lambda _c=False, i=idx: self._backend.setEqByIndex(i))
            self.eq_actions[item["value"]] = act

        self.game_act = QAction("Игровой режим", self.menu, checkable=True)
        self.game_act.triggered.connect(lambda checked: self._backend.setGameMode(bool(checked)))
        self.menu.addAction(self.game_act)

        self.menu.addSeparator()
        open_act = QAction(f"Открыть {APP_NAME}", self.menu)
        open_act.triggered.connect(self._show_window)
        self.menu.addAction(open_act)

        quit_act = QAction("Выход", self.menu)
        quit_act.triggered.connect(self._backend.quitApp)
        self.menu.addAction(quit_act)

        self.tray.setContextMenu(self.menu)
        self.tray.activated.connect(self._on_activated)

        self._backend.stateChanged.connect(self._sync_state)
        self._backend.devicesChanged.connect(self._sync_devices_menu)
        self.tray.show()
        self._sync_state()
        self._sync_devices_menu()

    def bind_window(self, win: QWindow) -> None:
        self._window = win

    def hide(self) -> None:
        self.tray.hide()

    def _show_window(self) -> None:
        self._backend.windowShowRequested.emit()

    def _on_activated(self, reason: QSystemTrayIcon.ActivationReason) -> None:
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            if self._window is not None and self._window.isVisible():
                self._window.setVisible(False)
            else:
                self._show_window()
        elif reason == QSystemTrayIcon.ActivationReason.MiddleClick:
            threading.Thread(target=self._session.toggle_noise_mode, daemon=True).start()

    def _sync_devices_menu(self) -> None:
        self.devs_menu.clear()
        for act in list(self.devs_group.actions()):
            self.devs_group.removeAction(act)

        items = self._backend.savedDevices
        if not items:
            empty = QAction("Нет сохранённых наушников", self.devs_menu)
            empty.setEnabled(False)
            self.devs_menu.addAction(empty)
            return

        for d in items:
            suffix = " (онлайн)" if d["btConnected"] else ""
            act = QAction(f"{d['displayName']}{suffix}", self.devs_menu, checkable=True)
            act.setChecked(bool(d["active"]))
            mac = d["mac"]
            act.triggered.connect(lambda _c=False, m=mac: self._backend.selectSavedDevice(m))
            self.devs_group.addAction(act)
            self.devs_menu.addAction(act)

    def _sync_state(self) -> None:
        st = self._session.snapshot
        self.tray.setIcon(make_kalm_icon(st.connected, st.noise_mode))

        name = st.device_name or APP_NAME
        if not st.connected:
            self.tray.setToolTip(f"{name} — Не подключено")
            self.header_act.setText(f"{name} — Не подключено")
            self.batt_act.setText("Заряд: —")
            for a in (self.act_anc, self.act_trans, self.act_off, self.game_act):
                a.setEnabled(False)
            return

        mode_txt = st.noise_mode.label_ru if st.noise_mode is not None else "—"
        b_parts = []
        if st.left_battery.level is not None:
            b_parts.append(f"Л: {st.left_battery.level}%")
        if st.right_battery.level is not None:
            b_parts.append(f"П: {st.right_battery.level}%")
        if st.case_battery.level is not None and not st.case_battery.stale:
            b_parts.append(f"Кейс: {st.case_battery.level}%")
        batt_str = "  ".join(b_parts) if b_parts else "—"

        self.tray.setToolTip(f"{name}\nРежим: {mode_txt}\n{batt_str}")
        self.header_act.setText(f"{name} ({mode_txt})")
        self.batt_act.setText(f"Заряд: {batt_str}")

        for a in (self.act_anc, self.act_trans, self.act_off, self.game_act):
            a.setEnabled(True)

        if st.noise_mode == NoiseMode.ANC:
            self.act_anc.setChecked(True)
        elif st.noise_mode == NoiseMode.OFF:
            self.act_off.setChecked(True)
        elif st.noise_mode == NoiseMode.TRANSPARENCY:
            self.act_trans.setChecked(True)

        if st.audio_effect in self.eq_actions:
            self.eq_actions[st.audio_effect].setChecked(True)
        if st.low_latency is not None:
            self.game_act.setChecked(st.low_latency)


def launch_gui(target_device: Optional[str] = None, start_minimized: bool = False) -> int:
    if not target_device:
        existing = ipc_request({"cmd": "show"}, timeout=0.5)
        if existing and existing.get("ok"):
            return 0

    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setApplicationDisplayName(APP_NAME)
    app.setDesktopFileName(APP_ID)
    app.setQuitOnLastWindowClosed(False)
    app.setWindowIcon(make_kalm_icon(False, None))

    registry = DeviceRegistry()
    registry.ensure_initial_import()

    override_mac: Optional[str] = None
    if target_device:
        resolved = registry.resolve_query(target_device)
        if resolved:
            registry.set_active(resolved.mac)
            override_mac = resolved.mac

    holder: dict = {}

    session = KalmSession(
        registry=registry,
        override_mac=override_mac,
        on_state_changed=lambda st: holder["backend"].push_state_from_worker(st)
        if "backend" in holder
        else None,
        on_devices_changed=lambda: holder["backend"].push_devices_refresh_from_worker()
        if "backend" in holder
        else None,
        on_error=lambda err: holder["backend"].push_error_from_worker(err)
        if "backend" in holder
        else None,
    )

    backend = KalmBackend(session, registry)
    holder["backend"] = backend
    tray = KalmTrayController(session, backend)

    ipc = IpcServer(session, on_show_window=backend.push_show_from_worker)
    ipc.start()
    session.start()

    # Load Kirigami QML asynchronously inside app.exec() so Python GIL is freely released for QQmlThread
    engine = QQmlEngine()
    engine.rootContext().setContextProperty("backend", backend)
    qml_path = Path(__file__).resolve().parent / "qml" / "Main.qml"
    component = QQmlComponent(engine)

    def _on_qml_status(status: QQmlComponent.Status) -> None:
        if status == QQmlComponent.Status.Ready:
            win = component.create()
            if isinstance(win, QWindow):
                holder["window"] = win
                backend.bind_window(win)
                tray.bind_window(win)
                win.setVisible(not start_minimized)
        elif status == QQmlComponent.Status.Error:
            print("Ошибка загрузки QML Kirigami:", component.errorString(), file=sys.stderr)
            app.quit()

    component.statusChanged.connect(_on_qml_status)
    component.loadUrl(QUrl.fromLocalFile(str(qml_path)), QQmlComponent.CompilationMode.Asynchronous)

    # Periodic refresh of saved devices' Bluetooth status
    bt_timer = QTimer()
    bt_timer.setInterval(6000)
    bt_timer.timeout.connect(backend.refresh_saved_devices_async)
    bt_timer.start()

    # Signal handling for Ctrl+C / SIGTERM
    sig_timer = QTimer()
    sig_timer.start(400)
    sig_timer.timeout.connect(lambda: None)
    signal.signal(signal.SIGINT, lambda *_: backend.quitApp())
    signal.signal(signal.SIGTERM, lambda *_: backend.quitApp())

    code = app.exec()
    tray.hide()
    ipc.stop()
    session.stop()
    holder.clear()
    del component
    del engine
    return code
