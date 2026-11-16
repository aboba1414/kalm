"""Compact GAIA protocol implementation for vivo / iQOO TWS earbuds."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime
from enum import IntEnum
from typing import Dict, List, Optional, Tuple

VIVO_GAIA_UUID = "00000837-d102-11e1-9b23-00025b00a5a5"
GATT_SERVICE_UUID = "00001100-d102-11e1-9b23-00025b00a5a5"
AUDIO_SINK_UUID = "0000110b-0000-1000-8000-00805f9b34fb"

PREAMBLE = 0xFF
FLAG_CHECKSUM = 0x01
FLAG_LENGTH_EXTENSION = 0x02
COMMAND_BYTES = 4
MAX_FRAME_BYTES = 65_544

GAIA_VENDOR = 0x000A
VIVO_VENDOR = 0x001B

# Commands
CMD_HANDSHAKE = 0x0300
CMD_HANDSHAKE_RESP = 0x8300

REGISTER_NOTIFICATIONS: Tuple[int, ...] = (0x0202, 0x0203, 0x0204, 0x0205, 0x0206)

CMD_SET_DOUBLE_TAP = 0x0102
CMD_QUERY_DOUBLE_TAP = 0x0202
CMD_REPORT_DOUBLE_TAP = 0x8202

CMD_SET_WEAR_DETECTION = 0x0103
CMD_QUERY_WEAR_DETECTION = 0x0203
CMD_ACK_WEAR_DETECTION = 0x8103
CMD_REPORT_WEAR_DETECTION = 0x8203

CMD_QUERY_BATTERY = 0x0207
CMD_REPORT_BATTERY = 0x8207

CMD_QUERY_WEAR_STATE = 0x020D
CMD_REPORT_WEAR_STATE = 0x820D

CMD_SET_AUDIO_EFFECT = 0x0118
CMD_QUERY_AUDIO_EFFECT = 0x0218
CMD_ACK_AUDIO_EFFECT = 0x8118
CMD_REPORT_AUDIO_EFFECT = 0x8218

CMD_QUERY_FIRMWARE = 0x021C
CMD_REPORT_FIRMWARE = 0x821C

CMD_SET_FIND_DEVICE = 0x0120
CMD_ACK_FIND_DEVICE = 0x8120

CMD_SET_NOISE_MODE = 0x0130
CMD_QUERY_NOISE_MODE = 0x0230
CMD_ACK_NOISE_MODE = 0x8130
CMD_REPORT_NOISE_MODE = 0x8230

CMD_SET_NOISE_CYCLE = 0x0131
CMD_QUERY_NOISE_CYCLE = 0x0231
CMD_ACK_NOISE_CYCLE = 0x8131
CMD_REPORT_NOISE_CYCLE = 0x8231
NOISE_CYCLE_SUBCMD = 0x05

CMD_QUERY_MULTI_CONNECT = 0x0249
CMD_REPORT_MULTI_CONNECT = 0x8249
CMD_SET_MULTI_CONNECT = 0x014A
CMD_ACK_MULTI_CONNECT = 0x814A

CMD_SET_LOW_LATENCY = 0x0151
CMD_QUERY_LOW_LATENCY = 0x0251
CMD_ACK_LOW_LATENCY = 0x8151
CMD_REPORT_LOW_LATENCY = 0x8251

CMD_REPORT_TELEMETRY = 0x8224
CMD_PEER_TIME_REQUEST = 0x8509
CMD_HOST_TIME_RESPONSE = 0x0509


class NoiseMode(IntEnum):
    """Active noise control modes (unified across vivo TWS models)."""

    ANC = 0
    OFF = 1
    TRANSPARENCY = 2

    @property
    def label_ru(self) -> str:
        return {
            NoiseMode.ANC: "Шумоподавление",
            NoiseMode.OFF: "Выключено",
            NoiseMode.TRANSPARENCY: "Прозрачность",
        }[self]

    @property
    def slug(self) -> str:
        return {
            NoiseMode.ANC: "anc",
            NoiseMode.OFF: "off",
            NoiseMode.TRANSPARENCY: "transparency",
        }[self]

    @classmethod
    def from_str(cls, text: str) -> "NoiseMode":
        key = text.strip().lower()
        mapping = {
            "anc": cls.ANC,
            "nc": cls.ANC,
            "on": cls.ANC,
            "noise": cls.ANC,
            "шумодав": cls.ANC,
            "шумоподавление": cls.ANC,
            "0": cls.ANC,
            "off": cls.OFF,
            "normal": cls.OFF,
            "выкл": cls.OFF,
            "выключено": cls.OFF,
            "1": cls.OFF,
            "trans": cls.TRANSPARENCY,
            "transparency": cls.TRANSPARENCY,
            "transparent": cls.TRANSPARENCY,
            "ambient": cls.TRANSPARENCY,
            "прозрачность": cls.TRANSPARENCY,
            "2": cls.TRANSPARENCY,
        }
        if key not in mapping:
            raise ValueError(f"Неизвестный режим: {text!r}. Доступны: anc, off, trans")
        return mapping[key]


# DeepX / Sound Effects presets supported by vivo TWS 5 Hi-Fi (AudioEffect table 28)
AUDIO_EFFECT_PRESETS: Dict[int, str] = {
    0x00: "Стандартный",
    0x01: "Чистый вокал",
    0x02: "Мегабас",
    0x03: "Чистые высокие",
    0x05: "Аудиокниги",
    0x0B: "Мастер звука (Hi-Fi)",
}

# Long-press noise cycle presets (0x0131 subcmd 0x05)
NOISE_CYCLE_PRESETS: Dict[int, str] = {
    0x0B: "Шумодав + Прозрачность + Выкл",
    0x0A: "Шумодав + Прозрачность",
    0x08: "Шумодав + Выкл",
    0x09: "Прозрачность + Выкл",
}

MC_DISCONNECTED = 0x00
MC_CONNECTED_INACTIVE = 0x01
MC_CONNECTED_ACTIVE = 0x02

WEAR_LEFT_IN_CASE = 0x01
WEAR_RIGHT_IN_CASE = 0x02
WEAR_LEFT_WORN = 0x04
WEAR_RIGHT_WORN = 0x08


@dataclass(frozen=True)
class WireProfile:
    label: str
    gaia_version: int
    noise_query_payload: bytes
    noise_set_suffix: bytes


PROFILE_FAMILY_V4 = WireProfile(
    label="vivo TWS 4/5 Family (GAIA v4)",
    gaia_version=4,
    noise_query_payload=b"\x00",
    noise_set_suffix=b"\x03\x01",
)

PROFILE_TWS_3E_V3 = WireProfile(
    label="vivo TWS 3e (GAIA v3)",
    gaia_version=3,
    noise_query_payload=b"",
    noise_set_suffix=b"\x03",
)

PROFILE_AIR3_PRO_V3 = WireProfile(
    label="vivo TWS Air3 Pro (GAIA v3)",
    gaia_version=3,
    noise_query_payload=b"",
    noise_set_suffix=b"\x04\x00",
)


def select_profile(device_name: str) -> WireProfile:
    norm = "".join(ch.lower() for ch in (device_name or "") if ch.isalnum())
    if "air3pro" in norm:
        return PROFILE_AIR3_PRO_V3
    if "tws3e" in norm:
        return PROFILE_TWS_3E_V3
    return PROFILE_FAMILY_V4


@dataclass
class Frame:
    version: int
    flags: int
    vendor: int
    command: int
    payload: bytes
    raw: bytes


@dataclass
class BatteryReading:
    level: Optional[int] = None
    charging: bool = False
    stale: bool = True


@dataclass
class MultiConnectDevice:
    mac: bytes
    raw3: bytes
    status: int
    name: str

    @property
    def mac_str(self) -> str:
        return ":".join(f"{b:02X}" for b in self.mac)

    @property
    def connected(self) -> bool:
        return self.status != MC_DISCONNECTED

    @property
    def active(self) -> bool:
        return self.status == MC_CONNECTED_ACTIVE

    @property
    def status_ru(self) -> str:
        if self.status == MC_CONNECTED_ACTIVE:
            return "Подключено (активно)"
        if self.status == MC_CONNECTED_INACTIVE:
            return "Подключено"
        return "Отключено"


@dataclass
class EarbudState:
    connected: bool = False
    device_name: str = ""
    device_mac: str = ""
    firmware: Optional[str] = None

    noise_mode: Optional[NoiseMode] = None
    noise_effect: Optional[int] = None
    transparency_effect: Optional[int] = None

    left_battery: BatteryReading = field(default_factory=BatteryReading)
    right_battery: BatteryReading = field(default_factory=BatteryReading)
    case_battery: BatteryReading = field(default_factory=BatteryReading)

    left_wear: Optional[str] = None
    right_wear: Optional[str] = None
    wear_detection: Optional[bool] = None

    audio_effect: Optional[int] = None
    low_latency: Optional[bool] = None
    noise_cycle_left: Optional[int] = None
    noise_cycle_right: Optional[int] = None

    paired_devices: List[MultiConnectDevice] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {
            "connected": self.connected,
            "device_name": self.device_name,
            "device_mac": self.device_mac,
            "firmware": self.firmware,
            "noise_mode": self.noise_mode.slug if self.noise_mode is not None else None,
            "noise_mode_label": self.noise_mode.label_ru if self.noise_mode is not None else None,
            "battery": {
                "left": self.left_battery.level,
                "left_charging": self.left_battery.charging,
                "left_stale": self.left_battery.stale,
                "right": self.right_battery.level,
                "right_charging": self.right_battery.charging,
                "right_stale": self.right_battery.stale,
                "case": self.case_battery.level,
                "case_charging": self.case_battery.charging,
                "case_stale": self.case_battery.stale,
            },
            "wear": {
                "left": self.left_wear,
                "right": self.right_wear,
                "auto_pause": self.wear_detection,
            },
            "audio_effect": self.audio_effect,
            "audio_effect_label": AUDIO_EFFECT_PRESETS.get(self.audio_effect, None)
            if self.audio_effect is not None
            else None,
            "low_latency": self.low_latency,
            "noise_cycle": {
                "left": self.noise_cycle_left,
                "right": self.noise_cycle_right,
                "label": NOISE_CYCLE_PRESETS.get(self.noise_cycle_left)
                if self.noise_cycle_left is not None
                else None,
            },
            "paired_devices": [
                {
                    "mac": d.mac_str,
                    "name": d.name,
                    "status": d.status,
                    "status_label": d.status_ru,
                    "connected": d.connected,
                    "active": d.active,
                }
                for d in self.paired_devices
            ],
        }


def _xor_checksum(buf: bytes, count: int) -> int:
    acc = 0
    for i in range(count):
        acc ^= buf[i]
    return acc & 0xFF


def encode_frame(
    version: int,
    vendor: int,
    command: int,
    payload: bytes = b"",
    flags: int = 0,
) -> bytes:
    extended = bool(flags & FLAG_LENGTH_EXTENSION) or len(payload) > 254
    if extended:
        flags |= FLAG_LENGTH_EXTENSION
    header_size = 5 if extended else 4
    out = bytearray(header_size + COMMAND_BYTES + len(payload))
    out[0] = PREAMBLE
    out[1] = version & 0xFF
    out[2] = flags & 0xFF
    if extended:
        out[3] = (len(payload) >> 8) & 0xFF
        out[4] = len(payload) & 0xFF
    else:
        out[3] = len(payload) & 0xFF
    out[header_size] = (vendor >> 8) & 0xFF
    out[header_size + 1] = vendor & 0xFF
    out[header_size + 2] = (command >> 8) & 0xFF
    out[header_size + 3] = command & 0xFF
    out[header_size + COMMAND_BYTES :] = payload
    if flags & FLAG_CHECKSUM:
        out.append(_xor_checksum(out, len(out)))
    return bytes(out)


class FrameDecoder:
    """Streaming Compact GAIA decoder supporting length extension (0x02) and XOR checksum (0x01)."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def reset(self) -> None:
        self._buf.clear()

    def feed(self, chunk: bytes) -> List[Frame]:
        if chunk:
            self._buf.extend(chunk)
        frames: List[Frame] = []
        while True:
            idx = self._buf.find(PREAMBLE)
            if idx < 0:
                self._buf.clear()
                break
            if idx > 0:
                del self._buf[:idx]

            if len(self._buf) < 4:
                break

            ver = self._buf[1]
            flags = self._buf[2]
            if ver not in (3, 4) or (flags & ~(FLAG_CHECKSUM | FLAG_LENGTH_EXTENSION)) != 0:
                del self._buf[0]
                continue

            extended = bool(flags & FLAG_LENGTH_EXTENSION)
            header_size = 5 if extended else 4
            if len(self._buf) < header_size + COMMAND_BYTES:
                break

            vendor = (self._buf[header_size] << 8) | self._buf[header_size + 1]
            if vendor not in (GAIA_VENDOR, VIVO_VENDOR):
                del self._buf[0]
                continue

            payload_len = (
                ((self._buf[3] << 8) | self._buf[4]) if extended else self._buf[3]
            )
            checksum_len = 1 if (flags & FLAG_CHECKSUM) else 0
            total_len = header_size + COMMAND_BYTES + payload_len + checksum_len
            if total_len > MAX_FRAME_BYTES:
                del self._buf[0]
                continue
            if len(self._buf) < total_len:
                break

            raw = bytes(self._buf[:total_len])
            del self._buf[:total_len]

            if checksum_len and _xor_checksum(raw, len(raw) - 1) != raw[-1]:
                continue

            command = (raw[header_size + 2] << 8) | raw[header_size + 3]
            payload_start = header_size + COMMAND_BYTES
            payload = raw[payload_start : payload_start + payload_len]
            frames.append(
                Frame(
                    version=ver,
                    flags=flags,
                    vendor=vendor,
                    command=command,
                    payload=payload,
                    raw=raw,
                )
            )
        return frames


