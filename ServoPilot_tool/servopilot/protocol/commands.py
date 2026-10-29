from __future__ import annotations

import math
import struct

from .constants import FeedbackSource, ManualMode, PositionEndMode, SectionMask


def _finite(value: float, label: str) -> float:
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{label}必须是有限数值")
    return value


def pack_hello(client_nonce: int) -> bytes:
    return struct.pack("<I", client_nonce & 0xFFFFFFFF)


def pack_status_mask(mask: SectionMask | int = SectionMask.ALL) -> bytes:
    return struct.pack("<I", int(mask))


def pack_telemetry(mask: SectionMask | int, period_ms: int) -> bytes:
    if not 0 <= period_ms <= 60_000:
        raise ValueError("状态上传周期必须为0～60000ms")
    return struct.pack("<IH", int(mask), period_ms)


def pack_watchdog(timeout_ms: int) -> bytes:
    if not 0 <= timeout_ms <= 60_000:
        raise ValueError("看门狗超时必须为0～60000ms")
    return struct.pack("<H", timeout_ms)


def pack_manual(mode: ManualMode | int, raw: int) -> bytes:
    if int(mode) not in (1, 2):
        raise ValueError("手动控制模式必须为电压或电流")
    if not -32767 <= raw <= 32767:
        raise ValueError("原始指令范围为-32767～32767")
    return struct.pack("<Bh", int(mode), raw)


def pack_raw(raw: int) -> bytes:
    if not -32767 <= raw <= 32767:
        raise ValueError("原始指令范围为-32767～32767")
    return struct.pack("<h", raw)


def pack_float(value: float, label: str = "参数") -> bytes:
    return struct.pack("<f", _finite(value, label))


def pack_feedback(source: FeedbackSource | int) -> bytes:
    if int(source) not in (0, 1):
        raise ValueError("反馈源必须为PCU或陀螺仪")
    return struct.pack("<B", int(source))


def pack_end_mode(mode: PositionEndMode | int) -> bytes:
    if int(mode) not in (0, 1):
        raise ValueError("位置结束模式必须为失能或保持")
    return struct.pack("<B", int(mode))


def pack_sine(base_rpm: float, amplitude_rpm: float, frequency_hz: float) -> bytes:
    base = _finite(base_rpm, "基准速度")
    amplitude = _finite(amplitude_rpm, "正弦幅值")
    frequency = _finite(frequency_hz, "正弦频率")
    if amplitude < 0.0:
        raise ValueError("正弦幅值不能为负数")
    if not 0.01 <= frequency <= 50.0:
        raise ValueError("正弦频率范围为0.01～50Hz")
    if base - amplitude < -100000.0 or base + amplitude > 100000.0:
        raise ValueError("正弦速度范围超出±100000rpm")
    return struct.pack("<fff", base, amplitude, frequency)


def decode_response(payload: bytes) -> tuple[int, int, bytes]:
    if len(payload) < 2:
        raise ValueError("应答Payload少于2字节")
    return payload[0], payload[1], payload[2:]

