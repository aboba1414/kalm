"""Linux BlueZ device discovery, Bluetooth connect/disconnect, and RFCOMM GAIA transport."""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from gi.repository import Gio, GLib

from .protocol import (
    AUDIO_SINK_UUID,
    CMD_HANDSHAKE_RESP,
    GAIA_VENDOR,
    GATT_SERVICE_UUID,
    VIVO_GAIA_UUID,
    FrameDecoder,
    build_handshake,
)

CANDIDATE_CHANNELS: Tuple[int, ...] = (15, 13, 14, 12, 16, 11)


@dataclass
class BluezDevice:
    path: str
    mac: str
    name: str
    connected: bool
    paired: bool
    icon: str
    uuids: List[str]
    battery_pct: Optional[int] = None

    @property
    def is_vivo_tws(self) -> bool:
        if any(u.lower() in (VIVO_GAIA_UUID, GATT_SERVICE_UUID) for u in self.uuids):
            return True
        low = self.name.lower()
        return ("vivo tws" in low) or ("iqoo tws" in low) or ("vivo headphones" in low)

    @property
    def is_audio_device(self) -> bool:
        if self.is_vivo_tws:
            return True
        if "audio" in self.icon.lower() or "headset" in self.icon.lower() or "headphone" in self.icon.lower():
            return True
        return any(u.lower() == AUDIO_SINK_UUID for u in self.uuids)


def _cache_file() -> Path:
    base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "kalm" / "channels.json"


def _load_cached_channel(mac: str) -> Optional[int]:
    try:
        data = json.loads(_cache_file().read_text("utf-8"))
        val = data.get(mac.upper())
        return int(val) if isinstance(val, int) and 1 <= val <= 30 else None
    except Exception:
        return None


def _save_cached_channel(mac: str, channel: int) -> None:
    try:
        path = _cache_file()
        path.parent.mkdir(parents=True, exist_ok=True)
        try:
            data = json.loads(path.read_text("utf-8"))
        except Exception:
            data = {}
        data[mac.upper()] = int(channel)
        path.write_text(json.dumps(data, indent=2), "utf-8")
    except Exception:
        pass


def list_bluez_devices(only_vivo: bool = False, only_audio: bool = False) -> List[BluezDevice]:
    """Query BlueZ ObjectManager over D-Bus for Bluetooth devices."""
    conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    res = conn.call_sync(
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        None,
        GLib.VariantType("(a{oa{sa{sv}}})"),
        Gio.DBusCallFlags.NONE,
        3000,
        None,
    )
    objects: Dict[str, Dict[str, Dict[str, object]]] = res.unpack()[0]
    devices: List[BluezDevice] = []
    for obj_path, ifaces in objects.items():
        dev = ifaces.get("org.bluez.Device1")
        if not dev:
            continue
        mac = str(dev.get("Address", "")).upper()
        if not mac:
            continue
        name = str(dev.get("Alias") or dev.get("Name") or mac)
        connected = bool(dev.get("Connected", False))
        paired = bool(dev.get("Paired", False))
        icon = str(dev.get("Icon", ""))
        uuids = [str(u).lower() for u in dev.get("UUIDs", [])]
        batt_iface = ifaces.get("org.bluez.Battery1")
        batt = int(batt_iface["Percentage"]) if batt_iface and "Percentage" in batt_iface else None
        item = BluezDevice(
            path=obj_path,
            mac=mac,
            name=name,
            connected=connected,
            paired=paired,
            icon=icon,
            uuids=uuids,
            battery_pct=batt,
        )
        if only_vivo and not item.is_vivo_tws:
            continue
        if only_audio and not item.is_audio_device:
            continue
        devices.append(item)

    devices.sort(key=lambda d: (not d.is_vivo_tws, not d.connected, not d.paired, d.name.lower()))
    return devices


def find_bluez_by_mac(mac: str) -> Optional[BluezDevice]:
    norm = mac.strip().upper()
    for dev in list_bluez_devices(only_vivo=False):
        if dev.mac == norm:
            return dev
    return None


def connect_bluez_device(mac: str) -> bool:
    """Trigger system-level Bluetooth connection (A2DP/HFP) via BlueZ Device1.Connect."""
    dev = find_bluez_by_mac(mac)
    if dev is None:
        return False
    conn = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    try:
        conn.call_sync(
            "org.bluez",
            dev.path,
            "org.bluez.Device1",
            "Connect",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            10000,
            None,
        )
        return True
    except Exception:
        return False


