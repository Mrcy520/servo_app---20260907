# -*- coding: utf-8 -*-
"""
链路层自检（无需硬件）

用一个假串口模拟驱动器行为，验证 protocol.py + link.py 的收发、拆帧、
一问一答配对、超时重发、暂存/锁存语义与控制帧发送节奏。

运行：python selftest_link.py
"""

import struct
import sys
import threading
import time
import types

import protocol as P

# ---------------------------------------------------------------- 假串口 ----
_sim = None


class FakeSerial(object):
    """把写出的字节当成驱动器收到的请求，按协议回一帧应答。"""

    def __init__(self, *a, **kw):
        global _sim
        _sim = self
        self.is_open = True
        self.rx = bytearray()
        self.written = []
        self.cond = threading.Condition()
        self.params = dict((p.idx, 0) for p in P.PARAMS)
        self.params[0x09] = 193
        self.params[0x0A] = 16
        self.params[0x0B] = 200
        self.params[0x0C] = (1 << 16) | 0
        self.params[0x0D] = 1
        self.staged = {}
        self.drop_next = 0          # 让接下来 N 次请求不应答（模拟丢帧）
        self.auto_ctrl = True       # 是否回送上行控制帧

    # pyserial 兼容接口
    def reset_input_buffer(self):
        with self.cond:
            self.rx.clear()

    def close(self):
        self.is_open = False
        with self.cond:
            self.cond.notify_all()

    def write(self, data):
        data = bytes(data)
        self.written.append(data)
        with self.cond:
            self._handle(data)
            self.cond.notify_all()
        return len(data)

    def read(self, n):
        with self.cond:
            if not self.rx:
                self.cond.wait(0.02)
            if not self.rx:
                return b""
            out = bytes(self.rx[:n])
            del self.rx[:n]
            return out

    # 驱动器行为
    def _handle(self, data):
        if len(data) == P.PARAM_LEN and data[0] == P.PARAM_HEAD0:
            req = P.unpack_param(data)
            if req is None:
                return
            if self.drop_next > 0:
                self.drop_next -= 1
                return
            rsp = dict(req)
            idx, cmd, val = req["idx"], req["cmd"], req["value"]
            p = P.PARAM_BY_IDX.get(idx)
            rsp["status"] = P.PARAM_ST_OK
            rsp["value"] = 0
            if p is None:
                rsp["status"] = P.PARAM_ST_INDEX
            elif cmd == P.PARAM_CMD_READ:
                rsp["value"] = self.params.get(idx, 0)
            elif cmd != P.PARAM_CMD_WRITE:
                rsp["status"] = P.PARAM_ST_UNKNOWN
            elif p.ro:
                rsp["status"] = P.PARAM_ST_READONLY
            elif not p.in_range(val):
                rsp["status"] = P.PARAM_ST_RANGE
            else:
                self.staged[idx] = val
            self.rx += P.pack_param(req["addr"], cmd, idx, rsp["value"], rsp["status"])
        elif len(data) == P.CTRL_DOWN_LEN and data[0] == P.CTRL_HEAD:
            if not self.auto_ctrl:
                return
            up = bytearray([P.CTRL_HEAD, data[1], 0x20, 0x00, 0x00, 0x00, 0x00, 0x64, 0x10])
            up.append(P.sum8(up))
            self.rx += bytes(up)

    def latch(self):
        for k, v in self.staged.items():
            self.params[k] = v
        self.staged.clear()


def install_fake_serial():
    mod = types.ModuleType("serial")
    mod.Serial = FakeSerial
    tools = types.ModuleType("serial.tools")
    lp = types.ModuleType("serial.tools.list_ports")
    lp.comports = lambda: []
    tools.list_ports = lp
    mod.tools = tools
    sys.modules["serial"] = mod
    sys.modules["serial.tools"] = tools
    sys.modules["serial.tools.list_ports"] = lp


