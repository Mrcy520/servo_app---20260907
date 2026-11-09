# -*- coding: utf-8 -*-
"""
驱动板上位机 · 链路层

职责：
  1. 串口收发（后台线程）与字节流拆帧（10 B 控制帧 / 16 B 参数帧混流）
  2. 参数一问一答：按 (命令, 索引) 配对，超时自动重发（文档要求）
  3. 下行控制帧按固定频率持续发送，维持 200 ms 链路超时保护不动作

与固件分层对应：
  Utility/comm_proto.c  收发调度 · 寻址 · 超时   ->  本文件的 SerialLink / ControlSender
  Utility/proto.c       帧编解码                ->  protocol.py
"""

import collections
import sys
import threading
import time

PYSERIAL_OK = False
PYSERIAL_VER = ""
PYSERIAL_ERR = ""

try:
    import serial
    from serial.tools import list_ports as _list_ports
    PYSERIAL_OK = True
    PYSERIAL_VER = getattr(serial, "VERSION", "?")
except Exception as _exc:                            # 未安装 pyserial 时界面仍可打开
    serial = None
    _list_ports = None
    PYSERIAL_ERR = "%s: %s" % (type(_exc).__name__, _exc)

import protocol as P


def _ports_from_registry():
    """直接读 HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM，作为 pyserial 枚举失败时的兜底。"""
    if sys.platform != "win32":
        return []
    try:
        import winreg
        key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM")
    except Exception:
        return []
    out = []
    try:
        i = 0
        while True:
            try:
                name, val, _typ = winreg.EnumValue(key, i)
            except OSError:
                break
            i += 1
            if isinstance(val, str) and val.upper().startswith("COM"):
                out.append((val.upper(), name.split("\\")[-1]))
    finally:
        winreg.CloseKey(key)
    return out


def list_ports():
    """返回 [(设备名, 描述)]，已排序。

    pyserial 缺失或 SetupAPI 枚举不到时退回注册表查询 —— 两种情况原因不同，
    上层据 PYSERIAL_OK / PYSERIAL_ERR 区分提示，不要都当成"没有串口"。
    """
    out = []
    if PYSERIAL_OK:
        try:
            for p in _list_ports.comports():
                out.append((p.device, p.description or ""))
        except Exception:
            out = []
    if not out:
        out = _ports_from_registry()
    return sorted(out, key=lambda x: x[0])


class Stats(object):
    """链路统计：用于界面右上角与日志诊断。"""

    def __init__(self):
        self.ctrl_rx = 0        # 收到的合法上行帧
        self.ctrl_err = 0       # 帧头/校验/ID 不匹配
        self.param_rx = 0       # 收到的合法参数应答
        self.param_err = 0      # CRC 错误的参数帧
        self.slip = 0           # 丢弃的杂散字节
        self.tx_ctrl = 0        # 已发送的控制帧
        self.tx_param = 0       # 已发送的参数帧
        self.retries = 0        # 累计重发次数
        self.timeouts = 0       # 累计无应答次数
        self.last_ctrl = None
        self.last_raw = None
        self.last_ctrl_t = 0.0
        self._marks = collections.deque(maxlen=2000)

    def mark_rx(self, t):
        self.ctrl_rx += 1
        self._marks.append(t)

    def fps(self):
        if len(self._marks) < 2:
            return 0.0
        span = self._marks[-1] - self._marks[0]
        if span <= 0:
            return 0.0
        return (len(self._marks) - 1) / span

    def alive(self, timeout_ms=P.CTRL_TIMEOUT_MS):
        if self.last_ctrl_t <= 0:
            return False
        return (time.time() - self.last_ctrl_t) * 1000.0 < timeout_ms


