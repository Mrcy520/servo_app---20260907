from __future__ import annotations

from enum import IntEnum, IntFlag


PROTOCOL_VERSION = 0x01
FRAME_HEADER = b"\xA5\x5A"
MAX_PAYLOAD = 256
MIN_FRAME_SIZE = 12


class FrameType(IntEnum):
    REQUEST = 0x01
    RESPONSE = 0x02
    EVENT = 0x03


class Command(IntEnum):
    HELLO = 0x0001
    PING = 0x0002
    GET_CAPABILITIES = 0x0003
    GET_STATUS = 0x0010
    SET_TELEMETRY = 0x0011
    SET_WATCHDOG = 0x0012

    SET_MANUAL_COMMAND = 0x0100
    MANUAL_ENABLE = 0x0101
    OUTPUT_DISABLE = 0x0102
    CLEAR_FAULT = 0x0103
    AUTO_ZERO = 0x0104

    GYRO_BIAS_START = 0x0200

    POSITION_MOVE_RELATIVE = 0x0300
    POSITION_SET_END_MODE = 0x0301
    POSITION_SET_KP = 0x0302
    POSITION_SET_MAX_SPEED = 0x0303
    POSITION_STOP = 0x0304

    SPEED_SET_TARGET = 0x0400
    SPEED_SET_FEEDBACK = 0x0401
    SPEED_START = 0x0402
    SPEED_STOP = 0x0403
    SPEED_SET_SINE = 0x0404
    SPEED_SINE_START = 0x0405
    SPEED_SINE_STOP = 0x0406
    SPEED_SET_KP = 0x0407
    SPEED_SET_KI = 0x0408
    SPEED_SET_MAX_CURRENT = 0x0409

    FAULT_EVENT = 0x8001
    STATE_EVENT = 0x8002


COMMAND_NAMES = {command.value: command.name for command in Command}


class ResultCode(IntEnum):
    OK = 0x00
    UNSUPPORTED_VERSION = 0x01
    UNKNOWN_COMMAND = 0x02
    BAD_LENGTH = 0x03
    INVALID_PARAMETER = 0x04
    INVALID_STATE = 0x05
    NO_PCU_FEEDBACK = 0x06
    PCU_FAULT = 0x07
    GYRO_NOT_CALIBRATED = 0x08
    NO_GYRO_DATA = 0x09
    BUSY = 0x0A
    SEND_FAILED = 0x0B
    CURRENT_TEST_ACTIVE = 0x0C
    SEQUENCE_CONFLICT = 0x0D
    INTERNAL_ERROR = 0x0E
    FEEDBACK_TIMEOUT = 0x0F


RESULT_NAMES = {
    ResultCode.OK: "成功",
    ResultCode.UNSUPPORTED_VERSION: "协议版本不支持",
    ResultCode.UNKNOWN_COMMAND: "未知命令",
    ResultCode.BAD_LENGTH: "数据长度错误",
    ResultCode.INVALID_PARAMETER: "参数非法",
    ResultCode.INVALID_STATE: "当前状态不允许执行",
    ResultCode.NO_PCU_FEEDBACK: "没有有效PCU反馈",
    ResultCode.PCU_FAULT: "PCU存在故障",
    ResultCode.GYRO_NOT_CALIBRATED: "陀螺仪零偏校准未完成",
    ResultCode.NO_GYRO_DATA: "没有有效陀螺仪数据",
    ResultCode.BUSY: "设备忙",
    ResultCode.SEND_FAILED: "命令发送失败",
    ResultCode.CURRENT_TEST_ACTIVE: "纯电流测试模式正在运行",
    ResultCode.SEQUENCE_CONFLICT: "请求序号冲突",
    ResultCode.INTERNAL_ERROR: "控制器内部错误",
    ResultCode.FEEDBACK_TIMEOUT: "反馈超时",
}


class SectionMask(IntFlag):
    PCU = 1 << 0
    MANUAL = 1 << 1
    POSITION = 1 << 2
    SPEED = 1 << 3
    GYRO = 1 << 4
    COMMUNICATION = 1 << 5
    ALL = PCU | MANUAL | POSITION | SPEED | GYRO | COMMUNICATION


class SectionId(IntEnum):
    PCU = 0x01
    MANUAL = 0x02
    POSITION = 0x03
    SPEED = 0x04
    GYRO = 0x05
    COMMUNICATION = 0x06


class ManualMode(IntEnum):
    VOLTAGE_OPEN_LOOP = 1
    CURRENT_CLOSED_LOOP = 2


class FeedbackSource(IntEnum):
    PCU_DIFFERENTIAL = 0
    GYRO = 1


class PositionEndMode(IntEnum):
    DISABLE = 0
    HOLD = 1


class Capability(IntFlag):
    MANUAL_CONTROL = 1 << 0
    SPEED_LOOP = 1 << 1
    POSITION_LOOP = 1 << 2
    GYRO = 1 << 3
    SINE_SPEED = 1 << 4
    TELEMETRY = 1 << 5
    WATCHDOG = 1 << 6
    DUPLICATE_PROTECTION = 1 << 7

