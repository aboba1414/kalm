"""Persistent multi-earbud registry (~/.config/kalm/devices.json)."""

from __future__ import annotations

import json
import os
import re
import threading
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import List, Optional

from .transport import list_bluez_devices

MAC_RE = re.compile(r"^[0-9A-F]{2}(:[0-9A-F]{2}){5}$")


def normalize_mac(raw: str) -> str:
    cleaned = raw.strip().upper().replace("-", ":")
    if not MAC_RE.match(cleaned):
        raise ValueError(f"Некорректный MAC-адрес Bluetooth: {raw!r} (ожидается XX:XX:XX:XX:XX:XX)")
    return cleaned


@dataclass
class SavedEarbud:
    mac: str
    name: str
    alias: str = ""

    @property
    def display_name(self) -> str:
        return (self.alias or self.name or self.mac).strip()


@dataclass
class RegistryData:
    active_mac: Optional[str] = None
    auto_switch: bool = True
    initialized: bool = False
    devices: List[SavedEarbud] = field(default_factory=list)


class DeviceRegistry:
    """Thread-safe configuration store for multiple earbuds."""

    def __init__(self, path: Optional[Path] = None) -> None:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
        self._path = path or (base / "kalm" / "devices.json")
        self._lock = threading.RLock()
        self._data = RegistryData()
        self.load()

    @property
    def path(self) -> Path:
        return self._path

    def load(self) -> None:
        with self._lock:
            if not self._path.exists():
                self._data = RegistryData()
                return
            try:
                raw = json.loads(self._path.read_text("utf-8"))
                devs = [
                    SavedEarbud(
                        mac=normalize_mac(d["mac"]),
                        name=str(d.get("name") or d["mac"]),
                        alias=str(d.get("alias") or ""),
                    )
                    for d in raw.get("devices", [])
                    if isinstance(d, dict) and "mac" in d
                ]
                active = raw.get("active_mac")
                norm_active = normalize_mac(active) if isinstance(active, str) and active else None
                if norm_active and not any(d.mac == norm_active for d in devs):
                    norm_active = devs[0].mac if devs else None
                self._data = RegistryData(
                    active_mac=norm_active,
                    auto_switch=bool(raw.get("auto_switch", True)),
                    initialized=bool(raw.get("initialized", True)),
                    devices=devs,
                )
            except Exception:
                self._data = RegistryData()

    def save(self) -> None:
        with self._lock:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            payload = {
                "active_mac": self._data.active_mac,
                "auto_switch": self._data.auto_switch,
                "initialized": self._data.initialized,
                "devices": [asdict(d) for d in self._data.devices],
            }
            tmp = self._path.with_suffix(".tmp")
            tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), "utf-8")
            tmp.replace(self._path)

    def ensure_initial_import(self) -> None:
        """On first launch, import paired vivo / iQOO TWS earbuds from BlueZ automatically."""
        with self._lock:
            if self._data.initialized:
                return
            self._data.initialized = True
            try:
                vivo_devs = list_bluez_devices(only_vivo=True)
            except Exception:
                vivo_devs = []
            for bdev in vivo_devs:
                self._add_unlocked(bdev.mac, bdev.name)
                if bdev.connected and self._data.active_mac is None:
                    self._data.active_mac = bdev.mac
            if self._data.active_mac is None and self._data.devices:
                self._data.active_mac = self._data.devices[0].mac
            self.save()

    @property
    def devices(self) -> List[SavedEarbud]:
        with self._lock:
            return [SavedEarbud(d.mac, d.name, d.alias) for d in self._data.devices]

    @property
    def active_mac(self) -> Optional[str]:
        with self._lock:
            return self._data.active_mac

    @property
    def auto_switch(self) -> bool:
        with self._lock:
            return self._data.auto_switch

    def set_auto_switch(self, enabled: bool) -> None:
        with self._lock:
            self._data.auto_switch = bool(enabled)
            self.save()

    def get(self, mac: str) -> Optional[SavedEarbud]:
        norm = normalize_mac(mac)
        with self._lock:
            for d in self._data.devices:
                if d.mac == norm:
                    return SavedEarbud(d.mac, d.name, d.alias)
        return None

    def get_active(self) -> Optional[SavedEarbud]:
        with self._lock:
            if not self._data.active_mac:
                return None
            for d in self._data.devices:
                if d.mac == self._data.active_mac:
                    return SavedEarbud(d.mac, d.name, d.alias)
            return None

    def _add_unlocked(self, mac: str, name: str = "", alias: str = "") -> SavedEarbud:
        norm = normalize_mac(mac)
        for d in self._data.devices:
            if d.mac == norm:
                if name:
                    d.name = name
                if alias:
                    d.alias = alias
                return SavedEarbud(d.mac, d.name, d.alias)
        entry = SavedEarbud(mac=norm, name=(name or norm).strip(), alias=alias.strip())
        self._data.devices.append(entry)
        if self._data.active_mac is None:
            self._data.active_mac = norm
        return SavedEarbud(entry.mac, entry.name, entry.alias)

    def add(self, mac: str, name: str = "", alias: str = "", make_active: bool = True) -> SavedEarbud:
        with self._lock:
            self._data.initialized = True
            entry = self._add_unlocked(mac, name, alias)
            if make_active:
                self._data.active_mac = entry.mac
            self.save()
            return entry

    def remove(self, mac: str) -> bool:
        norm = normalize_mac(mac)
        with self._lock:
            before = len(self._data.devices)
            self._data.devices = [d for d in self._data.devices if d.mac != norm]
            if len(self._data.devices) == before:
                return False
            if self._data.active_mac == norm:
                self._data.active_mac = self._data.devices[0].mac if self._data.devices else None
            self.save()
            return True

    def rename(self, mac: str, alias: str) -> bool:
        norm = normalize_mac(mac)
        with self._lock:
            for d in self._data.devices:
                if d.mac == norm:
                    d.alias = alias.strip()
                    self.save()
                    return True
        return False

    def set_active(self, mac: str) -> bool:
        norm = normalize_mac(mac)
        with self._lock:
            if any(d.mac == norm for d in self._data.devices):
                self._data.active_mac = norm
                self.save()
                return True
        return False

    def resolve_query(self, query: str) -> Optional[SavedEarbud]:
        q = query.strip()
        if not q:
            return None
        try:
            norm = normalize_mac(q)
            found = self.get(norm)
            if found:
                return found
            return SavedEarbud(mac=norm, name=norm)
        except ValueError:
            pass

        low = q.lower()
        with self._lock:
            for d in self._data.devices:
                if d.display_name.lower() == low or d.name.lower() == low:
                    return SavedEarbud(d.mac, d.name, d.alias)
            for d in self._data.devices:
                if low in d.display_name.lower() or low in d.name.lower():
                    return SavedEarbud(d.mac, d.name, d.alias)
        return None
