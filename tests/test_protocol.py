"""Unit tests using real GAIA frames captured from vivo TWS 5 Hi-Fi."""

from __future__ import annotations

import unittest

from kalm.protocol import (
    EarbudState,
    FrameDecoder,
    NoiseMode,
    PROFILE_FAMILY_V4,
    apply_frame_to_state,
    build_handshake,
    build_set_noise_mode,
)


class TestKalmProtocol(unittest.TestCase):
    def test_encode_noise_modes(self) -> None:
        self.assertEqual(build_handshake().hex(), "ff040000000a0300")
        self.assertEqual(
            build_set_noise_mode(NoiseMode.ANC, PROFILE_FAMILY_V4).hex(),
            "ff040003001b0130000301",
        )
        self.assertEqual(
            build_set_noise_mode(NoiseMode.OFF, PROFILE_FAMILY_V4).hex(),
            "ff040003001b0130010301",
        )
        self.assertEqual(
            build_set_noise_mode(NoiseMode.TRANSPARENCY, PROFILE_FAMILY_V4).hex(),
            "ff040003001b0130020301",
        )

    def test_decode_live_captured_stream(self) -> None:
        raw_stream = bytes.fromhex(
            "ff030005001b820700395bff00"
            "ff030002001b820d000c"
            "ff04020112001b8224"
            "0001117b226576656e744964223a22413130327c3130303937222c22706172616d73223a7b"
            "2274797065223a2230222c22505f4c223a2234222c22505f52223a2233222c22425f48223a"
            "2231222c22425f50223a2231222c22415f4d223a2231222c22415f4c223a2230222c225253"
            "53495f31223a222d3532222c22525353495f32223a2230222c22525353495f50223a222d37"
            "32222c224d5f41326470223a2231222c224d5f53636f223a2230222c2250223a22345f3422"
            "2c2242223a22315f31222c2241223a2231222c2252535349223a222d35355f305f305f2d37"
            "30227d2c2256223a22322e31302e35355f322e31302e3535222c2243223a2235375f393122"
            "2c2244223a22383836333432227d00"
            "ff030004001b823000010301"
        )

        dec = FrameDecoder()
        frames = []
        for i in range(0, len(raw_stream), 19):
            frames.extend(dec.feed(raw_stream[i : i + 19]))

        self.assertEqual(len(frames), 4)
        st = EarbudState(connected=True)
        for f in frames:
            apply_frame_to_state(f, st)

        self.assertEqual(st.left_battery.level, 57)
        self.assertEqual(st.right_battery.level, 91)
        self.assertIsNone(st.case_battery.level)
        self.assertTrue(st.case_battery.stale)
        self.assertEqual(st.left_wear, "worn")
        self.assertEqual(st.right_wear, "worn")
        self.assertEqual(st.firmware, "2.10.55")
        self.assertEqual(st.noise_mode, NoiseMode.OFF)

    def test_decode_multipoint(self) -> None:
        mp_hex = (
            "ff03003f001b8249"
            "000338d57ae9cc8e7c010c02066665646f7261"
            "94f6f27d42d25a420c000d484f4e4f52203230302050726f"
            "505a651efa1e4c010c0009737465616d6465636b"
        )
        dec = FrameDecoder()
        frames = dec.feed(bytes.fromhex(mp_hex))
        self.assertEqual(len(frames), 1)

        st = EarbudState(connected=True)
        apply_frame_to_state(frames[0], st)
        self.assertEqual(len(st.paired_devices), 3)
        self.assertEqual(st.paired_devices[0].name, "fedora")
        self.assertTrue(st.paired_devices[0].active)


if __name__ == "__main__":
    unittest.main()