def build_handshake() -> bytes:
    return encode_frame(4, GAIA_VENDOR, CMD_HANDSHAKE)


def build_register_notifications() -> List[bytes]:
    return [encode_frame(4, VIVO_VENDOR, cmd) for cmd in REGISTER_NOTIFICATIONS]


def build_query_battery() -> bytes:
    return encode_frame(4, VIVO_VENDOR, CMD_QUERY_BATTERY)


def build_query_noise_mode(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_QUERY_NOISE_MODE,
        profile.noise_query_payload,
    )


def build_set_noise_mode(
    mode: NoiseMode,
    profile: WireProfile = PROFILE_FAMILY_V4,
) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_NOISE_MODE,
        bytes([int(mode)]) + profile.noise_set_suffix,
    )


def build_query_wear_state(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_QUERY_WEAR_STATE)


def build_set_wear_detection(enabled: bool, profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_WEAR_DETECTION,
        bytes([1 if enabled else 0]),
    )


def build_query_wear_detection(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_QUERY_WEAR_DETECTION)


def build_query_audio_effect(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_QUERY_AUDIO_EFFECT)


def build_set_audio_effect(effect_id: int, profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_AUDIO_EFFECT,
        bytes([effect_id & 0xFF]),
    )


def build_query_low_latency(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_QUERY_LOW_LATENCY)


