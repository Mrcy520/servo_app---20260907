# -*- coding: utf-8 -*-
"""
驱动板上位机 · 协议编解码层

与固件 Utility/Src/proto.c 逐字节对齐，两类帧共用一条串口：

    参数链路 16 B（本上位机的主体，见 Doc/驱动板参数协议.docx）
        5A A5 | 地址 | 命令 | 索引 | 状态 | 数值(int32 大端) | 保留(4) | CRC16(2 大端)
        CRC-16/CCITT-FALSE 覆盖字节 1~14，多项式 0x1021、初值 0xFFFF、不反射、不异或

    控制链路 10 B（参数锁存必须靠它：控制字 bit3 上升沿）
        AA | ID(0x54+站号) | 控制字 | 模式 | 值1(i16 大端) | 值2(i16 大端) | 保留 | 累加和

本模块只做"帧长什么样、怎么装、怎么拆"，不含收发调度（见 link.py）。
可脱离串口与界面单独自检：python protocol.py
"""

import struct

# ===== 参数链路 ============================================================
PARAM_LEN = 16
PARAM_HEAD0 = 0x5A
PARAM_HEAD1 = 0xA5
PARAM_CRC_SPAN = 14          # 对第 1~14 字节计算 CRC

PARAM_CMD_READ = 0x01
PARAM_CMD_WRITE = 0x02

PARAM_ST_OK = 0x00
PARAM_ST_UNKNOWN = 0x01      # 未知命令码
PARAM_ST_RANGE = 0x02        # 数值越界
PARAM_ST_INDEX = 0x03        # 非法索引
PARAM_ST_CRC = 0x04          # CRC 错误
PARAM_ST_READONLY = 0x05     # 只读参数

STATUS_NAME = {
    PARAM_ST_OK: "成功",
    PARAM_ST_UNKNOWN: "未知命令码",
    PARAM_ST_RANGE: "数值越界",
    PARAM_ST_INDEX: "非法索引",
    PARAM_ST_CRC: "CRC 错误",
    PARAM_ST_READONLY: "只读参数",
}

# ===== 控制链路 ============================================================
CTRL_UP_LEN = 10
CTRL_DOWN_LEN = 10
CTRL_HEAD = 0xAA
CTRL_ID_BASE = 0x54

# 上行状态位
ST_ANGLE_ERR = 0x01
ST_OVER_CUR = 0x02
ST_OVER_VOLT = 0x04
ST_UNDER_VOLT = 0x08
ST_OVER_TEMP = 0x10
ST_ENABLED = 0x20
ST_COMM_LOST = 0x40
ST_FAULT_LATCH = 0x80

STATUS_BITS = [
    (ST_ANGLE_ERR, "ANG", "角度传感器报警"),
    (ST_OVER_CUR, "OC", "过流"),
    (ST_OVER_VOLT, "OV", "过压"),
    (ST_UNDER_VOLT, "UV", "欠压"),
    (ST_OVER_TEMP, "OT", "过温"),
    (ST_ENABLED, "EN", "使能回读"),
    (ST_COMM_LOST, "LOST", "通讯超时"),
    (ST_FAULT_LATCH, "LATCH", "故障锁存"),
]

# 下行控制字
CW_ENABLE = 0x01             # 电平有效
CW_CLR_FAULT = 0x02          # 上升沿
CW_ZERO = 0x04               # 上升沿
CW_LATCH = 0x08              # 上升沿
CW_MASK = 0x0F               # bit4~7 为保留位，必须填 0

MODE_VOLTAGE = 0x01
MODE_CURRENT = 0x02

CTRL_TIMEOUT_MS = 200        # 下行超时门限（参数 0x0B 可改）
CTRL_REF_MAX = 32767
CTRL_REF_PER_PCT = 327.67

# 上行量纲
CTRL_IQ_LSB = 0.01           # A/LSB
CTRL_VBUS_LSB = 0.5          # V/LSB
CTRL_ANG_LSB = 360.0 / 16777216.0


# ===== 校验与组帧 ==========================================================

def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF, 不反射, 不异或。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte & 0xFF) << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def sum8(data):
    return sum(data) & 0xFF


def ctrl_id(addr):
    """控制链路 ID 码 = 0x54 + 站号（与参数链路地址共用同一编号）"""
    return (CTRL_ID_BASE + addr) & 0xFF


def pack_param(addr, cmd, idx, value=0, status=0):
    """组 16 字节参数帧。请求帧 status 填 0，保留区填 0。"""
    f = bytearray(PARAM_LEN)
    f[0] = PARAM_HEAD0
    f[1] = PARAM_HEAD1
    f[2] = addr & 0xFF
    f[3] = cmd & 0xFF
    f[4] = idx & 0xFF
    f[5] = status & 0xFF
    f[6:10] = struct.pack(">i", int(value))
    crc = crc16_ccitt_false(f[:PARAM_CRC_SPAN])
    f[14] = (crc >> 8) & 0xFF
    f[15] = crc & 0xFF
    return bytes(f)


