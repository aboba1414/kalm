"""Multi-device session manager, Unix IPC server/client, and oneshot CLI helper."""

from __future__ import annotations

import copy
import json
import os
import socket
import threading
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

from .protocol import (
    CMD_ACK_AUDIO_EFFECT,
    CMD_ACK_FIND_DEVICE,
    CMD_ACK_LOW_LATENCY,
    CMD_ACK_NOISE_CYCLE,
    CMD_ACK_NOISE_MODE,
    CMD_ACK_WEAR_DETECTION,
    CMD_REPORT_AUDIO_EFFECT,
    CMD_REPORT_BATTERY,
    CMD_REPORT_LOW_LATENCY,
    CMD_REPORT_MULTI_CONNECT,
    CMD_REPORT_NOISE_CYCLE,
    CMD_REPORT_NOISE_MODE,
    CMD_REPORT_WEAR_DETECTION,
    CMD_REPORT_WEAR_STATE,
    EarbudState,
    Frame,
    FrameDecoder,
    NoiseMode,
    PROFILE_FAMILY_V4,
    WireProfile,
    apply_frame_to_state,
    build_find_device,
    build_host_time_response,
    build_query_audio_effect,
    build_query_battery,
    build_query_low_latency,
    build_query_multi_connect,
    build_query_noise_cycle,
    build_query_noise_mode,
    build_query_wear_detection,
    build_query_wear_state,
    build_register_notifications,
    build_set_audio_effect,
    build_set_low_latency,
    build_set_multi_connect,
    build_set_noise_cycle,
    build_set_noise_mode,
    build_set_wear_detection,
    select_profile,
)
from .registry import DeviceRegistry, SavedEarbud
from .transport import BluezDevice, find_bluez_by_mac, list_bluez_devices, open_gaia_rfcomm


def ipc_socket_path() -> Path:
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR") or f"/tmp/kalm-{os.getuid()}"
    p = Path(runtime_dir)
    p.mkdir(parents=True, exist_ok=True)
    return p / "kalm.sock"