def _discover_channel_via_sdp_subprocess(dev_path: str) -> Optional[int]:
    """Discover the exact RFCOMM channel for VIVO_GAIA_UUID via BlueZ Profile1 in an isolated process."""
    code = (
        "import os, socket, dbus, dbus.service, dbus.mainloop.glib\n"
        "from gi.repository import GLib\n"
        f"UUID = {VIVO_GAIA_UUID!r}\n"
        f"DEV_PATH = {dev_path!r}\n"
        "PROF_PATH = f'/io/kalm/sdp_{os.getpid()}'\n"
        "dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)\n"
        "bus = dbus.SystemBus()\n"
        "loop = GLib.MainLoop()\n"
        "ch = []\n"
        "class P(dbus.service.Object):\n"
        "    @dbus.service.method('org.bluez.Profile1', in_signature='oha{sv}', out_signature='')\n"
        "    def NewConnection(self, path, fd, props):\n"
        "        rfd = fd.take()\n"
        "        s = socket.fromfd(rfd, socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)\n"
        "        os.close(rfd)\n"
        "        ch.append(s.getpeername()[1])\n"
        "        s.close()\n"
        "        loop.quit()\n"
        "p = P(bus, PROF_PATH)\n"
        "mgr = dbus.Interface(bus.get_object('org.bluez', '/org/bluez'), 'org.bluez.ProfileManager1')\n"
        "mgr.RegisterProfile(PROF_PATH, UUID, {'Role': 'client', 'RequireAuthentication': False, 'RequireAuthorization': False})\n"
        "dev = dbus.Interface(bus.get_object('org.bluez', DEV_PATH), 'org.bluez.Device1')\n"
        "GLib.idle_add(lambda: (dev.ConnectProfile(UUID, reply_handler=lambda: None, error_handler=lambda e: loop.quit()), False)[-1])\n"
        "GLib.timeout_add_seconds(4, lambda: (loop.quit(), False)[-1])\n"
        "loop.run()\n"
        "try: mgr.UnregisterProfile(PROF_PATH)\n"
        "except Exception: pass\n"
        "if ch: print(ch[0])\n"
    )
    try:
        proc = subprocess.run(
            [sys.executable, "-c", code],
            capture_output=True,
            text=True,
            timeout=6,
        )
        out = proc.stdout.strip()
        if out.isdigit():
            return int(out)
    except Exception:
        pass
    return None


def _try_connect_and_handshake(
    mac: str,
    channel: int,
    timeout: float = 2.0,
) -> Optional[Tuple[socket.socket, bytes]]:
    sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_STREAM, socket.BTPROTO_RFCOMM)
    sock.settimeout(timeout)
    try:
        sock.connect((mac, channel))
        sock.sendall(build_handshake())
        decoder = FrameDecoder()
        leftover = bytearray()
        sock.settimeout(1.5)
        while True:
            chunk = sock.recv(1024)
            if not chunk:
                break
            leftover.extend(chunk)
            frames = decoder.feed(chunk)
            if any(
                f.vendor == GAIA_VENDOR and f.command == CMD_HANDSHAKE_RESP
                for f in frames
            ):
                return sock, bytes(leftover)
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass
    return None


def open_gaia_rfcomm(
    mac: str,
    dev_path: Optional[str] = None,
    name: str = "",
    timeout: float = 2.5,
) -> Tuple[socket.socket, bytes]:
    """Open a verified GAIA RFCOMM socket to ``mac``."""
    norm_mac = mac.strip().upper()
    candidates: List[int] = []
    cached = _load_cached_channel(norm_mac)
    if cached is not None:
        candidates.append(cached)
    for ch in CANDIDATE_CHANNELS:
        if ch not in candidates:
            candidates.append(ch)

    for ch in candidates[:3]:
        res = _try_connect_and_handshake(norm_mac, ch, timeout=timeout)
        if res is not None:
            _save_cached_channel(norm_mac, ch)
            return res

    if dev_path:
        sdp_ch = _discover_channel_via_sdp_subprocess(dev_path)
        if sdp_ch is not None:
            res = _try_connect_and_handshake(norm_mac, sdp_ch, timeout=timeout)
            if res is not None:
                _save_cached_channel(norm_mac, sdp_ch)
                return res
    else:
        sdp_ch = None

    for ch in candidates[3:]:
        if ch == sdp_ch:
            continue
        res = _try_connect_and_handshake(norm_mac, ch, timeout=1.5)
        if res is not None:
            _save_cached_channel(norm_mac, ch)
            return res

    title = f"{name} ({norm_mac})" if name else norm_mac
    raise ConnectionError(
        f"Не удалось открыть канал GAIA с наушниками {title}. "
        "Убедитесь, что они подключены по Bluetooth и вынуты из закрытого кейса."
    )
