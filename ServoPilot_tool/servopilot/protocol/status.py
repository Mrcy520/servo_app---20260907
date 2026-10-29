from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any

from .constants import SectionId


@dataclass(slots=True)
class StatusSnapshot:
    sections: dict[SectionId, dict[str, Any]] = field(default_factory=dict)
    unknown_sections: dict[int, bytes] = field(default_factory=dict)

    def get(self, section: SectionId) -> dict[str, Any]:
        return self.sections.get(section, {})


def _unpack_exact(fmt: str, data: bytes) -> tuple[Any, ...]:
    expected = struct.calcsize(fmt)
    if len(data) < expected:
        raise ValueError(f"状态区长度不足：需要{expected}，实际{len(data)}")
    return struct.unpack_from(fmt, data)


def _parse_pcu(data: bytes) -> dict[str, Any]:
    values = _unpack_exact("<BBBBIIfIIII7s", data)
    keys = (
        "data_valid", "status_bits", "device_id", "reserved",
        "valid_frames", "angle_raw", "angle_deg", "checksum_errors",
        "format_errors", "tx_frames", "tx_errors", "last_tx_frame",
    )
    return dict(zip(keys, values))


def _parse_manual(data: bytes) -> dict[str, Any]:
    mode, requested, raw = _unpack_exact("<BBh", data)
    return {"staged_mode": mode, "output_requested": requested, "staged_raw": raw}


def _parse_position(data: bytes) -> dict[str, Any]:
    values = _unpack_exact("<BBBBiiifff", data)
    keys = (
        "state", "end_mode", "active", "reserved", "current_mdeg",
        "target_mdeg", "error_mdeg", "kp_rpm_per_deg", "max_speed_rpm",
        "speed_reference_rpm",
    )
    return dict(zip(keys, values))


def _parse_speed(data: bytes) -> dict[str, Any]:
    values = _unpack_exact("<BBBBffffffIffffffffhHIII", data)
    keys = (
        "state", "loop_mode", "sine_active", "feedback_source",
        "target_rpm", "reference_rpm", "feedback_rpm", "error_rpm",
        "pcu_differential_rpm", "gyro_rpm", "feedback_age_ms",
        "sine_base_rpm", "sine_amplitude_rpm", "sine_frequency_hz",
        "kp", "ki", "max_current_a", "current_command_a", "output_norm",
        "current_raw", "reserved", "control_updates", "feedback_timeouts",
        "command_errors",
    )
    return dict(zip(keys, values))


def _parse_gyro(data: bytes) -> dict[str, Any]:
    values = _unpack_exact("<BBBBIfffIIIIII", data)
    keys = (
        "data_valid", "bias_state", "bias_done", "reserved", "valid_frames",
        "gyro_x_raw_dps", "gyro_x_corrected_dps", "gyro_x_bias_dps",
        "calibration_samples", "calibration_total", "checksum_errors",
        "format_errors", "dropped_frames", "uart_overflow_count",
    )
    return dict(zip(keys, values))


def _parse_communication(data: bytes) -> dict[str, Any]:
    command_count, error_count, uptime_ms, boot_id = _unpack_exact("<IIII", data)
    return {
        "command_count": command_count,
        "error_count": error_count,
        "uptime_ms": uptime_ms,
        "boot_id": boot_id,
    }


_SECTION_PARSERS = {
    SectionId.PCU: _parse_pcu,
    SectionId.MANUAL: _parse_manual,
    SectionId.POSITION: _parse_position,
    SectionId.SPEED: _parse_speed,
    SectionId.GYRO: _parse_gyro,
    SectionId.COMMUNICATION: _parse_communication,
}


def parse_status_sections(data: bytes) -> StatusSnapshot:
    snapshot = StatusSnapshot()
    offset = 0
    while offset < len(data):
        if len(data) - offset < 3:
            raise ValueError("状态区头不完整")
        raw_id = data[offset]
        length = struct.unpack_from("<H", data, offset + 1)[0]
        offset += 3
        if length > len(data) - offset:
            raise ValueError("状态区长度超过剩余Payload")
        section_data = data[offset : offset + length]
        offset += length
        try:
            section_id = SectionId(raw_id)
        except ValueError:
            snapshot.unknown_sections[raw_id] = section_data
            continue
        parser = _SECTION_PARSERS.get(section_id)
        if parser is not None:
            snapshot.sections[section_id] = parser(section_data)
    return snapshot