class KalmSession:
    """Thread-safe persistent connection supporting multiple saved earbuds."""

    def __init__(
        self,
        registry: DeviceRegistry,
        override_mac: Optional[str] = None,
        on_state_changed: Optional[Callable[[EarbudState], None]] = None,
        on_devices_changed: Optional[Callable[[], None]] = None,
        on_error: Optional[Callable[[str], None]] = None,
    ) -> None:
        self.registry = registry
        self._override_mac = override_mac.upper() if override_mac else None
        self._on_state_changed = on_state_changed
        self._on_devices_changed = on_devices_changed
        self._on_error = on_error

        self._lock = threading.RLock()
        self._sock: Optional[socket.socket] = None
        self._profile: WireProfile = PROFILE_FAMILY_V4
        self._state = EarbudState()

        self._stop_event = threading.Event()
        self._wake_event = threading.Event()
        self._worker: Optional[threading.Thread] = None

        self._waiters_lock = threading.Lock()
        self._waiters: List[tuple[Callable[[Frame], bool], threading.Event, list]] = []

    @property
    def snapshot(self) -> EarbudState:
        with self._lock:
            return copy.deepcopy(self._state)

    def start(self) -> None:
        self.registry.ensure_initial_import()
        active = self.registry.get_active()
        if active:
            with self._lock:
                self._state.device_name = active.display_name
                self._state.device_mac = active.mac
        if self._worker and self._worker.is_alive():
            return
        self._stop_event.clear()
        self._worker = threading.Thread(target=self._run_loop, name="KalmSessionWorker", daemon=True)
        self._worker.start()

    def stop(self) -> None:
        self._stop_event.set()
        self._wake_event.set()
        self._close_socket()
        if self._worker and self._worker.is_alive():
            self._worker.join(timeout=2.0)

    def switch_to_device(self, mac: str) -> bool:
        if not self.registry.set_active(mac):
            return False
        self._override_mac = None
        active = self.registry.get_active()
        self._close_socket(reset_identity=True)
        if active:
            with self._lock:
                self._state.device_name = active.display_name
                self._state.device_mac = active.mac
        self._emit_devices()
        self._emit_state()
        self._wake_event.set()
        return True

    def notify_registry_updated(self) -> None:
        active = self.registry.get_active()
        with self._lock:
            cur_mac = self._state.device_mac
            if active is None:
                need_drop = bool(cur_mac)
            elif cur_mac and active.mac != cur_mac:
                need_drop = True
            else:
                self._state.device_name = active.display_name
                need_drop = False
        if need_drop:
            self._close_socket(reset_identity=True)
            if active:
                with self._lock:
                    self._state.device_name = active.display_name
                    self._state.device_mac = active.mac
        self._emit_devices()
        self._emit_state()
        self._wake_event.set()

    def force_reconnect(self) -> None:
        self._close_socket()
        self._wake_event.set()

    def _emit_state(self) -> None:
        cb = self._on_state_changed
        if cb:
            try:
                cb(self.snapshot)
            except Exception:
                pass

    def _emit_devices(self) -> None:
        cb = self._on_devices_changed
        if cb:
            try:
                cb()
            except Exception:
                pass

    def _emit_error(self, msg: str) -> None:
        cb = self._on_error
        if cb:
            try:
                cb(msg)
            except Exception:
                pass

    def _close_socket(self, reset_identity: bool = False) -> None:
        with self._lock:
            sock = self._sock
            self._sock = None
            was_connected = self._state.connected
            if reset_identity:
                self._state = EarbudState()
            else:
                self._state.connected = False
                self._state.left_battery.stale = True
                self._state.right_battery.stale = True
                self._state.case_battery.stale = True
        if sock is not None:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
        if was_connected or reset_identity:
            self._emit_state()

    def _send_raw(self, data: bytes) -> bool:
        with self._lock:
            sock = self._sock
        if sock is None:
            return False
        try:
            sock.sendall(data)
            return True
        except OSError:
            self._close_socket()
            return False

    def _send_and_wait(
        self,
        packet: bytes,
        predicate: Callable[[Frame], bool],
        timeout: float = 1.8,
    ) -> Optional[Frame]:
        ev = threading.Event()
        holder: list = []
        entry = (predicate, ev, holder)
        with self._waiters_lock:
            self._waiters.append(entry)
        try:
            if not self._send_raw(packet):
                return None
            if ev.wait(timeout):
                return holder[0] if holder else None
            return None
        finally:
            with self._waiters_lock:
                if entry in self._waiters:
                    self._waiters.remove(entry)

    # ---- Public Controls ----

    def refresh(self) -> bool:
        p = self._profile
        ok = False
        for pkt in (
            build_query_battery(),
            build_query_noise_mode(p),
            build_query_wear_state(p),
            build_query_wear_detection(p),
            build_query_audio_effect(p),
            build_query_low_latency(p),
            build_query_noise_cycle(p),
            build_query_multi_connect(p),
        ):
            ok = self._send_raw(pkt) or ok
        return ok

    def set_noise_mode(self, mode: NoiseMode) -> bool:
        pkt = build_set_noise_mode(mode, self._profile)
        resp = self._send_and_wait(
            pkt,
            lambda f: f.command in (CMD_ACK_NOISE_MODE, CMD_REPORT_NOISE_MODE)
            and len(f.payload) >= 2
            and f.payload[0] == 0
            and f.payload[1] == int(mode),
            timeout=2.0,
        )
        if resp is not None:
            return True
        q = self._send_and_wait(
            build_query_noise_mode(self._profile),
            lambda f: f.command == CMD_REPORT_NOISE_MODE,
            timeout=1.5,
        )
        return bool(q and len(q.payload) >= 2 and q.payload[1] == int(mode))

    def toggle_noise_mode(self, cycle: Optional[List[NoiseMode]] = None) -> Optional[NoiseMode]:
        order = cycle or [NoiseMode.ANC, NoiseMode.TRANSPARENCY, NoiseMode.OFF]
        cur = self.snapshot.noise_mode
        nxt = order[(order.index(cur) + 1) % len(order)] if cur in order else order[0]
        if self.set_noise_mode(nxt):
            return nxt
        return None

    def set_audio_effect(self, effect_id: int) -> bool:
        resp = self._send_and_wait(
            build_set_audio_effect(effect_id, self._profile),
            lambda f: f.command in (CMD_ACK_AUDIO_EFFECT, CMD_REPORT_AUDIO_EFFECT)
            and len(f.payload) >= 2
            and f.payload[0] == 0,
            timeout=1.8,
        )
        return resp is not None

    def set_low_latency(self, enabled: bool) -> bool:
        resp = self._send_and_wait(
            build_set_low_latency(enabled, self._profile),
            lambda f: f.command in (CMD_ACK_LOW_LATENCY, CMD_REPORT_LOW_LATENCY)
            and len(f.payload) >= 2
            and f.payload[0] == 0,
            timeout=1.8,
        )
        return resp is not None

    def set_wear_detection(self, enabled: bool) -> bool:
        resp = self._send_and_wait(
            build_set_wear_detection(enabled, self._profile),
            lambda f: f.command in (CMD_ACK_WEAR_DETECTION, CMD_REPORT_WEAR_DETECTION)
            and len(f.payload) >= 2
            and f.payload[0] == 0,
            timeout=1.8,
        )
        return resp is not None

    def set_noise_cycle(self, cycle_code: int) -> bool:
        resp = self._send_and_wait(
            build_set_noise_cycle(cycle_code, cycle_code, self._profile),
            lambda f: f.command in (CMD_ACK_NOISE_CYCLE, CMD_REPORT_NOISE_CYCLE)
            and len(f.payload) >= 4
            and f.payload[0] == 0,
            timeout=1.8,
        )
        return resp is not None

    def set_device_connection(self, target_mac: str, connect: bool) -> bool:
        snap = self.snapshot
        if not snap.paired_devices:
            return False
        pkt = build_set_multi_connect(snap.paired_devices, target_mac, connect, self._profile)
        if not self._send_raw(pkt):
            return False
        time.sleep(0.4)
        self._send_raw(build_query_multi_connect(self._profile))
        return True

    def find_earbuds(self, ring: bool) -> bool:
        resp = self._send_and_wait(
            build_find_device(ring, self._profile),
            lambda f: f.command == CMD_ACK_FIND_DEVICE,
            timeout=1.8,
        )
        return resp is not None

    # ---- Device Resolution & Worker ----

    def _pick_target(self) -> Optional[tuple[SavedEarbud, Optional[BluezDevice]]]:
        try:
            bluez_map: Dict[str, BluezDevice] = {
                d.mac: d for d in list_bluez_devices(only_vivo=False)
            }
        except Exception:
            bluez_map = {}

        if self._override_mac:
            bdev = bluez_map.get(self._override_mac)
            saved = self.registry.get(self._override_mac) or SavedEarbud(
                mac=self._override_mac,
                name=bdev.name if bdev else self._override_mac,
            )
            return saved, bdev

        active = self.registry.get_active()
        if active:
            bdev = bluez_map.get(active.mac)
            if bdev and bdev.connected:
                return active, bdev
            # If active is offline and auto_switch is enabled, check other saved earbuds
            if self.registry.auto_switch:
                for cand in self.registry.devices:
                    cb = bluez_map.get(cand.mac)
                    if cb and cb.connected:
                        self.registry.set_active(cand.mac)
                        self._emit_devices()
                        return cand, cb
            return active, bdev

        # No saved earbuds at all — auto-detect connected vivo TWS if present
        for bdev in bluez_map.values():
            if bdev.is_vivo_tws and bdev.connected:
                added = self.registry.add(bdev.mac, bdev.name, make_active=True)
                self._emit_devices()
                return added, bdev
        return None

    def _run_loop(self) -> None:
        while not self._stop_event.is_set():
            picked = self._pick_target()
            if picked is None:
                self._wake_event.wait(3.0)
                self._wake_event.clear()
                continue

            saved, bdev = picked
            with self._lock:
                self._state.device_name = saved.display_name
                self._state.device_mac = saved.mac

            if bdev is None or not bdev.connected:
                self._wake_event.wait(2.5)
                self._wake_event.clear()
                continue

            # Update underlying official Bluetooth name if changed
            if bdev.name and bdev.name != saved.name:
                self.registry.add(saved.mac, name=bdev.name, make_active=False)

            try:
                sock, initial_bytes = open_gaia_rfcomm(
                    saved.mac,
                    dev_path=bdev.path,
                    name=saved.display_name,
                    timeout=2.5,
                )
            except Exception as exc:
                self._emit_error(str(exc))
                self._wake_event.wait(3.5)
                self._wake_event.clear()
                continue

            profile = select_profile(bdev.name)
            with self._lock:
                self._sock = sock
                self._profile = profile
                self._state.connected = True
                self._state.device_name = saved.display_name
                self._state.device_mac = saved.mac

            decoder = FrameDecoder()
            if initial_bytes:
                self._handle_frames(decoder.feed(initial_bytes))

            for reg_pkt in build_register_notifications():
                self._send_raw(reg_pkt)
            self.refresh()
            self._emit_devices()
            self._emit_state()

            last_poll = time.monotonic()
            sock.settimeout(1.0)
            while not self._stop_event.is_set():
                with self._lock:
                    if self._sock is not sock:
                        break
                try:
                    chunk = sock.recv(2048)
                    if not chunk:
                        break
                    frames = decoder.feed(chunk)
                    if frames:
                        self._handle_frames(frames)
                except socket.timeout:
                    pass
                except OSError:
                    break

                now = time.monotonic()
                if now - last_poll >= 30.0:
                    last_poll = now
                    self._send_raw(build_query_battery())
                    self._send_raw(build_query_wear_state(self._profile))

            self._close_socket()
            self._emit_devices()
            if not self._stop_event.is_set():
                self._wake_event.wait(2.0)
                self._wake_event.clear()

    def _handle_frames(self, frames: List[Frame]) -> None:
        changed = False
        reply_time = False
        with self._lock:
            for f in frames:
                tag = apply_frame_to_state(f, self._state)
                if tag == "time_request":
                    reply_time = True
                elif tag not in ("ignored", "unknown", "handshake"):
                    changed = True

        with self._waiters_lock:
            active_waiters = list(self._waiters)
        for f in frames:
            for pred, ev, holder in active_waiters:
                if not ev.is_set():
                    try:
                        if pred(f):
                            holder.append(f)
                            ev.set()
                    except Exception:
                        pass

        if reply_time:
            self._send_raw(build_host_time_response(profile=self._profile))
        if changed:
            self._emit_state()


