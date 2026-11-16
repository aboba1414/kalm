"""Unit tests for Kalm multi-earbud registry."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from kalm.registry import DeviceRegistry


class TestDeviceRegistry(unittest.TestCase):
    def test_multi_device_crud_and_persistence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp) / "devices.json"
            reg = DeviceRegistry(path=cfg)

            d1 = reg.add("1c:ba:b8:0f:5a:0f", "vivo TWS 5 Hi-Fi", make_active=True)
            d2 = reg.add("AA-BB-CC-11-22-33", "vivo TWS 3e", alias="Спорт", make_active=False)

            self.assertEqual(d1.mac, "1C:BA:B8:0F:5A:0F")
            self.assertEqual(d2.mac, "AA:BB:CC:11:22:33")
            self.assertEqual(reg.active_mac, "1C:BA:B8:0F:5A:0F")
            self.assertEqual(len(reg.devices), 2)

            # Resolve by alias, name, or MAC
            self.assertEqual(reg.resolve_query("спорт").mac, "AA:BB:CC:11:22:33")
            self.assertEqual(reg.resolve_query("tws 5").mac, "1C:BA:B8:0F:5A:0F")

            # Rename & switch active
            self.assertTrue(reg.rename(d1.mac, "Основные"))
            self.assertTrue(reg.set_active(d2.mac))

            # Reload from disk
            reg2 = DeviceRegistry(path=cfg)
            self.assertEqual(reg2.active_mac, "AA:BB:CC:11:22:33")
            self.assertEqual(reg2.get(d1.mac).display_name, "Основные")

            # Remove active -> falls back to remaining device
            self.assertTrue(reg2.remove(d2.mac))
            self.assertEqual(reg2.active_mac, "1C:BA:B8:0F:5A:0F")


if __name__ == "__main__":
    unittest.main()