class SerialLink(object):
    """一条串口 = 两类帧。参数请求同步阻塞，控制帧由 ControlSender 异步推。"""

    def __init__(self, port, baud, addr, log=None):
        self.port = port
        self.baud = baud
        self.addr = addr
        self.stats = Stats()
        self._log = log or (lambda *a, **k: None)
        self._ser = None
        self._rx_thread = None
        self._stop = threading.Event()
        self._wr_lock = threading.Lock()
        self._pend_lock = threading.Lock()
        self._pending = {}                  # (cmd, idx) -> [Event, box]
        self._buf = bytearray()
        self.error = ""

    # ---------------------------------------------------------------- 生命周期
    def open(self):
        if not PYSERIAL_OK:
            raise RuntimeError("未安装 pyserial，请执行：pip install pyserial")
        self._ser = serial.Serial(self.port, self.baud, timeout=0.02,
                                  bytesize=8, parity="N", stopbits=1)
        self._ser.reset_input_buffer()
        self._stop.clear()
        self._rx_thread = threading.Thread(target=self._rx_loop, name="rx", daemon=True)
        self._rx_thread.start()
        return True

    def close(self):
        self._stop.set()
        if self._rx_thread is not None:
            self._rx_thread.join(timeout=0.5)
            self._rx_thread = None
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    @property
    def is_open(self):
        return self._ser is not None and self._ser.is_open

    # ------------------------------------------------------------------- 发送
    def write(self, frame):
        if not self.is_open:
            return False
        try:
            with self._wr_lock:
                self._ser.write(frame)
            return True
        except Exception as exc:
            self.error = "写串口失败：%r" % (exc,)
            self._log(self.error, "err")
            return False

    # ------------------------------------------------------------------- 接收
    def _rx_loop(self):
        while not self._stop.is_set():
            try:
                data = self._ser.read(4096)
            except Exception as exc:
                if not self._stop.is_set():
                    self.error = "读串口失败：%r" % (exc,)
                    self._log(self.error, "err")
                return
            if data:
                self._feed(data)

    def _feed(self, data):
        buf = self._buf
        buf += data
        st = self.stats
        while True:
            if len(buf) < 2:
                return
            head = buf[0]
            if head == P.CTRL_HEAD:
                # 帧头 AA，第二字节为固定 ID 码（0x55）
                if len(buf) < P.CTRL_UP_LEN:
                    return
                frame = bytes(buf[:P.CTRL_UP_LEN])
                rsp = P.unpack_ctrl_up(frame, self.addr)
                if rsp is not None:
                    st.last_ctrl = rsp
                    st.last_raw = frame
                    st.last_ctrl_t = time.time()
                    st.mark_rx(st.last_ctrl_t)
                    del buf[:P.CTRL_UP_LEN]
                    continue
                st.ctrl_err += 1
                st.slip += 1
                del buf[0]
                continue
            if head == P.PARAM_HEAD0 and buf[1] == P.PARAM_HEAD1:
                if len(buf) < P.PARAM_LEN:
                    return
                frame = bytes(buf[:P.PARAM_LEN])
                rsp = P.unpack_param(frame)
                if rsp is not None:
                    st.param_rx += 1
                    del buf[:P.PARAM_LEN]
                    self._dispatch(rsp)
                    continue
                st.param_err += 1
                st.slip += 1
                del buf[0]
                continue
            del buf[0]
            st.slip += 1

    def _dispatch(self, rsp):
        if rsp["addr"] != self.addr:
            return                      # 别的站号的应答，丢弃
        key = (rsp["cmd"], rsp["idx"])
        with self._pend_lock:
            entry = self._pending.get(key)
        if entry is not None:
            entry[1]["rsp"] = rsp
            entry[0].set()

    # --------------------------------------------------------------- 一问一答
    def request(self, cmd, idx, value=0, timeout=0.10, retries=4):
        """发一帧参数请求并等待配对应答；全部尝试超时返回 None。

        文档：CRC 错误或地址不匹配的请求被驱动器直接丢弃、不产生应答，
        由上位机超时重发 —— 所以单次超时不等于协议错误。
        """
        key = (cmd, idx)
        ev = threading.Event()
        box = {"rsp": None}
        with self._pend_lock:
            self._pending[key] = [ev, box]
        try:
            for attempt in range(1, retries + 1):
                ev.clear()
                box["rsp"] = None
                frame = P.pack_param(self.addr, cmd, idx, value)
                if not self.write(frame):
                    return None
                self.stats.tx_param += 1
                if ev.wait(timeout):
                    if attempt > 1:
                        self.stats.retries += attempt - 1
                    return box["rsp"]
            self.stats.retries += retries - 1
            self.stats.timeouts += 1
            return None
        finally:
            with self._pend_lock:
                self._pending.pop(key, None)

    def read_param(self, idx, **kw):
        return self.request(P.PARAM_CMD_READ, idx, 0, **kw)

    def write_param(self, idx, value, **kw):
        return self.request(P.PARAM_CMD_WRITE, idx, value, **kw)


