"""Command-line interface for Kalm."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from typing import Dict, List, Optional

from . import APP_NAME
from .client import (
    ipc_request,
    oneshot_set_eq,
    oneshot_set_game,
    oneshot_set_noise,
    oneshot_status,
    oneshot_toggle_noise,
)
from .protocol import AUDIO_EFFECT_PRESETS, NoiseMode
from .registry import DeviceRegistry
from .transport import find_bluez_by_mac, list_bluez_devices

EQ_ALIASES: Dict[str, int] = {
    "0": 0x00,
    "standard": 0x00,
    "стандартный": 0x00,
    "1": 0x01,
    "vocal": 0x01,
    "voice": 0x01,
    "вокал": 0x01,
    "2": 0x02,
    "bass": 0x02,
    "бас": 0x02,
    "3": 0x03,
    "treble": 0x03,
    "highs": 0x03,
    "высокие": 0x03,
    "5": 0x05,
    "audiobook": 0x05,
    "книги": 0x05,
    "11": 0x0B,
    "master": 0x0B,
    "hifi": 0x0B,
    "мастер": 0x0B,
}

WEAR_RU = {
    "worn": "в ухе",
    "in_case": "в кейсе",
    "removed": "снят",
}


def _notify_desktop(title: str, body: str) -> None:
    if not shutil.which("notify-send"):
        return
    try:
        subprocess.Popen(
            [
                "notify-send",
                "-a",
                APP_NAME,
                "-i",
                "audio-headset",
                "-h",
                "string:x-canonical-private-synchronous:kalm",
                title,
                body,
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        pass


def _format_batt(level: Optional[int], charging: bool, stale: bool) -> str:
    if level is None:
        return "—"
    suffix = " ⚡" if charging else (" (устар.)" if stale else "")
    return f"{level}%{suffix}"


def _print_status(data: dict) -> None:
    if not data.get("connected"):
        print("Статус: Не подключено")
        return

    name = data.get("device_name") or "Наушники"
    mac = data.get("device_mac") or ""
    fw = data.get("firmware") or "—"
    mode = data.get("noise_mode_label") or "—"
    batt = data.get("battery", {})
    wear = data.get("wear", {})

    left_b = _format_batt(batt.get("left"), bool(batt.get("left_charging")), bool(batt.get("left_stale")))
    right_b = _format_batt(batt.get("right"), bool(batt.get("right_charging")), bool(batt.get("right_stale")))
    case_b = _format_batt(batt.get("case"), bool(batt.get("case_charging")), bool(batt.get("case_stale")))

    lw = WEAR_RU.get(wear.get("left"), "—")
    rw = WEAR_RU.get(wear.get("right"), "—")

    print(f"Наушники:        {name} ({mac})")
    print(f"Прошивка:        {fw}")
    print(f"Шумоподавление:  {mode}")
    print(f"Заряд:           Л: {left_b}   П: {right_b}   Кейс: {case_b}")
    print(f"Положение:       Л: {lw}   П: {rw}")

    eq_label = data.get("audio_effect_label")
    if eq_label:
        print(f"Звук (DeepX):    {eq_label}")

    ll = data.get("low_latency")
    if ll is not None:
        print(f"Игровой режим:   {'Включён' if ll else 'Выключен'}")

    nc = data.get("noise_cycle", {}).get("label")
    if nc:
        print(f"Долгое нажатие:  {nc}")

    devs = data.get("paired_devices") or []
    if devs:
        print("Мультипоинт:")
        for d in devs:
            marker = "●" if d.get("connected") else "○"
            print(f"  {marker} {d['name']} ({d['mac']}) — {d['status_label']}")


def _batt_summary_short(data: dict) -> str:
    b = data.get("battery", {})
    parts = []
    if b.get("left") is not None:
        parts.append(f"Л {b['left']}%")
    if b.get("right") is not None:
        parts.append(f"П {b['right']}%")
    if b.get("case") is not None and not b.get("case_stale"):
        parts.append(f"Кейс {b['case']}%")
    return " · ".join(parts)


def _handle_devices_cmd(args: argparse.Namespace) -> int:
    reg = DeviceRegistry()
    reg.ensure_initial_import()
    action = args.dev_action or "list"

    if action == "list":
        devs = reg.devices
        if not devs:
            print("Сохранённых наушников пока нет. Используйте: kalm devices scan / kalm devices add <MAC>")
            return 0
        bluez = {d.mac: d for d in list_bluez_devices(only_vivo=False)}
        active_mac = reg.active_mac
        print("Сохранённые наушники:")
        for d in devs:
            mark = "★" if d.mac == active_mac else " "
            b = bluez.get(d.mac)
            bt_status = "Подключено" if (b and b.connected) else ("Сопряжено" if b else "Не найдено в BT")
            alias_note = f" [{d.name}]" if d.alias and d.alias != d.name else ""
            print(f"  {mark} {d.display_name}{alias_note} — {d.mac} ({bt_status})")
        return 0

    if action == "scan":
        found = list_bluez_devices(only_audio=True)
        if not found:
            print("Аудиоустройства Bluetooth не найдены.")
            return 0
        saved_macs = {d.mac for d in reg.devices}
        print("Доступные аудиоустройства Bluetooth:")
        for b in found:
            tag = "vivo/iQOO GAIA" if b.is_vivo_tws else "Bluetooth Audio"
            conn = "Подключено" if b.connected else "Не подключено"
            saved = " [уже добавлено]" if b.mac in saved_macs else ""
            print(f"  • {b.name} — {b.mac} ({tag}, {conn}){saved}")
        return 0

    if action == "add":
        bdev = find_bluez_by_mac(args.mac)
        name = bdev.name if bdev else args.mac.upper()
        entry = reg.add(args.mac, name=name, alias=args.alias or "", make_active=True)
        ipc_request({"cmd": "reload_registry"})
        print(f"Добавлены и выбраны наушники: {entry.display_name} ({entry.mac})")
        return 0

    if action == "remove":
        target = reg.resolve_query(args.target)
        if not target or not reg.remove(target.mac):
            raise RuntimeError(f"Наушники {args.target!r} не найдены в списке.")
        ipc_request({"cmd": "reload_registry"})
        print(f"Удалено: {target.display_name} ({target.mac})")
        return 0

    if action == "use":
        target = reg.resolve_query(args.target)
        if not target or not reg.set_active(target.mac):
            raise RuntimeError(f"Наушники {args.target!r} не найдены в списке.")
        ipc_request({"cmd": "select_device", "mac": target.mac})
        print(f"Активные наушники: {target.display_name} ({target.mac})")
        return 0

    if action == "rename":
        target = reg.resolve_query(args.target)
        if not target or not reg.rename(target.mac, args.alias):
            raise RuntimeError(f"Наушники {args.target!r} не найдены в списке.")
        ipc_request({"cmd": "reload_registry"})
        print(f"Переименовано: {args.alias or target.name} ({target.mac})")
        return 0

    return 0


def run_cli(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="kalm",
        description="Kalm — управление шумоподавлением и настройками наушников vivo / iQOO TWS",
    )
    parser.add_argument("-d", "--device", "--mac", dest="device", help="MAC-адрес или имя наушников")
    parser.add_argument("--minimized", action="store_true", help="Запустить GUI свёрнутым в системный трей")

    sub = parser.add_subparsers(dest="subcmd")

    sub.add_parser("gui", help="Открыть графическое приложение Kirigami (по умолчанию)")

    p_status = sub.add_parser("status", help="Показать текущий режим, заряд и статус")
    p_status.add_argument("--json", action="store_true", help="Вывести в формате JSON")

    for cmd_name, desc in (
        ("anc", "Включить активное шумоподавление (ANC)"),
        ("trans", "Включить режим прозрачности"),
        ("off", "Выключить шумоподавление"),
    ):
        p = sub.add_parser(cmd_name, help=desc)
        p.add_argument("--notify", action="store_true", help="Показать системное уведомление")

    p_set = sub.add_parser("mode", help="Установить режим (anc | trans | off)")
    p_set.add_argument("target", help="anc | trans | off")
    p_set.add_argument("--notify", action="store_true", help="Показать системное уведомление")

    p_toggle = sub.add_parser("toggle", help="Циклически переключить режим шумоподавления")
    p_toggle.add_argument(
        "--modes",
        default="anc,trans,off",
        help="Список режимов через запятую (например: anc,trans или anc,trans,off)",
    )
    p_toggle.add_argument("--notify", action="store_true", help="Показать системное уведомление")

    p_eq = sub.add_parser("eq", help="Выбрать пресет звука DeepX / Hi-Fi")
    p_eq.add_argument("preset", help="standard | vocal | bass | treble | audiobook | master")

    p_game = sub.add_parser("game", help="Игровой режим с низкой задержкой (on | off)")
    p_game.add_argument("state", choices=["on", "off"], help="on или off")

    p_devs = sub.add_parser("devices", help="Управление списком наушников")
    dev_sub = p_devs.add_subparsers(dest="dev_action")
    dev_sub.add_parser("list", help="Показать сохранённые наушники")
    dev_sub.add_parser("scan", help="Найти аудиоустройства Bluetooth")
    p_add = dev_sub.add_parser("add", help="Добавить наушники по MAC-адресу")
    p_add.add_argument("mac", help="MAC-адрес XX:XX:XX:XX:XX:XX")
    p_add.add_argument("--alias", default="", help="Своё название (необязательно)")
    p_rm = dev_sub.add_parser("remove", help="Удалить наушники из списка")
    p_rm.add_argument("target", help="MAC-адрес или название")
    p_use = dev_sub.add_parser("use", help="Сделать наушники активными")
    p_use.add_argument("target", help="MAC-адрес или название")
    p_ren = dev_sub.add_parser("rename", help="Задать своё название наушникам")
    p_ren.add_argument("target", help="MAC-адрес или текущее название")
    p_ren.add_argument("alias", help="Новое название (пустая строка — сброс)")

    args = parser.parse_args(argv)

    if args.subcmd in (None, "gui"):
        from .gui import launch_gui

        return launch_gui(target_device=args.device, start_minimized=args.minimized)

    try:
        if args.subcmd == "devices":
            return _handle_devices_cmd(args)

        if args.subcmd == "status":
            ipc = ipc_request({"cmd": "status"}) if not args.device else None
            state = (
                ipc["state"]
                if (ipc and ipc.get("ok") and ipc["state"].get("connected"))
                else oneshot_status(args.device)
            )
            if args.json:
                print(json.dumps(state, ensure_ascii=False, indent=2))
            else:
                _print_status(state)
            return 0

        if args.subcmd in ("anc", "trans", "off", "mode"):
            raw_target = args.target if args.subcmd == "mode" else args.subcmd
            mode = NoiseMode.from_str(raw_target)
            ipc = ipc_request({"cmd": "set_noise", "mode": mode.slug}) if not args.device else None
            if ipc is not None:
                if not ipc.get("ok"):
                    raise RuntimeError(ipc.get("error") or "Ошибка переключения режима")
                state = ipc["state"]
            else:
                state = oneshot_set_noise(mode, device_query=args.device)

            label = state.get("noise_mode_label") or mode.label_ru
            print(f"Режим установлен: {label}")
            if getattr(args, "notify", False):
                _notify_desktop(f"Режим: {label}", _batt_summary_short(state) or state.get("device_name", APP_NAME))
            return 0

        if args.subcmd == "toggle":
            modes_list = [NoiseMode.from_str(x) for x in args.modes.split(",") if x.strip()]
            if len(modes_list) < 2:
                raise ValueError("Укажите минимум 2 режима для toggle (например --modes anc,trans)")
            ipc = (
                ipc_request({"cmd": "toggle_noise", "modes": [m.slug for m in modes_list]})
                if not args.device
                else None
            )
            if ipc is not None:
                if not ipc.get("ok"):
                    raise RuntimeError(ipc.get("error") or "Ошибка переключения режима")
                state = ipc["state"]
            else:
                state = oneshot_toggle_noise(modes_list, device_query=args.device)

            label = state.get("noise_mode_label") or "—"
            print(f"Режим переключён: {label}")
            if args.notify:
                _notify_desktop(f"Режим: {label}", _batt_summary_short(state) or state.get("device_name", APP_NAME))
            return 0

        if args.subcmd == "eq":
            key = args.preset.strip().lower()
            if key not in EQ_ALIASES:
                raise ValueError(
                    f"Неизвестный пресет {args.preset!r}. Доступны: standard, vocal, bass, treble, audiobook, master"
                )
            eff = EQ_ALIASES[key]
            ipc = ipc_request({"cmd": "set_eq", "effect": eff}) if not args.device else None
            if ipc is not None and ipc.get("ok"):
                state = ipc["state"]
            else:
                state = oneshot_set_eq(eff, device_query=args.device)
            print(f"Эквалайзер: {AUDIO_EFFECT_PRESETS.get(eff, str(eff))}")
            return 0

        if args.subcmd == "game":
            en = args.state == "on"
            ipc = ipc_request({"cmd": "set_game", "enabled": en}) if not args.device else None
            if ipc is not None and ipc.get("ok"):
                state = ipc["state"]
            else:
                state = oneshot_set_game(en, device_query=args.device)
            print(f"Игровой режим: {'Включён' if en else 'Выключен'}")
            return 0

    except Exception as exc:
        print(f"Ошибка: {exc}", file=sys.stderr)
        return 1

    return 0
