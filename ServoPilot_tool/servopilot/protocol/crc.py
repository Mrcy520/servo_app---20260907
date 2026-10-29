from __future__ import annotations


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    """CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF."""

    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