def build_set_low_latency(enabled: bool, profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_LOW_LATENCY,
        bytes([1 if enabled else 0]),
    )


def build_query_noise_cycle(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_QUERY_NOISE_CYCLE,
        bytes([NOISE_CYCLE_SUBCMD]),
    )


def build_set_noise_cycle(
    left_code: int,
    right_code: int,
    profile: WireProfile = PROFILE_FAMILY_V4,
) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_NOISE_CYCLE,
        bytes([NOISE_CYCLE_SUBCMD, left_code & 0xFF, right_code & 0xFF]),
    )


def build_query_multi_connect(profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_QUERY_MULTI_CONNECT)


def build_set_multi_connect(
    devices: List[MultiConnectDevice],
    target_mac: str,
    connect: bool,
    profile: WireProfile = PROFILE_FAMILY_V4,
) -> bytes:
    norm_target = target_mac.upper()
    payload = bytearray()
    for dev in devices:
        payload.extend(dev.mac[:6])
        if dev.mac_str.upper() == norm_target:
            payload.append(MC_CONNECTED_ACTIVE if connect else MC_DISCONNECTED)
        else:
            payload.append(dev.status & 0xFF)
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_SET_MULTI_CONNECT, bytes(payload))


def build_find_device(ring: bool, profile: WireProfile = PROFILE_FAMILY_V4) -> bytes:
    return encode_frame(
        profile.gaia_version,
        VIVO_VENDOR,
        CMD_SET_FIND_DEVICE,
        bytes([1 if ring else 0]),
    )


