import struct
import unittest

from protocol import build_frame, parse_frame


class ProtocolTests(unittest.TestCase):
    def test_big_endian_frame_matches_firmware_layout(self):
        frame = build_frame((1.0, -2.5, 3.25), (0.1, 0.0, 1.0), 25.34, 1, "big")
        self.assertEqual(len(frame), 32)
        self.assertEqual(frame[:4], bytes.fromhex("AA 55 01 20"))
        self.assertEqual(frame[4:8], struct.pack(">f", 1.0))
        self.assertEqual(frame[28:30], struct.pack(">h", 2534))
        self.assertEqual(frame[30], 1)
        self.assertEqual(frame[31], sum(frame[:-1]) & 0xFF)
        decoded = parse_frame(frame, "big")
        self.assertEqual(decoded.counter, 1)
        self.assertAlmostEqual(decoded.temperature, 25.34, places=3)

    def test_little_endian_round_trip(self):
        frame = build_frame((1.0, 2.0, 3.0), (4.0, 5.0, 6.0), -10.25, 200, "little")
        self.assertEqual(frame[4:8], struct.pack("<f", 1.0))
        decoded = parse_frame(frame, "little")
        self.assertEqual(decoded.gyro, (1.0, 2.0, 3.0))
        self.assertEqual(decoded.accel, (4.0, 5.0, 6.0))
        self.assertEqual(decoded.counter, 200)

    def test_checksum_rejects_corruption(self):
        frame = bytearray(build_frame((0, 0, 0), (0, 0, 1), 0, 1))
        frame[10] ^= 0x01
        with self.assertRaisesRegex(ValueError, "checksum"):
            parse_frame(frame)

