from __future__ import annotations

import struct
import unittest

from servopilot.protocol.codec import FrameDecoder, encode_frame
from servopilot.protocol.commands import decode_response, pack_sine
from servopilot.protocol.constants import Command, FrameType, SectionId
from servopilot.protocol.crc import crc16_ccitt_false
from servopilot.protocol.status import parse_status_sections


class CrcTests(unittest.TestCase):
    def test_standard_check_value(self) -> None:
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)


class FrameTests(unittest.TestCase):
    def test_encode_and_decode(self) -> None:
        payload = struct.pack("<f", 5.0)
        packet = encode_frame(FrameType.REQUEST, 1, Command.SPEED_SET_TARGET, payload)
        self.assertEqual(packet[:2], b"\xA5\x5A")
        self.assertEqual(packet[10:14], b"\x00\x00\xA0\x40")

        decoder = FrameDecoder()
        frames = decoder.feed(packet)
        self.assertEqual(len(frames), 1)
        frame = frames[0]
        self.assertEqual(frame.sequence, 1)
        self.assertEqual(frame.command, Command.SPEED_SET_TARGET)
        self.assertEqual(frame.payload, payload)

    def test_fragmented_and_joined_input(self) -> None:
        first = encode_frame(FrameType.REQUEST, 10, Command.PING)
        second = encode_frame(FrameType.REQUEST, 11, Command.SPEED_STOP)
        decoder = FrameDecoder()
        self.assertEqual(decoder.feed(first[:5]), [])
        frames = decoder.feed(first[5:] + second)
        self.assertEqual([frame.sequence for frame in frames], [10, 11])

    def test_noise_and_bad_crc_resynchronize(self) -> None:
        bad = bytearray(encode_frame(FrameType.REQUEST, 3, Command.PING))
        bad[-1] ^= 0xFF
        good = encode_frame(FrameType.REQUEST, 4, Command.GET_CAPABILITIES)
        decoder = FrameDecoder()
        frames = decoder.feed(b"noise" + bad + good)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].sequence, 4)
        self.assertEqual(decoder.crc_errors, 1)

    def test_decode_response(self) -> None:
        result, detail, data = decode_response(b"\x00\x00abc")
        self.assertEqual((result, detail, data), (0, 0, b"abc"))


class CommandTests(unittest.TestCase):
    def test_sine_parameters(self) -> None:
        payload = pack_sine(5.0, 2.0, 1.0)
        self.assertEqual(struct.unpack("<fff", payload), (5.0, 2.0, 1.0))

    def test_sine_rejects_invalid_frequency(self) -> None:
        with self.assertRaises(ValueError):
            pack_sine(0.0, 1.0, 100.0)


class StatusTests(unittest.TestCase):
    def test_parse_pcu_and_manual_sections(self) -> None:
        pcu = struct.pack(
            "<BBBBIIfIIII7s",
            1,
            0x04,
            0x55,
            0,
            1234,
            0x800000,
            180.0,
            1,
            2,
            30,
            0,
            b"\xAA\x55\x01\x02\x00\x00\x02",
        )
        manual = struct.pack("<BBh", 2, 1, 100)
        payload = (
            struct.pack("<BH", SectionId.PCU, len(pcu))
            + pcu
            + struct.pack("<BH", SectionId.MANUAL, len(manual))
            + manual
        )
        snapshot = parse_status_sections(payload)
        self.assertEqual(snapshot.get(SectionId.PCU)["device_id"], 0x55)
        self.assertAlmostEqual(snapshot.get(SectionId.PCU)["angle_deg"], 180.0)
        self.assertEqual(snapshot.get(SectionId.MANUAL)["staged_raw"], 100)

    def test_protocol_section_sizes(self) -> None:
        self.assertEqual(struct.calcsize("<BBBBIIfIIII7s"), 39)
        self.assertEqual(struct.calcsize("<BBh"), 4)
        self.assertEqual(struct.calcsize("<BBBBiiifff"), 28)
        self.assertEqual(struct.calcsize("<BBBBffffffIffffffffhHIII"), 80)
        self.assertEqual(struct.calcsize("<BBBBIfffIIIIII"), 44)
        self.assertEqual(struct.calcsize("<IIII"), 16)


if __name__ == "__main__":
    unittest.main()