# ------------------------------------------------------------------ 测试 ----
def main():
    install_fake_serial()
    import link as L

    ok = True

    def check(cond, name, detail=""):
        nonlocal ok
        print("  [%s] %-44s %s" % ("PASS" if cond else "FAIL", name, detail))
        ok = ok and bool(cond)

    print("[1] 连接与上行帧接收")
    lk = L.SerialLink("FAKE", 921600, 1)
    lk.open()
    sender = L.ControlSender(lk, hz=500)
    sender.set_enable(True)
    sender.set_refs(10.0, 0.0)
    sender.start()
    time.sleep(0.2)
    st = lk.stats
    check(st.ctrl_rx > 20, "上行帧被解析", "%d 帧" % st.ctrl_rx)
    check(st.ctrl_err == 0 and st.slip == 0, "无错帧/无滑窗", "err=%d slip=%d" % (st.ctrl_err, st.slip))
    check(st.last_ctrl is not None and st.last_ctrl["status"] == 0x20, "状态位解出 EN")
    check(st.alive(), "链路在线判定")
    check(_sim.written and _sim.written[-1][:4] == bytes.fromhex("AA550101"), "控制帧头/ID/控制字/模式",
          " ".join("%02X" % b for b in _sim.written[-1]))

    print("[2] 参数读")
    rsp = lk.read_param(0x09)
    check(rsp is not None and rsp["status"] == 0 and rsp["value"] == 193, "读 0x09 = 193")
    rsp = lk.read_param(0x30)
    check(rsp is not None and rsp["status"] == P.PARAM_ST_INDEX, "非法索引 -> 0x03")

    print("[3] 参数写 + 暂存语义")
    rsp = lk.write_param(0x0A, 18)
    check(rsp is not None and rsp["status"] == P.PARAM_ST_OK, "写 0x0A = 18 被接受")
    check(lk.read_param(0x0A)["value"] == 16, "未锁存前读到旧值 16")
    rsp = lk.write_param(0x0A, 21)
    check(rsp["status"] == P.PARAM_ST_RANGE, "越界 -> 0x02")
    rsp = lk.write_param(0x0C, 7)
    check(rsp["status"] == P.PARAM_ST_READONLY, "只读 -> 0x05")
    _sim.latch()
    check(lk.read_param(0x0A)["value"] == 18, "锁存后读到新值 18")

    print("[4] 超时重发")
    before = lk.stats.timeouts
    _sim.drop_next = 10
    t0 = time.perf_counter()
    rsp = lk.read_param(0x0B, timeout=0.05, retries=3)
    dt = time.perf_counter() - t0
    check(rsp is None, "全部尝试超时返回 None", "%.0f ms" % (dt * 1000))
    check(lk.stats.timeouts == before + 1, "超时计数 +1")
    _sim.drop_next = 0

    print("[5] 单帧丢失由重发兜住")
    before = lk.stats.retries
    _sim.drop_next = 1
    rsp = lk.read_param(0x0B, timeout=0.05, retries=3)
    check(rsp is not None and rsp["value"] == 200, "丢 1 帧后重发成功")
    check(lk.stats.retries == before + 1, "重发计数 +1")

    print("[6] 错帧与杂散字节")
    e0 = lk.stats.param_err
    bad = bytearray(P.pack_param(1, P.PARAM_CMD_READ, 0x09))
    bad[9] ^= 0xFF
    lk._feed(b"\x00\x11\x22" + bytes(bad) + P.pack_param(1, P.PARAM_CMD_READ, 0x09, 0))
    check(lk.stats.param_err == e0 + 1, "坏 CRC 计入 param_err")
    check(lk.stats.slip >= 3, "杂散字节被丢弃", "slip=%d" % lk.stats.slip)

    print("[7] 控制字脉冲")
    n0 = len(_sim.written)
    sender.pulse(P.CW_LATCH, 60)
    time.sleep(0.18)
    seen = [f[2] for f in _sim.written[n0:]]
    check(any(v & P.CW_LATCH for v in seen), "bit3 出现过",
          "控制字取值 %s" % sorted(set(seen)))
    check(seen[-1] & P.CW_LATCH == 0, "脉冲后回落到基帧（bit3=0）")
    check(all(f[8] == 0 for f in _sim.written[n0:]), "保留字节恒为 0")

    print("[8] 设定值换算与帧一致性")
    check(P.pct_to_ref(10.0) == 3277, "10% -> 3277")
    check(P.pack_ctrl_down(1, P.CW_ENABLE, P.MODE_VOLTAGE, 3277, 0).hex() == "aa5501010ccd000000da",
          "帧与文档示例一致")

    sender.stop()
    lk.close()
    print("\n%s" % ("ALL PASS" if ok else "SOME CHECKS FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