def build_host_time_response(
    dt: Optional[datetime] = None,
    profile: WireProfile = PROFILE_FAMILY_V4,
) -> bytes:
    now = dt or datetime.now()
    payload = bytes(
        [
            (now.year // 100) & 0xFF,
            (now.year % 100) & 0xFF,
            now.month & 0xFF,
            now.day & 0xFF,
            now.hour & 0xFF,
            now.minute & 0xFF,
            now.second & 0xFF,
        ]
    )
    return encode_frame(profile.gaia_version, VIVO_VENDOR, CMD_HOST_TIME_RESPONSE, payload)


def apply_frame_to_state(frame: Frame, state: EarbudState) -> str:
    p = frame.payload

    if frame.vendor == GAIA_VENDOR and frame.command == CMD_HANDSHAKE_RESP:
        return "handshake"

    if frame.vendor != VIVO_VENDOR:
        return "unknown"

    cmd = frame.command

    if cmd in (CMD_ACK_NOISE_MODE, CMD_REPORT_NOISE_MODE):
        if len(p) >= 2 and p[0] == 0:
            try:
                state.noise_mode = NoiseMode(p[1])
                if len(p) > 2:
                    state.noise_effect = p[2]
                if len(p) > 3:
                    state.transparency_effect = p[3]
                return "noise_mode"
            except ValueError:
                pass
        return "ignored"

    if cmd == CMD_REPORT_BATTERY:
        if len(p) >= 5 and p[0] == 0:
            chg = p[4]
            _update_battery_slot(state.left_battery, p[1], bool(chg & 0x01))
            _update_battery_slot(state.right_battery, p[2], bool(chg & 0x02))
            _update_battery_slot(state.case_battery, p[3], bool(chg & 0x04))
            return "battery"
        return "ignored"

    if cmd == CMD_REPORT_WEAR_STATE:
        if len(p) >= 2 and p[0] == 0:
            flags = p[1]
            state.left_wear = _decode_ear_wear(flags, WEAR_LEFT_IN_CASE, WEAR_LEFT_WORN)
            state.right_wear = _decode_ear_wear(flags, WEAR_RIGHT_IN_CASE, WEAR_RIGHT_WORN)
            return "wear_state"
        return "ignored"

    if cmd in (CMD_ACK_WEAR_DETECTION, CMD_REPORT_WEAR_DETECTION):
        if len(p) >= 2 and p[0] == 0:
            state.wear_detection = p[1] != 0
            return "wear_detection"
        return "ignored"

    if cmd in (CMD_ACK_AUDIO_EFFECT, CMD_REPORT_AUDIO_EFFECT):
        if len(p) >= 2 and p[0] == 0:
            state.audio_effect = p[1]
            return "audio_effect"
        return "ignored"

    if cmd in (CMD_ACK_LOW_LATENCY, CMD_REPORT_LOW_LATENCY):
        if len(p) >= 2 and p[0] == 0 and p[1] in (0, 1):
            state.low_latency = p[1] == 1
            return "low_latency"
        return "ignored"

    if cmd in (CMD_ACK_NOISE_CYCLE, CMD_REPORT_NOISE_CYCLE):
        if len(p) >= 4 and p[0] == 0 and p[1] == NOISE_CYCLE_SUBCMD:
            state.noise_cycle_left = p[2]
            state.noise_cycle_right = p[3]
            return "noise_cycle"
        return "ignored"

    if cmd == CMD_REPORT_MULTI_CONNECT:
        parsed = parse_multi_connect_devices(p)
        if parsed is not None:
            state.paired_devices = parsed
            return "multi_connect"
        return "ignored"

    if cmd == CMD_REPORT_TELEMETRY:
        obj = parse_telemetry_payload(p)
        if obj:
            ver = obj.get("V")
            if isinstance(ver, str) and "." in ver:
                ver_clean = ver.split("_", 1)[0]
                if ver_clean != "6.55.35":
                    state.firmware = ver_clean
            batt = obj.get("C")
            if isinstance(batt, str) and "_" in batt:
                parts = batt.split("_", 1)
                if parts[0].isdigit() and parts[1].isdigit():
                    lb, rb = int(parts[0]), int(parts[1])
                    if state.left_battery.level is None and 0 < lb <= 100:
                        state.left_battery.level = lb
                        state.left_battery.stale = False
                    if state.right_battery.level is None and 0 < rb <= 100:
                        state.right_battery.level = rb
                        state.right_battery.stale = False
            return "telemetry"
        return "ignored"

    if cmd == CMD_PEER_TIME_REQUEST:
        return "time_request"

    if cmd == CMD_ACK_FIND_DEVICE:
        return "find_device"

    return f"cmd_0x{cmd:04x}"


def _update_battery_slot(slot: BatteryReading, raw_pct: int, charging: bool) -> None:
    if 0 <= raw_pct <= 100:
        slot.level = raw_pct
        slot.charging = charging
        slot.stale = False
    else:
        slot.charging = False
        slot.stale = True


def _decode_ear_wear(flags: int, in_case_bit: int, worn_bit: int) -> str:
    if flags & worn_bit:
        return "worn"
    if flags & in_case_bit:
        return "in_case"
    return "removed"


def parse_multi_connect_devices(payload: bytes) -> Optional[List[MultiConnectDevice]]:
    if len(payload) < 2 or payload[0] != 0:
        return None
    count = payload[1]
    offset = 2
    out: List[MultiConnectDevice] = []
    for _ in range(count):
        if offset + 11 > len(payload):
            return None
        mac = bytes(payload[offset : offset + 6])
        raw3 = bytes(payload[offset + 6 : offset + 9])
        status = payload[offset + 9]
        name_len = payload[offset + 10]
        if offset + 11 + name_len > len(payload):
            return None
        name = payload[offset + 11 : offset + 11 + name_len].decode("utf-8", errors="replace")
        out.append(MultiConnectDevice(mac=mac, raw3=raw3, status=status, name=name))
        offset += 11 + name_len
    return out


def parse_telemetry_payload(payload: bytes) -> Optional[dict]:
    start = payload.find(b"{")
    end = payload.rfind(b"}")
    if start < 0 or end <= start:
        return None
    try:
        return json.loads(payload[start : end + 1].decode("utf-8", errors="replace"))
    except ValueError:
        return None