class ControlSender(threading.Thread):
    """按固定周期发送下行控制帧，保证驱动器不触发 200 ms 通讯超时。

    bit1~bit3 是上升沿触发项：基帧里始终为 0，需要时用 pulse() 短暂置位，
    脉冲结束后自动回落到全 0，下一次 pulse 自然又是一个上升沿。
    """

    def __init__(self, link, hz=500.0, mode=P.MODE_VOLTAGE):
        super(ControlSender, self).__init__(name="ctrl-tx", daemon=True)
        self.link = link
        self.period = 1.0 / float(hz)
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._enable = False
        self._mode = mode
        self._ref1 = 0
        self._ref2 = 0
        self._pulse_bits = 0
        self._pulse_until = 0.0

    # ------------------------------------------------------------------- 设置
    def set_enable(self, on):
        with self._lock:
            self._enable = bool(on)

    def set_mode(self, mode):
        with self._lock:
            self._mode = mode

    def set_rate(self, hz):
        """动态调整控制帧速率（批量读参数时降速让出总线）。"""
        self.period = 1.0 / max(1.0, float(hz))

    def set_refs(self, ref1_pct, ref2_pct):
        with self._lock:
            self._ref1 = P.pct_to_ref(ref1_pct)
            self._ref2 = P.pct_to_ref(ref2_pct)

    def pulse(self, bits, ms=80):
        """置位 bit1~bit3 中的某一位，保持 ms 毫秒后自动回落。"""
        with self._lock:
            self._pulse_bits = bits & (P.CW_CLR_FAULT | P.CW_ZERO | P.CW_LATCH)
            self._pulse_until = time.perf_counter() + ms / 1000.0

    def state(self):
        with self._lock:
            return {
                "enable": self._enable,
                "mode": self._mode,
                "ref1": self._ref1,
                "ref2": self._ref2,
            }

    def stop(self):
        self._stop.set()

    # ------------------------------------------------------------------- 主循环
    def run(self):
        next_t = time.perf_counter()
        while not self._stop.is_set():
            now = time.perf_counter()
            if now < next_t:
                time.sleep(min(next_t - now, 0.0005))
                continue

            with self._lock:
                ctrl = P.CW_ENABLE if self._enable else 0
                mode = self._mode
                ref1, ref2 = self._ref1, self._ref2
                if self._pulse_bits and now < self._pulse_until:
                    ctrl |= self._pulse_bits
                    # 自动找零要求触发时 bit0 为 0，避免带电找零
                    if self._pulse_bits & P.CW_ZERO:
                        ctrl &= ~P.CW_ENABLE
                elif self._pulse_bits:
                    self._pulse_bits = 0

            frame = P.pack_ctrl_down(self.link.addr, ctrl, mode, ref1, ref2)
            if self.link.write(frame):
                self.link.stats.tx_ctrl += 1

            next_t += self.period
            if now - next_t > 0.05:      # 主机卡顿：重新对齐，不补发突发帧
                next_t = now
