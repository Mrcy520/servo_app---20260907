"""GSA200 32-byte frame builder/parser.

The firmware currently parses the six float values and the temperature as
big-endian values.  The UI exposes little-endian as an optional compatibility
mode for devices that follow the wording in the supplied protocol image.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Iterable, Sequence

FRAME_SIZE = 32
HEADER = bytes((0xAA, 0x55))
FORMAT_ID = 0x01
TEMPERATURE_SCALE = 100.0


@dataclass(frozen=True)
class SensorFrame:
    gyro: tuple[float, float, float]
    accel: tuple[float, float, float]
    temperature: float
    counter: int
    checksum: int


def _byteorder(endian: str) -> str:
    if endian in ("big", ">", "be"):
        return ">"
    if endian in ("little", "<", "le"):
        return "<"
    raise ValueError("endian must be 'big' or 'little'")


def _triplet(values: Iterable[float], label: str) -> tuple[float, float, float]:
    result = tuple(float(value) for value in values)
    if len(result) != 3:
        raise ValueError(f"{label} must contain exactly 3 values")
    return result  # type: ignore[return-value]


def build_frame(
    gyro: Sequence[float],
    accel: Sequence[float],
    temperature: float,
    counter: int,
    endian: str = "big",
) -> bytes:
    """Build one 32-byte frame and append the additive checksum."""

    order = _byteorder(endian)
    gyro_values = _triplet(gyro, "gyro")
    accel_values = _triplet(accel, "accel")
    counter_value = int(counter) & 0xFF
    temperature_raw = int(round(float(temperature) * TEMPERATURE_SCALE))
    if not -32768 <= temperature_raw <= 32767:
        raise ValueError("temperature must fit the signed int16 scaled by 100")

    frame = bytearray(HEADER + bytes((FORMAT_ID, FRAME_SIZE)))
    for value in (*gyro_values, *accel_values):
        frame.extend(struct.pack(order + "f", value))
    frame.extend(struct.pack(order + "h", temperature_raw))
    frame.append(counter_value)
    frame.append(sum(frame) & 0xFF)
    if len(frame) != FRAME_SIZE:
        raise AssertionError(f"unexpected frame size: {len(frame)}")
    return bytes(frame)


def checksum(frame_without_checksum: bytes | bytearray) -> int:
    return sum(frame_without_checksum) & 0xFF


def parse_frame(frame: bytes | bytearray, endian: str = "big") -> SensorFrame:
    """Validate and decode a frame, raising ValueError on malformed data."""

    if len(frame) != FRAME_SIZE:
        raise ValueError(f"frame must be {FRAME_SIZE} bytes")
    if bytes(frame[:2]) != HEADER:
        raise ValueError("invalid frame header")
    if frame[2] != FORMAT_ID or frame[3] != FRAME_SIZE:
        raise ValueError("invalid format id or frame length")
    expected = checksum(frame[:-1])
    if frame[-1] != expected:
        raise ValueError(f"checksum mismatch: got 0x{frame[-1]:02X}, expected 0x{expected:02X}")

    order = _byteorder(endian)
    values = [struct.unpack(order + "f", bytes(frame[offset : offset + 4]))[0] for offset in range(4, 28, 4)]
    temperature_raw = struct.unpack(order + "h", bytes(frame[28:30]))[0]
    return SensorFrame(
        gyro=tuple(values[:3]),  # type: ignore[arg-type]
        accel=tuple(values[3:]),  # type: ignore[arg-type]
        temperature=temperature_raw / TEMPERATURE_SCALE,
        counter=frame[30],
        checksum=frame[31],
    )


def format_hex(frame: bytes | bytearray) -> str:
    return " ".join(f"{byte:02X}" for byte in frame)