def unpack_param(frame):
    """拆 16 字节参数帧；帧头或 CRC 不对时返回 None。"""
    if len(frame) < PARAM_LEN:
        return None
    if frame[0] != PARAM_HEAD0 or frame[1] != PARAM_HEAD1:
        return None
    crc_rx = (frame[14] << 8) | frame[15]
    if crc16_ccitt_false(frame[:PARAM_CRC_SPAN]) != crc_rx:
        return None
    return {
        "addr": frame[2],
        "cmd": frame[3],
        "idx": frame[4],
        "status": frame[5],
        "value": struct.unpack(">i", bytes(frame[6:10]))[0],
    }


def pack_ctrl_down(addr, ctrl, mode, ref1=0, ref2=0, rsv=0):
    """组 10 字节下行控制帧。"""
    f = bytearray(CTRL_DOWN_LEN)
    f[0] = CTRL_HEAD
    f[1] = ctrl_id(addr)
    f[2] = ctrl & 0xFF
    f[3] = mode & 0xFF
    f[4:6] = struct.pack(">h", int(ref1))
    f[6:8] = struct.pack(">h", int(ref2))
    f[8] = rsv & 0xFF
    f[9] = sum8(f[:CTRL_DOWN_LEN - 1])
    return bytes(f)


def unpack_ctrl_up(frame, addr=None):
    """拆 10 字节上行控制帧；校验/ID 不符返回 None。"""
    if len(frame) < CTRL_UP_LEN:
        return None
    if frame[0] != CTRL_HEAD:
        return None
    if addr is not None and frame[1] != ctrl_id(addr):
        return None
    if sum8(frame[:CTRL_UP_LEN - 1]) != frame[CTRL_UP_LEN - 1]:
        return None
    return {
        "status": frame[2],
        "angle": (frame[3] << 16) | (frame[4] << 8) | frame[5],
        "iq": struct.unpack(">h", bytes(frame[6:8]))[0],
        "vbus": frame[8],
    }


# ===== 设定值换算（与固件 proto.h 的 ctrl_pct_to_ref 一致）=================

def pct_to_ref(pct):
    """百分比 -> 设定值：四舍五入（远离零）+ 饱和到 ±32767。"""
    v = float(pct) * CTRL_REF_PER_PCT
    v = (v + 0.5) if v >= 0 else (v - 0.5)
    v = int(v)
    if v > CTRL_REF_MAX:
        v = CTRL_REF_MAX
    elif v < -CTRL_REF_MAX:
        v = -CTRL_REF_MAX
    return v


def ref_to_pct(ref):
    return float(ref) / CTRL_REF_PER_PCT


# ===== 参数表（Doc/驱动板参数协议.docx 表 3-1）==============================

class ParamDef(object):
    """一项参数的元数据：量程、单位、换算方式、读写属性。"""

    __slots__ = ("idx", "name", "rmin", "rmax", "unit", "ro", "kind", "hint")

    def __init__(self, idx, name, rmin, rmax, unit, ro, kind, hint):
        self.idx = idx
        self.name = name
        self.rmin = rmin
        self.rmax = rmax
        self.unit = unit
        self.ro = ro
        self.kind = kind      # int / q4 / deg / ver
        self.hint = hint

    def clamp(self, raw):
        return max(self.rmin, min(self.rmax, int(raw)))

    def in_range(self, raw):
        return self.rmin <= int(raw) <= self.rmax

    def to_phys(self, raw):
        if raw is None:
            return None
        if self.kind == "q4":
            return raw / 16.0
        if self.kind == "deg":
            return raw * 360.0 / 11796480.0
        if self.kind == "ver":
            return raw
        return raw

    def from_phys(self, phys):
        if self.kind == "q4":
            return int(round(phys * 16.0))
        if self.kind == "deg":
            return int(round(phys * 11796480.0 / 360.0))
        return int(round(phys))

    def phys_text(self, raw):
        if raw is None:
            return "—"
        if self.kind == "ver":
            return "%d.%d" % ((raw >> 16) & 0xFFFF, raw & 0xFFFF)
        v = self.to_phys(raw)
        if self.kind == "q4":
            return "%.4f" % v
        if self.kind == "deg":
            return "%.4f °" % v
        return str(v)