class IpcServer:
    """Unix domain socket server so CLI & hotkeys stay synchronized with the Kirigami GUI."""

    def __init__(
        self,
        session: KalmSession,
        on_show_window: Optional[Callable[[], None]] = None,
    ) -> None:
        self._session = session
        self._on_show_window = on_show_window
        self._path = ipc_socket_path()
        self._server_sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> bool:
        if self._path.exists():
            resp = ipc_request({"cmd": "ping"}, timeout=0.4)
            if resp and resp.get("ok"):
                return False
            try:
                self._path.unlink()
            except OSError:
                pass

        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(str(self._path))
        os.chmod(self._path, 0o600)
        srv.listen(8)
        srv.settimeout(0.5)
        self._server_sock = srv
        self._stop.clear()
        self._thread = threading.Thread(target=self._accept_loop, name="KalmIpcServer", daemon=True)
        self._thread.start()
        return True

    def stop(self) -> None:
        self._stop.set()
        if self._server_sock:
            try:
                self._server_sock.close()
            except OSError:
                pass
            self._server_sock = None
        try:
            if self._path.exists():
                self._path.unlink()
        except OSError:
            pass

    def _accept_loop(self) -> None:
        assert self._server_sock is not None
        while not self._stop.is_set():
            try:
                conn, _ = self._server_sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle_client, args=(conn,), daemon=True).start()

    def _handle_client(self, conn: socket.socket) -> None:
        with conn:
            conn.settimeout(2.5)
            try:
                raw = conn.recv(4096)
                if not raw:
                    return
                req = json.loads(raw.decode("utf-8"))
                resp = self._dispatch(req)
            except Exception as exc:
                resp = {"ok": False, "error": str(exc)}
            try:
                conn.sendall(json.dumps(resp, ensure_ascii=False).encode("utf-8"))
            except OSError:
                pass

    def _dispatch(self, req: dict) -> dict:
        cmd = req.get("cmd")
        if cmd == "ping":
            return {"ok": True}
        if cmd == "show":
            if self._on_show_window:
                self._on_show_window()
            return {"ok": True}
        if cmd == "status":
            return {"ok": True, "state": self._session.snapshot.to_dict()}
        if cmd == "select_device":
            mac = str(req["mac"])
            ok = self._session.switch_to_device(mac)
            return {"ok": ok, "state": self._session.snapshot.to_dict()}
        if cmd == "reload_registry":
            self._session.registry.load()
            self._session.notify_registry_updated()
            return {"ok": True}
        if cmd == "set_noise":
            mode = NoiseMode.from_str(str(req.get("mode", "")))
            ok = self._session.set_noise_mode(mode)
            return {
                "ok": ok,
                "error": None if ok else "Наушники не подтвердили переключение режима",
                "state": self._session.snapshot.to_dict(),
            }
        if cmd == "toggle_noise":
            modes_raw = req.get("modes")
            cycle = [NoiseMode.from_str(m) for m in modes_raw] if modes_raw else None
            nxt = self._session.toggle_noise_mode(cycle)
            return {
                "ok": nxt is not None,
                "error": None if nxt is not None else "Не удалось переключить режим",
                "state": self._session.snapshot.to_dict(),
            }
        if cmd == "set_eq":
            eff = int(req["effect"])
            ok = self._session.set_audio_effect(eff)
            return {"ok": ok, "state": self._session.snapshot.to_dict()}
        if cmd == "set_game":
            en = bool(req["enabled"])
            ok = self._session.set_low_latency(en)
            return {"ok": ok, "state": self._session.snapshot.to_dict()}
        return {"ok": False, "error": f"Unknown IPC command: {cmd}"}


