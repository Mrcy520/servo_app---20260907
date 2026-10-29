from __future__ import annotations

import struct
from dataclasses import dataclass

from .constants import FRAME_HEADER, MAX_PAYLOAD, PROTOCOL_VERSION, FrameType
from .crc import crc16_ccitt_false


@dataclass(frozen=True, slots=True)
class Frame:
    version: int
    frame_type: FrameType
    sequence: int
    command: int
    payload: bytes

    def to_bytes(self) -> bytes:
        return encode_frame(
            self.frame_type,
            self.sequence,
            self.command,
            self.payload,
            self.version,
        )


def encode_frame(
    frame_type: FrameType | int,
    sequence: int,
    command: int,
    payload: bytes = b"",
    version: int = PROTOCOL_VERSION,
) -> bytes:
    payload = bytes(payload)
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)} > {MAX_PAYLOAD}")
    if not 0 <= sequence <= 0xFFFF:
        raise ValueError("sequence must be 0..65535")
    if not 0 <= command <= 0xFFFF:
        raise ValueError("command must be 0..65535")

    body = struct.pack(
        "<BBHHH",
        version,
        int(frame_type),
        sequence,
        command,
        len(payload),
    ) + payload
    crc = crc16_ccitt_false(body)
    return FRAME_HEADER + body + struct.pack("<H", crc)


class FrameDecoder:
    """Incremental decoder tolerant of fragmented, joined and noisy input."""

    def __init__(self, max_payload: int = MAX_PAYLOAD) -> None:
        self._buffer = bytearray()
        self.max_payload = max_payload
        self.crc_errors = 0
        self.length_errors = 0
        self.discarded_bytes = 0

    def reset(self) -> None:
        self._buffer.clear()
        self.crc_errors = 0
        self.length_errors = 0
        self.discarded_bytes = 0

    def feed(self, data: bytes | bytearray | memoryview) -> list[Frame]:
        if data:
            self._buffer.extend(data)
        frames: list[Frame] = []

        while True:
            header_index = self._buffer.find(FRAME_HEADER)
            if header_index < 0:
                keep = 1 if self._buffer.endswith(FRAME_HEADER[:1]) else 0
                discarded = len(self._buffer) - keep
                if discarded > 0:
                    del self._buffer[:discarded]
                    self.discarded_bytes += discarded
                break
            if header_index > 0:
                del self._buffer[:header_index]
                self.discarded_bytes += header_index

            if len(self._buffer) < 10:
                break

            version, raw_type, sequence, command, length = struct.unpack_from(
                "<BBHHH", self._buffer, 2
            )
            if length > self.max_payload:
                del self._buffer[0]
                self.length_errors += 1
                continue

            total_length = 12 + length
            if len(self._buffer) < total_length:
                break

            candidate = bytes(self._buffer[:total_length])
            received_crc = struct.unpack_from("<H", candidate, total_length - 2)[0]
            calculated_crc = crc16_ccitt_false(candidate[2:-2])
            if received_crc != calculated_crc:
                del self._buffer[0]
                self.crc_errors += 1
                continue

            try:
                frame_type = FrameType(raw_type)
            except ValueError:
                del self._buffer[:total_length]
                self.discarded_bytes += total_length
                continue

            frames.append(
                Frame(
                    version=version,
                    frame_type=frame_type,
                    sequence=sequence,
                    command=command,
                    payload=candidate[10:-2],
                )
            )
            del self._buffer[:total_length]

        return frames