PARAMS = [
    ParamDef(0x01, "电机极对数", 1, 100, "整数", False, "int", "1 ~ 100"),
    ParamDef(0x02, "电机方向", 0, 1, "0 反转 / 1 正转", False, "int", "0 或 1"),
    ParamDef(0x03, "编码器方向", 0, 1, "0 反转 / 1 正转", False, "int", "0 或 1"),
    ParamDef(0x04, "Q 轴电流比例", 16, 1600, "×16", False, "q4", "物理值 1.0 ~ 100.0（写入值 = 物理值 × 16）"),
    ParamDef(0x05, "Q 轴电流积分", 16, 3200000, "×16", False, "q4", "物理值 1.0 ~ 200000.0"),
    ParamDef(0x06, "D 轴电流比例", 16, 1600, "×16", False, "q4", "物理值 1.0 ~ 100.0"),
    ParamDef(0x07, "D 轴电流积分", 16, 3200000, "×16", False, "q4", "物理值 1.0 ~ 200000.0"),
    ParamDef(0x08, "电机零位", 0, 11796480, "1/2^15 度", False, "deg", "0 ~ 11796480（360° 对应 11796480）"),
    ParamDef(0x09, "电压增益", 0, 500, "整数", False, "int", "可按 5408 / 母线电压 估算"),
    ParamDef(0x0A, "电压限幅", 0, 20, "整数", False, "int", "可按 母线电压 / 1.732 估算"),
    ParamDef(0x0B, "通讯超时", 10, 1000, "ms", False, "int", "10 ~ 1000 ms"),
    ParamDef(0x0C, "固件版本", 0, 0x7FFFFFFF, "高 16 位主 / 低 16 位次", True, "ver", "只读，由固件给出"),
    ParamDef(0x0D, "协议版本", 0, 0x7FFFFFFF, "固定 1", True, "int", "只读，固定为 1"),
]

PARAM_BY_IDX = dict((p.idx, p) for p in PARAMS)
PARAM_INDEX_MIN = min(p.idx for p in PARAMS)
PARAM_INDEX_MAX = max(p.idx for p in PARAMS)


# ===== 自检（无需硬件）=====================================================

def selftest():
    ok = True

    def check(cond, name, detail=""):
        nonlocal ok
        print("  [%s] %-46s %s" % ("PASS" if cond else "FAIL", name, detail))
        ok = ok and bool(cond)

    check(crc16_ccitt_false(b"123456789") == 0x29B1, "CRC 标准检查值 0x29B1")

    # 参数帧：字节布局 + CRC 覆盖范围
    f = pack_param(1, PARAM_CMD_READ, 0x09)
    check(f[:6] == b"\x5A\xA5\x01\x01\x09\x00", "参数帧 头/地址/命令/索引/状态", f[:6].hex(" "))
    check(len(f) == 16, "参数帧长度 16 B")
    check(crc16_ccitt_false(f[:14]) == struct.unpack(">H", f[14:])[0], "参数帧 CRC 自洽")
    check(f[10:14] == b"\x00\x00\x00\x00", "保留区填 0")
    check(unpack_param(f)["idx"] == 0x09, "参数帧可往返解出")

    # 数值大端：-1 应编码为 FF FF FF FF
    w = pack_param(1, PARAM_CMD_WRITE, 0x01, -1)
    check(w[6:10] == b"\xFF\xFF\xFF\xFF", "int32 大端（-1）")

    # 控制帧：文档示例 +10% -> AA 55 01 01 0C CD 00 00 00 DA
    c = pack_ctrl_down(1, CW_ENABLE, MODE_VOLTAGE, pct_to_ref(10.0), 0)
    check(c == bytes.fromhex("AA5501010CCD000000DA"), "控制帧 = 文档示例（+10%）", c.hex(" ").upper())
    check(ctrl_id(1) == 0x55 and ctrl_id(2) == 0x56, "ID 码 = 0x54 + 站号")
    check(sum8(c[:9]) == c[9], "控制帧累加和自洽")

    # 设定值取整
    check(pct_to_ref(10.0) == 3277, "10%% -> 3277 (0x0CCD)", str(pct_to_ref(10.0)))
    check(pct_to_ref(-100.0) == -32767, "饱和到 -32767")
    check(pct_to_ref(0.5) == 164, "0.5%% -> 四舍五入 164", str(pct_to_ref(0.5)))

    # 上行帧解码：AA | ID | 状态 | 角度(3) | iq(2) | 母线 | SUM
    up_raw = bytearray([CTRL_HEAD, ctrl_id(1), 0x00, 0x00, 0x00, 0x00, 0x0C, 0xCD, 0x10])
    up_raw.append(sum8(up_raw))
    up = unpack_ctrl_up(bytes(up_raw), addr=1)
    check(up is not None and up["iq"] == 3277 and up["vbus"] == 0x10, "上行帧解码（iq / 母线）")
    check(unpack_ctrl_up(bytes(up_raw), addr=2) is None, "站号不符的上行帧被丢弃")

    # 参数表换算
    p4 = PARAM_BY_IDX[0x04]
    check(p4.from_phys(1.0) == 16 and p4.to_phys(16) == 1.0, "0x04 ×16 换算")
    p8 = PARAM_BY_IDX[0x08]
    check(p8.from_phys(360.0) == 11796480, "0x08 满圈 = 11796480")
    check(PARAM_BY_IDX[0x0C].phys_text(0x00010000) == "1.0", "0x0C 版本号显示")

    print("\n%s" % ("ALL PASS" if ok else "SOME CHECKS FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(selftest())