def ipc_request(payload: dict, timeout: float = 2.5) -> Optional[dict]:
    path = ipc_socket_path()
    if not path.exists():
        return None
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(str(path))
        sock.sendall(json.dumps(payload).encode("utf-8"))
        chunks: List[bytes] = []
        while True:
            part = sock.recv(8192)
            if not part:
                break
            chunks.append(part)
        if not chunks:
            return None
        return json.loads(b"".join(chunks).decode("utf-8"))
    except OSError:
        return None
    finally:
        try:
            sock.close()
        except OSError:
            pass


# ---- Standalone Oneshot Helper (when GUI/daemon is not running) ----


def _resolve_oneshot_target(
    registry: DeviceRegistry,
    device_query: Optional[str] = None,
) -> tuple[str, str, Optional[str]]:
    registry.ensure_initial_import()
    if device_query:
        resolved = registry.resolve_query(device_query)
        if resolved is None:
            raise RuntimeError(f"Наушники {device_query!r} не найдены в списке сохранённых.")
        bdev = find_bluez_by_mac(resolved.mac)
        if bdev and not bdev.connected:
            raise RuntimeError(f"Наушники {resolved.display_name} ({resolved.mac}) сейчас не подключены по Bluetooth.")
        return resolved.mac, resolved.display_name, (bdev.path if bdev else None)

    active = registry.get_active()
    if active:
        bdev = find_bluez_by_mac(active.mac)
        if bdev and bdev.connected:
            return active.mac, active.display_name, bdev.path
        if registry.auto_switch:
            for cand in registry.devices:
                cb = find_bluez_by_mac(cand.mac)
                if cb and cb.connected:
                    return cand.mac, cand.display_name, cb.path
        raise RuntimeError(f"Наушники {active.display_name} ({active.mac}) сейчас не подключены по Bluetooth.")

    for bdev in list_bluez_devices(only_vivo=True):
        if bdev.connected:
            return bdev.mac, bdev.name, bdev.path
    raise RuntimeError("Подключённые наушники vivo / iQOO TWS не найдены.")


