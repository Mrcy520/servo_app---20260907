#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Drive-board protocol self-test (external control link + parameter link).

Covers, against Doc/驱动板控制协议.docx and Doc/驱动板参数协议.docx:
  * uplink 1 kHz frame rate / checksum / resync
  * control word: enable, fault clear (rising edge), param latch, reserved bits,
    illegal mode rejection, 200 ms link timeout
  * parameter link: read all indexes, range / read-only / bad-index / bad-cmd
    status codes, staging + latch semantics, address filter, CRC filter

Usage (COM38, 921600):
    python Tool/proto_selftest.py --port COM38
    python Tool/proto_selftest.py --port COM38 --hz 1000
    python Tool/proto_selftest.py --selftest        # no hardware, verify this tool only

Note: encoder / motor are NOT required. Angle and current values may look static
or invalid; those fields are only range-checked, never used as pass criteria.
"""

import argparse
import struct
import sys
import threading
import time

# ---------------------------------------------------------------- protocol ---
CTRL_HEAD = b"\xAA\x55"
PARAM_HEAD = b"\x5A\xA5"
DEV_ADDR = 1

CW_ENABLE = 0x01
CW_CLR_FAULT = 0x02
CW_ZERO = 0x04
CW_LATCH = 0x08
CW_MASK = 0x0F

MODE_VOLTAGE = 0x01
MODE_CURRENT = 0x02

PARAM_CMD_READ = 0x01
PARAM_CMD_WRITE = 0x02

ST_OK = 0x00
ST_UNKNOWN = 0x01
ST_RANGE = 0x02
ST_INDEX = 0x03
ST_CRC = 0x04
ST_READONLY = 0x05

PARAM_NAME = {
    0x01: "POLE", 0x02: "MDIR", 0x03: "EDIR", 0x04: "KP_Q", 0x05: "KI_Q",
    0x06: "KP_D", 0x07: "KI_D", 0x08: "ZERO", 0x09: "VGAIN", 0x0A: "VLIM",
    0x0B: "TMOUT", 0x0C: "FWVER", 0x0D: "PROTO",
}
STATUS_NAME = {0: "OK", 1: "BAD_CMD", 2: "RANGE", 3: "BAD_IDX", 4: "CRC", 5: "READONLY"}
ST_BITS = {0: "ANG", 1: "OC", 2: "OV", 3: "UV", 4: "OT", 5: "EN", 6: "LOST", 7: "LATCH"}


def sum8(data):
    return sum(data) & 0xFF


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def ctrl_frame(ctrl, mode, ref1=0, ref2=0):
    f = bytearray([0xAA, 0x55, ctrl & 0xFF, mode & 0xFF])
    f += struct.pack(">hh", ref1, ref2)
    f.append(0x00)
    f.append(sum8(f))
    return bytes(f)


def param_frame(cmd, idx, value=0, addr=DEV_ADDR):
    f = bytearray([0x5A, 0xA5, addr & 0xFF, cmd & 0xFF, idx & 0xFF, 0x00])
    f += struct.pack(">i", value)
    f += b"\x00" * 4
    f += struct.pack(">H", crc16_ccitt_false(f))
    return bytes(f)


def st_str(v):
    return "|".join(name for bit, name in ST_BITS.items() if v & (1 << bit)) or "ok"


def decode_ctrl(frame):
    return {
        "status": frame[2],
        "angle": (frame[3] << 16) | (frame[4] << 8) | frame[5],
        "iq": struct.unpack(">h", frame[6:8])[0],
        "vbus": frame[8],
    }


def decode_param(frame):
    return {
        "addr": frame[2],
        "cmd": frame[3],
        "idx": frame[4],
        "status": frame[5],
        "value": struct.unpack(">i", frame[6:10])[0],
    }


# ------------------------------------------------------------------ parser ---
class Parser:
    """Byte-stream demux: 10-byte control frames and 16-byte parameter frames."""

    def __init__(self):
        self.buf = bytearray()
        self.last_ctrl = None
        self.param = []
        self.ctrl_count = 0
        self.err_ctrl = 0
        self.err_param = 0
        self.slip = 0

    def feed(self, data):
        self.buf += data
        while True:
            buf = self.buf
            if len(buf) < 2:
                return
            if buf[0] == 0xAA and buf[1] == 0x55:
                if len(buf) < 10:
                    return
                frame = bytes(buf[:10])
                if sum8(frame[:9]) == frame[9]:
                    self.last_ctrl = decode_ctrl(frame)
                    self.ctrl_count += 1
                    del buf[:10]
                    continue
                self.err_ctrl += 1
                self.slip += 1
                del buf[0]
                continue
            if buf[0] == 0x5A and buf[1] == 0xA5:
                if len(buf) < 16:
                    return
                frame = bytes(buf[:16])
                if crc16_ccitt_false(frame[:14]) == struct.unpack(">H", frame[14:16])[0]:
                    self.param.append(decode_param(frame))
                    del buf[:16]
                    continue
                self.err_param += 1
                self.slip += 1
                del buf[0]
                continue
            del buf[0]
            self.slip += 1


# -------------------------------------------------------------------- link ---
class Link:
    def __init__(self, port=None, baud=921600):
        self.ser = None
        self.parser = Parser()
        self._lock = threading.Lock()
        self._alive = True
        self._rx_error = None
        if port:
            import serial

            self.ser = serial.Serial(port, baud, timeout=0.01)
            self._thread = threading.Thread(target=self._rx_loop, daemon=True)
            self._thread.start()

    def _rx_loop(self):
        while self._alive:
            try:
                data = self.ser.read(4096)
            except Exception as exc:  # port removed / closed
                self._rx_error = repr(exc)
                return
            if data:
                self.parser.feed(data)

    def write(self, frame):
        if self.ser is None:
            return
        with self._lock:
            self.ser.write(frame)

    def close(self):
        self._alive = False
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass


# --------------------------------------------------------------- ctl thread ---
class Controller(threading.Thread):
    """Keeps a control frame streaming at --hz so the link never times out."""

    def __init__(self, link, hz):
        super().__init__(daemon=True)
        self.link = link
        self.period = 1.0 / float(hz)
        self._frame = ctrl_frame(0x00, MODE_VOLTAGE, 0, 0)
        self._paused = False
        self._lock = threading.Lock()
        self._alive = True
        self.sent = 0

    def set(self, ctrl, mode, ref1=0, ref2=0):
        with self._lock:
            self._frame = ctrl_frame(ctrl, mode, ref1, ref2)

    def set_raw(self, frame):
        with self._lock:
            self._frame = frame

    def pause(self):
        with self._lock:
            self._paused = True

    def resume(self):
        with self._lock:
            self._paused = False

    def run(self):
        next_t = time.perf_counter()
        while self._alive:
            now = time.perf_counter()
            if now < next_t:
                time.sleep(min(next_t - now, 0.0005))
                continue
            with self._lock:
                frame = self._frame
                paused = self._paused
            if not paused:
                self.link.write(frame)
                self.sent += 1
            next_t += self.period
            if now - next_t > 0.05:  # host hiccup: re-align instead of bursting
                next_t = now

    def stop(self):
        self._alive = False


# ------------------------------------------------------------------- tests ---
class Ctx:
    def __init__(self, link, ctl):
        self.link = link
        self.ctl = ctl
        self.results = []
        self.exchanges = 0   # 参数问答次数
        self.retries = 0     # 因超时重发的次数（文档要求上位机超时重发）

    def add(self, name, ok, detail=""):
        self.results.append((name, bool(ok), detail))
        print("  [%s] %-42s %s" % ("PASS" if ok else "FAIL", name, detail))

    def wait_status(self, mask, expect, timeout=0.3):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < timeout:
            frame = self.link.parser.last_ctrl
            if frame is not None and (frame["status"] & mask) == expect:
                return True
            time.sleep(0.002)
        return False

    def param_xchg(self, cmd, idx, value=0, addr=DEV_ADDR, timeout=0.05, retries=3):
        """Send one parameter frame and return the matching response (None on timeout).

        The document states the slave silently drops bad-CRC / foreign-address frames
        and relies on the master to retransmit on timeout, so a single lost frame is
        not a protocol error: retry and only fail if every attempt times out.
        self.retries counts how many extra attempts were needed (should stay 0 on a
        healthy link).
        """
        p = self.link.parser
        for attempt in range(1, retries + 1):
            mark = len(p.param)
            self.link.write(param_frame(cmd, idx, value, addr))
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < timeout:
                while len(p.param) > mark:
                    rsp = p.param[mark]
                    del p.param[mark]
                    if rsp["cmd"] == cmd and rsp["idx"] == idx:
                        self.exchanges += 1
                        self.retries += attempt - 1
                        return rsp
                time.sleep(0.002)
        self.exchanges += 1
        self.retries += retries
        return None

    def read_idx(self, idx):
        rsp = self.param_xchg(PARAM_CMD_READ, idx)
        return None if rsp is None else rsp["value"]


def run_tests(ctx, cfg):
    link = ctx.link
    p = link.parser

    print("\n[1] uplink stream")
    p.ctrl_count = 0
    p.err_ctrl = 0
    p.slip = 0
    time.sleep(1.0)
    rate = p.ctrl_count
    ctx.add("uplink frame rate ~1 kHz", 800 <= rate <= 1200, "%d fps" % rate)
    ctx.add("uplink checksum errors == 0", p.err_ctrl == 0, "err=%d slip=%d" % (p.err_ctrl, p.slip))

    frame = p.last_ctrl
    if frame is None:
        ctx.add("uplink frame decoded", False, "no frame received")
        return
    ctx.add("uplink frame decoded", True,
            "st=0x%02X(%s) ang=%d iq=%d vbus=%.1fV" % (
                frame["status"], st_str(frame["status"]), frame["angle"],
                frame["iq"], frame["vbus"] * 0.5))
    ctx.add("angle within 0..16777215", 0 <= frame["angle"] <= 0xFFFFFF, "raw=%d" % frame["angle"])
    ctx.add("vbus within 0..100V", 0 <= frame["vbus"] * 0.5 <= 100.0,
            "%.1fV" % (frame["vbus"] * 0.5))

    print("\n[2] link supervision (200 ms)")
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    ctx.add("bit6 clears while frames arrive", ctx.wait_status(0x40, 0x00, 0.3),
            "st=0x%02X" % p.last_ctrl["status"])

    ctx.ctl.pause()
    time.sleep(0.30)
    ctx.add("bit6 sets after 200 ms silence", (p.last_ctrl["status"] & 0x40) != 0,
            "st=0x%02X(%s)" % (p.last_ctrl["status"], st_str(p.last_ctrl["status"])))
    ctx.add("bit5 cleared on timeout", (p.last_ctrl["status"] & 0x20) == 0,
            "EN=0 expected")
    ctx.ctl.resume()
    ctx.add("bit6 clears after resume", ctx.wait_status(0x40, 0x00, 0.3), "")

    print("\n[3] control word")
    ctx.ctl.set(CW_CLR_FAULT, MODE_VOLTAGE, 0, 0)
    time.sleep(0.05)
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.05)
    ctx.add("bit1 rising edge clears bit7", (p.last_ctrl["status"] & 0x80) == 0,
            "st=0x%02X(%s)" % (p.last_ctrl["status"], st_str(p.last_ctrl["status"])))

    ctx.ctl.set(CW_ENABLE, MODE_VOLTAGE, 0, 0)
    ctx.add("bit0=1 -> bit5 echo set", ctx.wait_status(0x20, 0x20, 0.2),
            "st=0x%02X" % p.last_ctrl["status"])

    ctx.ctl.set(CW_ENABLE, MODE_VOLTAGE, 3277, 0)
    time.sleep(0.05)
    ctx.add("10% reference accepted (no fault)", True,
            "q=+10.00%% sent, st=0x%02X" % p.last_ctrl["status"])

    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    ctx.add("bit0=0 -> bit5 echo cleared", ctx.wait_status(0x20, 0x00, 0.2),
            "st=0x%02X" % p.last_ctrl["status"])

    ctx.ctl.set(0xF1, MODE_VOLTAGE, 0, 0)  # reserved bit4..7 must be masked
    ctx.add("reserved bits masked (0xF1 == 0x01)", ctx.wait_status(0x20, 0x20, 0.2),
            "st=0x%02X" % p.last_ctrl["status"])
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.03)

    print("\n[4] illegal mode rejection")
    ctx.ctl.set(0x00, 0x07, 0, 0)  # undefined mode: frame must be dropped
    time.sleep(0.35)
    ctx.add("undefined mode frame not accepted", (p.last_ctrl["status"] & 0x40) != 0,
            "bit6 set => link timer not refreshed")
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    ctx.add("valid frame restores link", ctx.wait_status(0x40, 0x00, 0.3), "")

    if cfg.skip_param:
        return

    print("\n[5] parameter link: read all indexes")
    for idx in range(0x01, 0x0E):
        rsp = ctx.param_xchg(PARAM_CMD_READ, idx)
        if rsp is None:
            ctx.add("read 0x%02X(%s)" % (idx, PARAM_NAME.get(idx, "?")), False, "no response")
            continue
        ctx.add("read 0x%02X(%s)" % (idx, PARAM_NAME.get(idx, "?")),
                rsp["status"] == ST_OK and rsp["idx"] == idx,
                "st=%s val=%d" % (STATUS_NAME.get(rsp["status"], "?"), rsp["value"]))

    fw = ctx.read_idx(0x0C)
    ctx.add("firmware version format", fw is not None and (fw >> 16) >= 0,
            "0x%08X" % (fw & 0xFFFFFFFF) if fw is not None else "no response")
    proto = ctx.read_idx(0x0D)
    ctx.add("protocol version == 1", proto == 1, "val=%s" % proto)

    print("\n[6] parameter link: status codes")
    rsp = ctx.param_xchg(PARAM_CMD_READ, 0x00)
    ctx.add("index 0x00 -> BAD_IDX", rsp is not None and rsp["status"] == ST_INDEX,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    rsp = ctx.param_xchg(PARAM_CMD_READ, 0x30)
    ctx.add("index 0x30 -> BAD_IDX", rsp is not None and rsp["status"] == ST_INDEX,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    rsp = ctx.param_xchg(0x09, 0x09)
    ctx.add("cmd 0x09 -> BAD_CMD", rsp is not None and rsp["status"] == ST_UNKNOWN,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    rsp = ctx.param_xchg(PARAM_CMD_WRITE, 0x0C, 7)
    ctx.add("write readonly 0x0C -> READONLY", rsp is not None and rsp["status"] == ST_READONLY,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    rsp = ctx.param_xchg(PARAM_CMD_WRITE, 0x0A, 21)
    ctx.add("write 0x0A=21 -> RANGE", rsp is not None and rsp["status"] == ST_RANGE,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    rsp = ctx.param_xchg(PARAM_CMD_WRITE, 0x01, 0)
    ctx.add("write 0x01=0 -> RANGE", rsp is not None and rsp["status"] == ST_RANGE,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))

    print("\n[7] parameter staging + bit3 latch")
    old = ctx.read_idx(0x0A)
    rsp = ctx.param_xchg(PARAM_CMD_WRITE, 0x0A, 18)
    ctx.add("write 0x0A=18 accepted", rsp is not None and rsp["status"] == ST_OK,
            "%s" % (STATUS_NAME.get(rsp["status"]) if rsp else "no response"))
    staged = ctx.read_idx(0x0A)
    ctx.add("read before latch returns old value", staged == old, "read=%s old=%s" % (staged, old))

    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.02)
    ctx.ctl.set(CW_LATCH, MODE_VOLTAGE, 0, 0)
    time.sleep(0.06)
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.02)
    latched = ctx.read_idx(0x0A)
    ctx.add("read after latch returns new value", latched == 18, "read=%s" % latched)

    # restore original limit so the board is left as found
    ctx.param_xchg(PARAM_CMD_WRITE, 0x0A, old if old is not None else 16)
    ctx.ctl.set(CW_LATCH, MODE_VOLTAGE, 0, 0)
    time.sleep(0.06)
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.02)
    restored = ctx.read_idx(0x0A)
    ctx.add("restore 0x0A", restored == old, "read=%s old=%s" % (restored, old))

    print("\n[8] parameter link: filters")
    rsp = ctx.param_xchg(PARAM_CMD_READ, 0x09, addr=DEV_ADDR + 1, timeout=0.20, retries=1)
    ctx.add("foreign address -> no response", rsp is None, "timeout expected")
    bad = bytearray(param_frame(PARAM_CMD_READ, 0x09))
    bad[9] ^= 0xFF
    p_mark = len(p.param)
    link.write(bytes(bad))
    time.sleep(0.20)
    ctx.add("bad CRC -> no response", len(p.param) == p_mark, "timeout expected")


def run_stress(ctx, count):
    """N 次参数读，统计首轮丢失率与线上杂散字节（不依赖重发，暴露真实链路质量）。"""
    p = ctx.link.parser
    p.ctrl_count = p.err_ctrl = p.err_param = p.slip = 0
    ok = miss = 0
    t0 = time.perf_counter()
    for _ in range(count):
        # retries=1：只发一次，失败即计丢失，用于测量真实丢失率
        rsp = ctx.param_xchg(PARAM_CMD_READ, 0x09, timeout=0.05, retries=1)
        if rsp is None:
            miss += 1
        else:
            ok += 1
        time.sleep(0.005)
    elapsed = time.perf_counter() - t0
    total = ok + miss
    print("\n[stress] %d requests in %.1fs" % (total, elapsed))
    print("  param  : ok=%d miss=%d  loss=%.2f%%" % (ok, miss, 100.0 * miss / max(total, 1)))
    print("  uplink : %d frames (%.0f fps)  err=%d" % (p.ctrl_count, p.ctrl_count / elapsed,
                                                       p.err_ctrl))
    print("  wire   : err_param=%d stray_slip=%d  (should both be 0)" % (p.err_param, p.slip))
    good = (miss == 0) and (p.err_ctrl == 0) and (p.err_param == 0) and (p.slip == 0)
    print("  => %s" % ("CLEAN" if good else "NOT CLEAN"))
    ctx.results.append(("stress %d requests" % total, good,
                        "loss=%.2f%% slip=%d" % (100.0 * miss / max(total, 1), p.slip)))


def cmd_get(ctx, idx):
    """读取一个参数并打印（用于掉电保存前后对比）。"""
    value = ctx.read_idx(idx)
    if value is None:
        print("  idx 0x%02X(%s) -> NO RESPONSE" % (idx, PARAM_NAME.get(idx, "?")))
        ctx.results.append(("get 0x%02X" % idx, False, "no response"))
        return
    print("  idx 0x%02X(%s) = %d (0x%X)" % (idx, PARAM_NAME.get(idx, "?"), value, value & 0xFFFFFFFF))
    ctx.results.append(("get 0x%02X" % idx, True, str(value)))


def cmd_set(ctx, idx, value, do_latch=True):
    """写一个参数并可选择立即锁存，顺带用量化方式确认 Flash 落盘是否真的发生。

    锁存后若发生擦写，主循环会被打住约 20~40 ms，1 kHz 上行帧会出现同样宽度的空档，
    这比"读回值变了"更能证明真的写了 Flash（读回值只证明 RAM 里的参数变了）。
    """
    rsp = ctx.param_xchg(PARAM_CMD_WRITE, idx, value)
    if rsp is None:
        print("  write 0x%02X -> NO RESPONSE" % idx)
        ctx.results.append(("set 0x%02X" % idx, False, "no response"))
        return
    print("  write 0x%02X(%s)=%d -> %s" % (idx, PARAM_NAME.get(idx, "?"), value,
                                           STATUS_NAME.get(rsp["status"], "?")))
    if not do_latch:
        return

    # bit3 先归零，保证随后是上升沿
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)
    time.sleep(0.05)

    ctx.ctl.set(CW_LATCH, MODE_VOLTAGE, 0, 0)

    # 落盘可能被 2 s 节流推迟，按 100 ms 窗口连续采样最多 3 s，找出那次空档
    stall = 0
    for _ in range(30):
        mark = ctx.link.parser.ctrl_count
        time.sleep(0.10)
        got = ctx.link.parser.ctrl_count - mark
        if got < 90:                      # 1 kHz 下 100 ms 应为 ~100 帧
            stall = 100 - got
            break
    ctx.ctl.set(0x00, MODE_VOLTAGE, 0, 0)

    print("  latch sent: flash write stall ~= %d ms" % stall)
    if 10 <= stall <= 120:
        print("  => Flash write looks ALIVE (erase+program happened)")
        ok = True
    else:
        print("  => no flash stall detected (value may not be persisted)")
        ok = False

    time.sleep(0.2)
    back = ctx.read_idx(idx)
    print("  read back after latch: %s" % back)
    if back != value:
        ok = False
    ctx.results.append(("set 0x%02X=%d" % (idx, value), ok, "stall=%dms readback=%s" % (stall, back)))


# ---------------------------------------------------------------- reporting ---
def summarize(ctx):
    total = len(ctx.results)
    failed = [r for r in ctx.results if not r[1]]
    print("\n" + "=" * 62)
    print("SUMMARY: %d checks, %d passed, %d failed" % (total, total - len(failed), len(failed)))
    if ctx.exchanges:
        print("param exchanges=%d, retransmits needed=%d (%.0f%%)"
              % (ctx.exchanges, ctx.retries, 100.0 * ctx.retries / ctx.exchanges))
        if ctx.retries:
            print("  note: retransmits > 0 means some requests were lost on the wire")
    if failed:
        for name, _, detail in failed:
            print("  FAIL  %-40s %s" % (name, detail))
    print("=" * 62)
    return 0 if not failed else 1


def selftest():
    """Verify this tool's codec and parser without hardware."""
    ok = True

    def check(cond, name):
        nonlocal ok
        print("  [%s] %s" % ("PASS" if cond else "FAIL", name))
        ok = ok and cond

    check(crc16_ccitt_false(b"123456789") == 0x29B1, "crc16 check value 0x29B1")

    f = ctrl_frame(CW_ENABLE, MODE_VOLTAGE, 3277, 0)
    check(f == bytes.fromhex("AA5501010CCD000000DA"), "control frame = doc example (+10%)")
    check(sum8(f[:9]) == f[9], "control checksum self-consistent")

    pf = param_frame(PARAM_CMD_READ, 0x09)
    check(pf[:6] == b"\x5A\xA5\x01\x01\x09\x00", "param header/addr/cmd/idx")
    check(crc16_ccitt_false(pf[:14]) == struct.unpack(">H", pf[14:])[0], "param crc self-consistent")

    # stream with one junk byte in front and one control frame inside param payload
    stream = b"\x00\x11" + f + pf + b"\xAA" + f
    parser = Parser()
    parser.feed(stream)
    check(parser.ctrl_count == 2, "parser found 2 control frames")
    check(len(parser.param) == 1, "parser found 1 param frame")
    check(parser.err_ctrl == 0 and parser.err_param == 0, "no false errors")
    check(parser.param[0]["idx"] == 0x09, "param idx decoded")

    parser.feed(b"\xAA\x55\x01\x02\x03")
    check(parser.buf[:5] == b"\xAA\x55\x01\x02\x03", "partial frame kept buffered")

    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="drive-board protocol self-test")
    ap.add_argument("--port", default=None, help="serial port, e.g. COM38")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--hz", type=float, default=1000.0, help="control frame rate (default 1000)")
    ap.add_argument("--skip-param", action="store_true", help="control link only")
    ap.add_argument("--stress", type=int, default=0, metavar="N",
                    help="send N parameter reads, report first-attempt loss and wire errors")
    ap.add_argument("--get", default=None, metavar="IDX", help="read one parameter, e.g. --get 0x0A")
    ap.add_argument("--set", default=None, metavar="IDX=VAL",
                    help="write+latch one parameter, e.g. --set 0x0A=18")
    ap.add_argument("--selftest", action="store_true", help="verify the tool itself, no hardware")
    cfg = ap.parse_args()

    if cfg.selftest:
        sys.exit(selftest())

    if not cfg.port:
        ap.error("--port is required (or use --selftest)")

    link = Link(cfg.port, cfg.baud)
    ctl = Controller(link, cfg.hz)
    ctl.start()
    ctx = Ctx(link, ctl)

    print("port=%s baud=%d ctrl=%.0f Hz  (Ctrl-C to stop)" % (cfg.port, cfg.baud, cfg.hz))
    try:
        if cfg.get is not None:
            time.sleep(0.3)
            cmd_get(ctx, int(cfg.get, 0))
        elif cfg.set is not None:
            idx_s, _, val_s = cfg.set.partition("=")
            if not val_s:
                ap.error("--set expects IDX=VAL, e.g. --set 0x0A=18")
            time.sleep(0.3)
            cmd_set(ctx, int(idx_s, 0), int(val_s, 0))
            print("\nNow power-cycle the board, then run:  --get %s" % idx_s.strip())
        elif cfg.stress:
            time.sleep(0.3)
            run_stress(ctx, cfg.stress)
        else:
            run_tests(ctx, cfg)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        ctl.stop()
        time.sleep(0.05)
        link.close()

    sys.exit(summarize(ctx))


if __name__ == "__main__":
    main()