def run_oneshot(
    action: Callable[[socket.socket, WireProfile, EarbudState, FrameDecoder], None],
    registry: Optional[DeviceRegistry] = None,
    device_query: Optional[str] = None,
) -> EarbudState:
    reg = registry or DeviceRegistry()
    mac, display_name, dev_path = _resolve_oneshot_target(reg, device_query)
    bdev = find_bluez_by_mac(mac)
    sock, initial_bytes = open_gaia_rfcomm(mac, dev_path=dev_path, name=display_name, timeout=3.0)
    profile = select_profile(bdev.name if bdev else display_name)
    state = EarbudState(connected=True, device_name=display_name, device_mac=mac)
    decoder = FrameDecoder()
    try:
        if initial_bytes:
            for f in decoder.feed(initial_bytes):
                apply_frame_to_state(f, state)
        action(sock, profile, state, decoder)
        return state
    finally:
        try:
            sock.close()
        except OSError:
            pass


def recv_until(
    sock: socket.socket,
    decoder: FrameDecoder,
    state: EarbudState,
    profile: WireProfile,
    stop_when: Callable[[Frame], bool],
    timeout: float = 1.8,
) -> bool:
    deadline = time.monotonic() + timeout
    matched = False
    while time.monotonic() < deadline:
        rem = max(0.05, deadline - time.monotonic())
        sock.settimeout(min(0.4, rem))
        try:
            chunk = sock.recv(2048)
            if not chunk:
                break
            for f in decoder.feed(chunk):
                tag = apply_frame_to_state(f, state)
                if tag == "time_request":
                    sock.sendall(build_host_time_response(profile=profile))
                if stop_when(f):
                    matched = True
            if matched:
                return True
        except socket.timeout:
            if matched:
                return True
    return matched


def oneshot_status(device_query: Optional[str] = None) -> dict:
    def _act(sock: socket.socket, profile: WireProfile, state: EarbudState, dec: FrameDecoder) -> None:
        for pkt in build_register_notifications():
            sock.sendall(pkt)
        for pkt in (
            build_query_battery(),
            build_query_noise_mode(profile),
            build_query_wear_state(profile),
            build_query_audio_effect(profile),
            build_query_low_latency(profile),
            build_query_noise_cycle(profile),
            build_query_multi_connect(profile),
        ):
            sock.sendall(pkt)

        seen = set()

        def _done(f: Frame) -> bool:
            if f.command in (
                CMD_REPORT_BATTERY,
                CMD_REPORT_NOISE_MODE,
                CMD_REPORT_WEAR_STATE,
                CMD_REPORT_AUDIO_EFFECT,
                CMD_REPORT_LOW_LATENCY,
                CMD_REPORT_MULTI_CONNECT,
            ):
                seen.add(f.command)
            return len(seen) >= 5 and state.firmware is not None

        recv_until(sock, dec, state, profile, _done, timeout=1.2)

    return run_oneshot(_act, device_query=device_query).to_dict()


def oneshot_set_noise(mode: NoiseMode, device_query: Optional[str] = None) -> dict:
    def _act(sock: socket.socket, profile: WireProfile, state: EarbudState, dec: FrameDecoder) -> None:
        sock.sendall(build_query_battery())
        sock.sendall(build_set_noise_mode(mode, profile))
        ok = recv_until(
            sock,
            dec,
            state,
            profile,
            lambda f: f.command in (CMD_ACK_NOISE_MODE, CMD_REPORT_NOISE_MODE)
            and len(f.payload) >= 2
            and f.payload[0] == 0
            and f.payload[1] == int(mode),
            timeout=2.0,
        )
        if not ok:
            raise RuntimeError("Наушники не подтвердили смену режима шумоподавления.")

    return run_oneshot(_act, device_query=device_query).to_dict()


def oneshot_toggle_noise(
    cycle: Optional[List[NoiseMode]] = None,
    device_query: Optional[str] = None,
) -> dict:
    order = cycle or [NoiseMode.ANC, NoiseMode.TRANSPARENCY, NoiseMode.OFF]

    def _act(sock: socket.socket, profile: WireProfile, state: EarbudState, dec: FrameDecoder) -> None:
        sock.sendall(build_query_battery())
        sock.sendall(build_query_noise_mode(profile))
        recv_until(
            sock,
            dec,
            state,
            profile,
            lambda f: f.command == CMD_REPORT_NOISE_MODE,
            timeout=1.5,
        )
        cur = state.noise_mode
        nxt = order[(order.index(cur) + 1) % len(order)] if cur in order else order[0]
        sock.sendall(build_set_noise_mode(nxt, profile))
        ok = recv_until(
            sock,
            dec,
            state,
            profile,
            lambda f: f.command in (CMD_ACK_NOISE_MODE, CMD_REPORT_NOISE_MODE)
            and len(f.payload) >= 2
            and f.payload[0] == 0
            and f.payload[1] == int(nxt),
            timeout=2.0,
        )
        if not ok:
            raise RuntimeError("Наушники не подтвердили переключение режима.")

    return run_oneshot(_act, device_query=device_query).to_dict()


def oneshot_set_eq(effect_id: int, device_query: Optional[str] = None) -> dict:
    def _act(sock: socket.socket, profile: WireProfile, state: EarbudState, dec: FrameDecoder) -> None:
        sock.sendall(build_set_audio_effect(effect_id, profile))
        ok = recv_until(
            sock,
            dec,
            state,
            profile,
            lambda f: f.command in (CMD_ACK_AUDIO_EFFECT, CMD_REPORT_AUDIO_EFFECT),
            timeout=1.8,
        )
        if not ok:
            raise RuntimeError("Наушники не подтвердили смену эквалайзера.")

    return run_oneshot(_act, device_query=device_query).to_dict()


def oneshot_set_game(enabled: bool, device_query: Optional[str] = None) -> dict:
    def _act(sock: socket.socket, profile: WireProfile, state: EarbudState, dec: FrameDecoder) -> None:
        sock.sendall(build_set_low_latency(enabled, profile))
        ok = recv_until(
            sock,
            dec,
            state,
            profile,
            lambda f: f.command in (CMD_ACK_LOW_LATENCY, CMD_REPORT_LOW_LATENCY),
            timeout=1.8,
        )
        if not ok:
            raise RuntimeError("Наушники не подтвердили переключение игрового режима.")

    return run_oneshot(_act, device_query=device_query).to_dict()
