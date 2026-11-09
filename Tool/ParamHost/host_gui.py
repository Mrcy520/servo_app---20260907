# -*- coding: utf-8 -*-
"""
驱动板上位机 · 图形界面

对应文档 Doc/驱动板参数协议.docx：
    帧格式 / 命令码 / 状态码 / 参数索引表（量程、倍率、只读属性）

设计要点（与固件行为一致）：
  · 写参数只进驱动器暂存区；必须由控制帧控制字 bit3 的上升沿锁存才生效。
    因此本程序常驻发送下行控制帧（默认 500 Hz），并在"锁存"按钮里发一次 bit3 脉冲。
  · 读取返回的是"生效值"：写完未锁存前读到的仍是旧值，可据此确认锁存是否成功。
  · 驱动器连续 200 ms 未收到合法下行帧即关输出，所以断开连接前不要停在半途。

运行：python host_gui.py     （协议层自检：python protocol.py）
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time

# tkinter 用 try 包住：这样 `--check` 能在没有 tkinter 的解释器上跑起来，
# 先告出真正原因（缺 tkinter 还是缺 pyserial），而不是直接抛 ImportError。
try:
    import tkinter as tk
    from tkinter import ttk, messagebox, filedialog
    TK_OK = True
    TK_ERR = ""
except Exception as _tk_exc:
    tk = None
    ttk = None
    messagebox = None
    filedialog = None
    TK_OK = False
    TK_ERR = "%s: %s" % (type(_tk_exc).__name__, _tk_exc)

import protocol as P

try:
    import link as L
except Exception as exc:                                  # pyserial 缺失时给出可读提示
    L = None
    _LINK_ERR = exc
else:
    _LINK_ERR = None

FONT = ("Microsoft YaHei UI", 9)
FONT_MONO = ("Consolas", 9)

BAUDS = ["9600", "57600", "115200", "230400", "460800", "921600", "1500000"]
RATES = ["100", "200", "500", "1000"]


def _dpi_aware():
    if sys.platform != "win32":
        return
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass


class HostApp(object):

    def __init__(self, root):
        self.root = root
        self.link = None
        self.sender = None
        self.values = {}          # idx -> 生效值（最近一次读取）
        self.pending = {}         # idx -> 已写入但未锁存的值
        self.cur_idx = None
        self._q = queue.Queue()
        self._syncing = False
        self._busy = False

        root.title("驱动板参数上位机 · 参数协议 v1.0（CRC-16/CCITT-FALSE）")
        root.geometry("1300x820")
        root.minsize(1160, 720)

        self._build_conn()
        self._build_body()
        self._build_log(self._outer)

        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(120, self._init_sash)
        root.after(50, self._tick)
        self._log("就绪。选择串口后点击「连接」；连接后自动读取全部参数。", "info")
        self._log("解释器：%s" % sys.executable, "info")
        if L is None:
            self._log("串口模块加载失败：%s" % _LINK_ERR, "err")
        elif L.PYSERIAL_OK:
            self._log("pyserial %s 就绪" % L.PYSERIAL_VER, "ok")
        else:
            self._log("pyserial 未安装：%s" % L.PYSERIAL_ERR, "err")
            self._log("请执行：\"%s\" -m pip install pyserial" % sys.executable, "err")
        root.after(150, self._refresh_ports)

    # ================================================================= 构建界面
    def _build_conn(self):
        bar = ttk.Frame(self.root, padding=(10, 8, 10, 4))
        bar.pack(fill="x")

        ttk.Label(bar, text="串口", font=FONT).pack(side="left")
        self.cb_port = ttk.Combobox(bar, width=10, font=FONT)   # 可编辑：枚举失败时可手工输入
        self.cb_port.pack(side="left", padx=(4, 2))
        self.btn_refresh = ttk.Button(bar, text="刷新", width=6, command=self._refresh_ports)
        self.btn_refresh.pack(side="left", padx=(0, 10))

        ttk.Label(bar, text="波特率", font=FONT).pack(side="left")
        self.cb_baud = ttk.Combobox(bar, width=9, font=FONT, state="readonly", values=BAUDS)
        self.cb_baud.set("921600")
        self.cb_baud.pack(side="left", padx=(4, 10))

        ttk.Label(bar, text="站号", font=FONT).pack(side="left")
        self.sp_addr = ttk.Spinbox(bar, from_=1, to=247, width=5, font=FONT)
        self.sp_addr.set("1")
        self.sp_addr.pack(side="left", padx=(4, 10))

        ttk.Label(bar, text="控制帧", font=FONT).pack(side="left")
        self.cb_hz = ttk.Combobox(bar, width=6, font=FONT, state="readonly", values=RATES)
        self.cb_hz.set("500")
        self.cb_hz.pack(side="left", padx=(4, 2))
        ttk.Label(bar, text="Hz", font=FONT).pack(side="left", padx=(0, 10))

        self.btn_conn = ttk.Button(bar, text="连接", width=10, command=self._toggle_conn)
        self.btn_conn.pack(side="left")

        self.lb_conn = tk.Label(bar, text="● 未连接", font=FONT, fg="#B00020")
        self.lb_conn.pack(side="left", padx=12)

        self.lb_stat = tk.Label(bar, text="", font=FONT_MONO, fg="#555555")
        self.lb_stat.pack(side="right")

    def _build_body(self):
        outer = ttk.PanedWindow(self.root, orient="vertical")
        outer.pack(fill="both", expand=True, padx=10, pady=(0, 6))
        self._outer = outer

        upper = ttk.PanedWindow(outer, orient="horizontal")
        outer.add(upper, weight=4)
        self._upper = upper

        # ---------------- 左：参数表 ----------------
        box = ttk.LabelFrame(upper, text=" 参数表（表 3-1） ", padding=6)
        upper.add(box, weight=3)

        cols = ("idx", "name", "cur", "phys", "pend", "range", "attr")
        tree_wrap = ttk.Frame(box)
        tree_wrap.pack(side="top", fill="both", expand=True)

        self.tree = ttk.Treeview(tree_wrap, columns=cols, show="headings", height=14,
                                 selectmode="browse")
        heads = {
            "idx": ("索引", 56), "name": ("名称", 120), "cur": ("生效值", 84),
            "phys": ("物理值", 100), "pend": ("待锁存", 76),
            "range": ("范围", 104), "attr": ("属性", 46),
        }
        for c, (text, w) in heads.items():
            self.tree.heading(c, text=text)
            self.tree.column(c, width=w, minwidth=w, anchor="center" if c != "name" else "w",
                             stretch=(c == "name"))
        vs = ttk.Scrollbar(tree_wrap, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vs.set)
        self.tree.pack(side="left", fill="both", expand=True)
        vs.pack(side="left", fill="y")

        self.tree.tag_configure("ro", foreground="#888888")
        self.tree.tag_configure("pend", background="#FFF6DA")
        self.tree.tag_configure("bad", foreground="#B00020")
        self.tree.bind("<<TreeviewSelect>>", self._on_select)

        for p in P.PARAMS:
            self.tree.insert("", "end", iid=str(p.idx), values=self._row_values(p.idx),
                             tags=("ro",) if p.ro else ())

        ops = ttk.Frame(box)
        ops.pack(side="bottom", fill="x", pady=(6, 0))
        self.btn_readall = ttk.Button(ops, text="读取全部", command=self._read_all)
        self.btn_readall.pack(side="left")
        self.btn_latch = ttk.Button(ops, text="锁存参数 (bit3)", command=self._latch)
        self.btn_latch.pack(side="left", padx=6)
        ttk.Button(ops, text="导出 JSON", command=self._export).pack(side="right")
        ttk.Button(ops, text="导入 JSON", command=self._import).pack(side="right", padx=6)

        # ---------------- 右：状态 / 控制 / 编辑 ----------------
        right = ttk.Frame(upper)
        upper.add(right, weight=2)

        # 实时状态
        fb = ttk.LabelFrame(right, text=" 上行状态（10 B 控制帧，1 kHz） ", padding=6)
        fb.pack(fill="x")
        bits = tk.Frame(fb)
        bits.pack(fill="x")
        self.bit_labels = {}
        for bit, short, _desc in P.STATUS_BITS:
            lb = tk.Label(bits, text=short, font=("Consolas", 8, "bold"), width=6,
                          bg="#E6E6E6", fg="#777777", relief="flat", padx=1, pady=2)
            lb.pack(side="left", padx=1)
            self.bit_labels[bit] = lb

        grid = tk.Frame(fb)
        grid.pack(fill="x", pady=(6, 0))
        self.lb_vals = {}
        for i, (key, text) in enumerate([("angle", "电角度"), ("iq", "Iq"), ("vbus", "母线电压")]):
            tk.Label(grid, text=text, font=FONT, fg="#555555").grid(row=0, column=i * 2, sticky="w")
            lb = tk.Label(grid, text="—", font=FONT_MONO, fg="#111111")
            lb.grid(row=0, column=i * 2 + 1, sticky="w", padx=(4, 14))
            self.lb_vals[key] = lb
        tk.Label(grid, text="帧率", font=FONT, fg="#555555").grid(row=1, column=0, sticky="w", pady=(2, 0))
        self.lb_vals["fps"] = tk.Label(grid, text="—", font=FONT_MONO, fg="#111111")
        self.lb_vals["fps"].grid(row=1, column=1, sticky="w", padx=(4, 14), pady=(2, 0))
        tk.Label(grid, text="链龄", font=FONT, fg="#555555").grid(row=1, column=2, sticky="w", pady=(2, 0))
        self.lb_vals["age"] = tk.Label(grid, text="—", font=FONT_MONO, fg="#111111")
        self.lb_vals["age"].grid(row=1, column=3, sticky="w", padx=(4, 14), pady=(2, 0))

        # 输出控制
        cb = ttk.LabelFrame(right, text=" 输出控制（下行 10 B） ", padding=6)
        cb.pack(fill="x", pady=(8, 0))

        row0 = ttk.Frame(cb)
        row0.pack(fill="x")
        self.var_en = tk.BooleanVar(value=False)
        self.ck_en = ttk.Checkbutton(row0, text="使能输出 (bit0)", variable=self.var_en,
                                     command=self._on_enable)
        self.ck_en.pack(side="left")

        self.var_mode = tk.IntVar(value=P.MODE_VOLTAGE)
        ttk.Radiobutton(row0, text="电压开环", value=P.MODE_VOLTAGE, variable=self.var_mode,
                        command=self._on_mode).pack(side="left", padx=(12, 4))
        ttk.Radiobutton(row0, text="电流闭环", value=P.MODE_CURRENT, variable=self.var_mode,
                        command=self._on_mode).pack(side="left")

        row1 = ttk.Frame(cb)
        row1.pack(fill="x", pady=(6, 0))
        ttk.Label(row1, text="设定值1", font=FONT).pack(side="left")
        self.var_ref1 = tk.StringVar(value="0.00")
        e1 = ttk.Entry(row1, textvariable=self.var_ref1, width=8, font=FONT_MONO, justify="right")
        e1.pack(side="left", padx=(4, 2))
        ttk.Label(row1, text="%", font=FONT).pack(side="left", padx=(0, 10))

        ttk.Label(row1, text="设定值2", font=FONT).pack(side="left")
        self.var_ref2 = tk.StringVar(value="0.00")
        e2 = ttk.Entry(row1, textvariable=self.var_ref2, width=8, font=FONT_MONO, justify="right")
        e2.pack(side="left", padx=(4, 2))
        ttk.Label(row1, text="%", font=FONT).pack(side="left")
        ttk.Button(row1, text="应用设定", width=9, command=self._apply_refs).pack(side="left", padx=8)

        row2 = ttk.Frame(cb)
        row2.pack(fill="x", pady=(6, 0))
        ttk.Button(row2, text="清故障 (bit1)", command=self._clear_fault).pack(side="left")
        ttk.Button(row2, text="自动找零 (bit2)", command=self._zero).pack(side="left", padx=6)

        self.lb_hint = tk.Label(cb, text="断开连接后 200 ms 驱动器自动关输出", font=FONT, fg="#777777")
        self.lb_hint.pack(anchor="w", pady=(6, 0))

        # 参数编辑
        eb = ttk.LabelFrame(right, text=" 参数编辑（写入后需锁存） ", padding=6)
        eb.pack(fill="both", expand=True, pady=(8, 0))

        self.lb_cur = tk.Label(eb, text="未选择参数", font=FONT, fg="#111111", anchor="w")
        self.lb_cur.pack(fill="x")

        r1 = ttk.Frame(eb)
        r1.pack(fill="x", pady=(6, 0))
        ttk.Label(r1, text="原始值", font=FONT).pack(side="left")
        self.var_raw = tk.StringVar()
        self.en_raw = ttk.Entry(r1, textvariable=self.var_raw, width=12, font=FONT_MONO, justify="right")
        self.en_raw.pack(side="left", padx=4)
        ttk.Label(r1, text="物理值", font=FONT).pack(side="left", padx=(10, 0))
        self.var_phys = tk.StringVar()
        self.en_phys = ttk.Entry(r1, textvariable=self.var_phys, width=12, font=FONT_MONO, justify="right")
        self.en_phys.pack(side="left", padx=4)

        self.var_raw.trace_add("write", self._raw_changed)
        self.var_phys.trace_add("write", self._phys_changed)

        self.lb_range = tk.Label(eb, text="", font=FONT, fg="#777777", anchor="w", justify="left")
        self.lb_range.pack(fill="x", pady=(4, 0))

        r2 = ttk.Frame(eb)
        r2.pack(fill="x", pady=(6, 0))
        self.btn_read1 = ttk.Button(r2, text="读取该项", command=self._read_one_click)
        self.btn_read1.pack(side="left")
        self.btn_write1 = ttk.Button(r2, text="写入（暂存）", command=self._write_one_click)
        self.btn_write1.pack(side="left", padx=6)

    def _build_log(self, outer):
        box = ttk.LabelFrame(outer, text=" 报文日志 ", padding=(6, 4))
        outer.add(box, weight=1)

        bar = ttk.Frame(box)
        bar.pack(fill="x")
        self.var_auto = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="自动滚动", variable=self.var_auto).pack(side="left")
        ttk.Button(bar, text="清空", width=6, command=self._clear_log).pack(side="left", padx=6)
        ttk.Label(bar, text="  原始帧(HEX)", font=FONT).pack(side="left")
        self.var_raw_frame = tk.StringVar()
        e = ttk.Entry(bar, textvariable=self.var_raw_frame, width=44, font=FONT_MONO)
        e.pack(side="left", padx=4)
        ttk.Button(bar, text="发送", width=6, command=self._send_raw).pack(side="left")

        wrap = ttk.Frame(box)
        wrap.pack(fill="both", expand=True, pady=(4, 0))
        self.txt = tk.Text(wrap, height=6, font=FONT_MONO, wrap="none", bg="#FCFCFC",
                           relief="solid", borderwidth=1)
        sb = ttk.Scrollbar(wrap, orient="vertical", command=self.txt.yview)
        self.txt.configure(yscrollcommand=sb.set)
        self.txt.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        self.txt.tag_configure("info", foreground="#555555")
        self.txt.tag_configure("ok", foreground="#1B5E20")
        self.txt.tag_configure("err", foreground="#B00020")
        self.txt.tag_configure("tx", foreground="#0D47A1")
        self.txt.tag_configure("rx", foreground="#33691E")
        self.txt.configure(state="disabled")

    # ==================================================================== 表格
    def _init_sash(self):
        """给参数表留出足够宽度，避免列被压缩后文字互相侵占。"""
        try:
            w = self._upper.winfo_width()
            if w > 400:
                self._upper.sashpos(0, max(580, int(w * 0.60)))
        except Exception:
            pass

    def _row_values(self, idx):
        p = P.PARAM_BY_IDX[idx]
        cur = self.values.get(idx)
        pend = self.pending.get(idx)
        return (
            "0x%02X" % idx,
            p.name,
            "—" if cur is None else str(cur),
            p.phys_text(cur),
            "" if pend is None else str(pend),
            "%d ~ %d" % (p.rmin, p.rmax) if not p.ro else "—",
            "只读" if p.ro else "读写",
        )

    def _refresh_row(self, idx):
        iid = str(idx)
        if not self.tree.exists(iid):
            return
        tags = ["ro"] if P.PARAM_BY_IDX[idx].ro else []
        if idx in self.pending:
            tags.append("pend")
        self.tree.item(iid, values=self._row_values(idx), tags=tuple(tags))

    def _on_select(self, _evt=None):
        sel = self.tree.selection()
        if not sel:
            return
        idx = int(sel[0])
        self.cur_idx = idx
        p = P.PARAM_BY_IDX[idx]
        self.lb_cur.configure(text="0x%02X  %s" % (idx, p.name))
        if p.ro:
            self.lb_range.configure(text="只读参数：%s" % p.hint)
        else:
            self.lb_range.configure(text="范围 %d ~ %d ｜ %s" % (p.rmin, p.rmax, p.hint))
        cur = self.values.get(idx)
        if cur is not None:
            self._set_editor(cur)
        self.en_raw.configure(state="disabled" if p.ro else "normal")
        self.en_phys.configure(state="disabled" if p.ro else "normal")
        self.btn_write1.configure(state="disabled" if p.ro else "normal")
        self.btn_read1.configure(state="normal" if self._connected() else "disabled")

    def _set_editor(self, raw):
        self._syncing = True
        try:
            self.var_raw.set(str(raw))
            self.var_phys.set(P.PARAM_BY_IDX[self.cur_idx].phys_text(raw))
        finally:
            self._syncing = False

    def _raw_changed(self, *_a):
        if self._syncing or self.cur_idx is None:
            return
        p = P.PARAM_BY_IDX[self.cur_idx]
        try:
            raw = int(self.var_raw.get().strip(), 0)
        except Exception:
            return
        self._syncing = True
        try:
            self.var_phys.set(p.phys_text(raw))
        finally:
            self._syncing = False

    def _phys_changed(self, *_a):
        if self._syncing or self.cur_idx is None:
            return
        p = P.PARAM_BY_IDX[self.cur_idx]
        try:
            phys = float(self.var_phys.get().strip().split()[0])
        except Exception:
            return
        raw = p.from_phys(phys)
        self._syncing = True
        try:
            self.var_raw.set(str(raw))
        finally:
            self._syncing = False

    # ==================================================================== 日志
    def _post(self, fn, *args):
        self._q.put((fn, args))

    def _log(self, msg, tag="info"):
        ts = time.strftime("%H:%M:%S")
        self.txt.configure(state="normal")
        self.txt.insert("end", "[%s] %s\n" % (ts, msg), tag)
        if self.var_auto.get():
            self.txt.see("end")
        self.txt.configure(state="disabled")
        line = int(self.txt.index("end-1c").split(".")[0])
        if line > 800:
            self.txt.delete("1.0", "200.0")

    def _log_t(self, msg, tag="info"):
        self._post(self._log, msg, tag)

    def _clear_log(self):
        self.txt.configure(state="normal")
        self.txt.delete("1.0", "end")
        self.txt.configure(state="disabled")

    def _hex(self, b):
        return " ".join("%02X" % x for x in b)

    def _send_raw(self):
        if not self._connected():
            messagebox.showwarning("未连接", "请先连接串口。")
            return
        s = self.var_raw_frame.get().replace(" ", "").replace(",", "")
        try:
            data = bytes.fromhex(s)
        except Exception:
            messagebox.showerror("格式错误", "请输入十六进制字节，例如 5AA501010900...")
            return
        if not data:
            return
        self.link.write(data)
        self._log("TX(原始) %s" % self._hex(data), "tx")

    # ==================================================================== 连接
    def _refresh_ports(self):
        if L is None:
            self._log("串口模块加载失败：%s" % _LINK_ERR, "err")
            return
        ports = L.list_ports()
        self.cb_port["values"] = [p[0] for p in ports]
        if ports:
            cur = self.cb_port.get().strip().upper()
            if cur not in [p[0] for p in ports]:
                # 默认优先选 USB 转串口设备，避免误选主板自带的 COM1
                pick = ports[0][0]
                for dev, desc in ports:
                    if any(k in (desc or "").upper()
                           for k in ("USB", "WCH", "CH34", "CP210", "FTDI", "SILICON")):
                        pick = dev
                        break
                self.cb_port.set(pick)
            self._log("发现 %d 个串口：%s" % (len(ports),
                                             ", ".join("%s(%s)" % (d, s) if s else d for d, s in ports)),
                      "ok")
            return

        # 一个都没找到：区分「装不上 pyserial」和「真的没有设备」两种情况
        self.cb_port["values"] = ["COM%d" % i for i in range(1, 33)]
        if not L.PYSERIAL_OK:
            self._log("未安装 pyserial：%s" % L.PYSERIAL_ERR, "err")
            self._log("当前解释器：%s" % sys.executable, "err")
            self._log("请执行：\"%s\" -m pip install pyserial" % sys.executable, "err")
        else:
            self._log("系统未枚举到任何串口（pyserial %s 已就绪）。"
                      "请检查 USB 转串口设备与驱动，或直接在下拉框里输入端口名（如 COM38）。"
                      % L.PYSERIAL_VER, "err")

    def _connected(self):
        return self.link is not None and self.link.is_open

    def _toggle_conn(self):
        if self._connected():
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        if L is None:
            messagebox.showerror("缺少依赖", "串口模块加载失败：\n%s" % _LINK_ERR)
            return
        if not L.PYSERIAL_OK:
            messagebox.showerror(
                "缺少 pyserial",
                "当前解释器没有安装 pyserial：\n%s\n\n%s\n\n请执行：\n\"%s\" -m pip install pyserial"
                % (sys.executable, L.PYSERIAL_ERR, sys.executable))
            return
        port = self.cb_port.get().strip().upper()
        if port.isdigit():
            port = "COM" + port
        if not port:
            messagebox.showwarning("未选择串口", "请选择或直接输入串口名（如 COM38）。")
            return
        try:
            baud = int(self.cb_baud.get())
            addr = int(self.sp_addr.get())
            hz = float(self.cb_hz.get())
        except Exception:
            messagebox.showerror("参数错误", "波特率 / 站号 / 控制帧频率填写有误。")
            return
        if not (1 <= addr <= 247):
            messagebox.showerror("站号越界", "站号必须为 1 ~ 247（表 2-1）。")
            return

        self.link = L.SerialLink(port, baud, addr, log=self._log_t)
        try:
            self.link.open()
        except Exception as exc:
            self.link = None
            messagebox.showerror("打开串口失败", "%s\n\n%s" % (port, exc))
            return

        self.sender = L.ControlSender(self.link, hz=hz, mode=self.var_mode.get())
        self.sender.set_enable(False)
        self.sender.start()

        self.btn_conn.configure(text="断开")
        self.lb_conn.configure(text="● 已连接 %s @ %d" % (port, baud), fg="#1B5E20")
        self.sp_addr.configure(state="disabled")
        self.cb_baud.configure(state="readonly")
        self.btn_refresh.configure(state="disabled")
        self._log("已连接 %s @ %d 8N1，站号 %d，控制帧 %d Hz" % (port, baud, addr, int(hz)), "ok")
        self._log("提示：驱动板若为调试档（COMM_DEBUG_REPORT=1），串口只输出文本、参数链路静默。", "info")
        self.root.after(200, self._read_all)

    def _disconnect(self):
        self.var_en.set(False)
        if self.sender is not None:
            self.sender.set_enable(False)
        time.sleep(0.05)
        if self.sender is not None:
            self.sender.stop()
            self.sender = None
        if self.link is not None:
            self.link.close()
            self.link = None
        self.btn_conn.configure(text="连接")
        self.lb_conn.configure(text="● 未连接", fg="#B00020")
        self.sp_addr.configure(state="normal")
        self.btn_refresh.configure(state="normal")
        self.lb_stat.configure(text="")
        self._log("已断开。驱动器应在 200 ms 后关闭输出。", "info")

    # ================================================================== 参数读写
    def _read_all(self):
        if not self._connected() or self._busy:
            return
        self._busy = True
        self._log("开始读取全部参数（0x01 ~ 0x0D）…", "info")

        def work():
            bad = 0
            for p in P.PARAMS:
                if not self._connected():
                    break
                rsp = self.link.read_param(p.idx)
                if rsp is None:
                    bad += 1
                    self._log_t("读 0x%02X %s：无应答（超时）" % (p.idx, p.name), "err")
                    continue
                if rsp["status"] != P.PARAM_ST_OK:
                    bad += 1
                    self._log_t("读 0x%02X %s：%s（0x%02X）"
                                % (p.idx, p.name, P.STATUS_NAME.get(rsp["status"], "?"), rsp["status"]), "err")
                    continue
                self._post(self._apply_value, p.idx, rsp["value"])
                self._log_t("读 0x%02X %-8s = %-10d (%s)"
                            % (p.idx, p.name, rsp["value"], p.phys_text(rsp["value"])), "rx")
            self._post(self._read_all_done, bad)

        threading.Thread(target=work, daemon=True).start()

    def _read_all_done(self, bad):
        self._busy = False
        st = self.link.stats if self.link else None
        extra = ""
        if st is not None:
            extra = "｜重发 %d 次 / 超时 %d 次" % (st.retries, st.timeouts)
        self._log("读取完成，失败 %d 项%s" % (bad, extra), "ok" if bad == 0 else "err")

    def _apply_value(self, idx, raw):
        self.values[idx] = raw
        self._refresh_row(idx)
        if idx == self.cur_idx and idx not in self.pending:
            self._set_editor(raw)

    def _read_one_click(self):
        if not self._connected() or self.cur_idx is None:
            return
        idx = self.cur_idx
        p = P.PARAM_BY_IDX[idx]

        def work():
            rsp = self.link.read_param(idx)
            if rsp is None:
                self._log_t("读 0x%02X %s：无应答" % (idx, p.name), "err")
                return
            if rsp["status"] != P.PARAM_ST_OK:
                self._log_t("读 0x%02X：%s" % (idx, P.STATUS_NAME.get(rsp["status"], "?")), "err")
                return
            self._post(self._apply_value, idx, rsp["value"])
            self._log_t("读 0x%02X %s = %d" % (idx, p.name, rsp["value"]), "rx")

        threading.Thread(target=work, daemon=True).start()

    def _write_one_click(self):
        if not self._connected():
            messagebox.showwarning("未连接", "请先连接串口。")
            return
        if self.cur_idx is None:
            return
        idx = self.cur_idx
        p = P.PARAM_BY_IDX[idx]
        try:
            raw = int(self.var_raw.get().strip(), 0)
        except Exception:
            messagebox.showerror("输入无效", "原始值必须是整数。")
            return
        if not p.in_range(raw):
            messagebox.showerror("超出量程", "%s 的范围是 %d ~ %d，当前 %d。"
                                 % (p.name, p.rmin, p.rmax, raw))
            return
        self._write_param(idx, raw)

    def _write_param(self, idx, raw):
        p = P.PARAM_BY_IDX[idx]

        def work():
            rsp = self.link.write_param(idx, raw)
            if rsp is None:
                self._log_t("写 0x%02X %s=%d：无应答" % (idx, p.name, raw), "err")
                return
            if rsp["status"] != P.PARAM_ST_OK:
                self._log_t("写 0x%02X %s=%d：拒收 %s"
                            % (idx, p.name, raw, P.STATUS_NAME.get(rsp["status"], "?")), "err")
                return
            self._post(self._mark_pending, idx, raw)
            self._log_t("写 0x%02X %-8s = %-10d 已进暂存区，等待锁存"
                        % (idx, p.name, raw), "tx")

        threading.Thread(target=work, daemon=True).start()

    def _mark_pending(self, idx, raw):
        self.pending[idx] = raw
        self._refresh_row(idx)
        self._log("待锁存项：%d 个（点「锁存参数」下发控制字 bit3 生效）" % len(self.pending), "info")

    def _latch(self):
        if not self._connected() or self.sender is None:
            messagebox.showwarning("未连接", "请先连接串口。")
            return
        if not self.pending:
            self._log("没有待锁存参数，仍发送一次 bit3 脉冲（幂等）。", "info")
        self.sender.pulse(P.CW_LATCH, 80)
        self._log("TX 控制字 bit3 上升沿 → 参数锁存生效", "tx")
        # 锁存后驱动器可能在输出关闭时写 Flash，主循环停摆 20~40 ms，稍等再回读
        self.root.after(350, self._after_latch)

    def _after_latch(self):
        self.pending.clear()
        for p in P.PARAMS:
            self._refresh_row(p.idx)
        self._log("锁存完成，回读确认生效值…", "info")
        self._read_all()

    # ================================================================== 输出控制
    def _on_enable(self):
        if self.sender is None:
            self.var_en.set(False)
            return
        on = bool(self.var_en.get())
        if on:
            st = self.link.stats.last_ctrl if self.link else None
            if st is not None and (st["status"] & P.ST_FAULT_LATCH):
                self.var_en.set(False)
                self.sender.set_enable(False)
                messagebox.showwarning("故障锁存中", "状态位 bit7（故障锁存）为 1，驱动器不接受使能。\n"
                                                     "请先点击「清故障」下发控制字 bit1。")
                return
        self.sender.set_enable(on)
        self._log("控制字 bit0 = %d（%s）" % (1 if on else 0, "使能输出" if on else "关闭输出"), "tx")

    def _on_mode(self):
        if self.sender is not None:
            self.sender.set_mode(self.var_mode.get())
            self._log("控制模式 = %d（%s）" % (self.var_mode.get(),
                                              "电压开环" if self.var_mode.get() == P.MODE_VOLTAGE else "电流闭环"), "tx")

    def _apply_refs(self):
        if self.sender is None:
            return
        try:
            r1 = float(self.var_ref1.get())
            r2 = float(self.var_ref2.get())
        except Exception:
            messagebox.showerror("输入无效", "设定值应为百分比数值，例如 10 或 -25.5")
            return
        for v in (r1, r2):
            if not (-100.0 <= v <= 100.0):
                messagebox.showerror("超出量程", "设定值范围 -100.00% ~ 100.00%（-32767 ~ 32767）。")
                return
        self.sender.set_refs(r1, r2)
        self._log("设定值1 = %+.2f%% (%d)、设定值2 = %+.2f%% (%d)"
                  % (r1, P.pct_to_ref(r1), r2, P.pct_to_ref(r2)), "tx")

    def _clear_fault(self):
        if not self._connected():
            messagebox.showwarning("未连接", "请先连接串口。")
            return
        self.sender.pulse(P.CW_CLR_FAULT, 80)
        self._log("TX 控制字 bit1 上升沿 → 清除故障锁存", "tx")

    def _zero(self):
        if not self._connected():
            messagebox.showwarning("未连接", "请先连接串口。")
            return
        try:
            uq = float(self.var_ref1.get())
        except Exception:
            uq = 0.0
        if not messagebox.askyesno(
                "自动找零",
                "将以设定值1（%+.2f%% → %d）作为找零电压，关闭使能后触发 bit2，约 5 秒后自动结束。\n\n继续？"
                % (uq, P.pct_to_ref(uq))):
            return
        self.var_en.set(False)
        self.sender.set_enable(False)
        self.sender.set_refs(uq, 0.0)
        time.sleep(0.05)
        self.sender.pulse(P.CW_ZERO, 120)
        self._log("TX 控制字 bit2 上升沿 → 自动找零，找零电压 %d，约 5 s 后自动结束"
                  % P.pct_to_ref(uq), "tx")

    # ================================================================ 导入导出
    def _export(self):
        path = filedialog.asksaveasfilename(defaultextension=".json",
                                            filetypes=[("JSON", "*.json")],
                                            initialfile="motor_params.json")
        if not path:
            return
        data = {
            "tool": "Motor_Prj ParamHost",
            "saved_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "device_addr": int(self.sp_addr.get() or 1),
            "values": dict((("0x%02X" % k), v) for k, v in sorted(self.values.items())),
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        self._log("已导出 %d 项参数到 %s" % (len(self.values), os.path.basename(path)), "ok")

    def _import(self):
        path = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not path:
            return
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        vals = data.get("values", {})
        if not vals:
            messagebox.showerror("文件无效", "未找到 values 字段。")
            return
        if not self._connected():
            messagebox.showwarning("未连接", "导入后需要写入驱动器，请先连接串口。")
            return
        if not messagebox.askyesno("确认导入", "将把 %d 项参数依次写入暂存区，随后锁存。\n\n继续？" % len(vals)):
            return

        self._busy = True

        def work():
            n = 0
            for k, v in sorted(vals.items()):
                idx = int(k, 0)
                p = P.PARAM_BY_IDX.get(idx)
                if p is None or p.ro:
                    continue
                if not p.in_range(v):
                    self._log_t("跳过 0x%02X %s=%d：超出量程" % (idx, p.name, v), "err")
                    continue
                if not self._connected():
                    break
                rsp = self.link.write_param(idx, int(v))
                if rsp is None or rsp["status"] != P.PARAM_ST_OK:
                    self._log_t("写 0x%02X 失败" % idx, "err")
                    continue
                n += 1
                self._post(self._mark_pending, idx, int(v))
            self._post(self._import_done, n)

        threading.Thread(target=work, daemon=True).start()

    def _import_done(self, n):
        self._busy = False
        self._log("导入完成：%d 项已进暂存区，请点「锁存参数」" % n, "ok" if n else "err")

    # =================================================================== 周期刷新
    def _tick(self):
        try:
            while True:
                fn, args = self._q.get_nowait()
                try:
                    fn(*args)
                except Exception as exc:
                    self._log("界面回调异常：%r" % (exc,), "err")
        except queue.Empty:
            pass

        if self._connected() and self.link is not None:
            st = self.link.stats
            f = st.last_ctrl
            if f is not None:
                for bit, _short, _desc in P.STATUS_BITS:
                    on = bool(f["status"] & bit)
                    lb = self.bit_labels[bit]
                    if not on:
                        lb.configure(bg="#E6E6E6", fg="#8A8A8A")
                    elif bit == P.ST_ENABLED:
                        lb.configure(bg="#2E7D32", fg="#FFFFFF")
                    elif bit in (P.ST_COMM_LOST, P.ST_FAULT_LATCH):
                        lb.configure(bg="#B00020", fg="#FFFFFF")
                    else:
                        lb.configure(bg="#D98800", fg="#FFFFFF")
                ang_map = f.get("angle_deg_map", f.get("angle_deg_cal", 0.0))
                ang_full = f.get("angle_deg_full", 0.0)
                self.lb_vals["angle"].configure(
                    text="%.2f° [原始: %.2f°]" % (ang_map, ang_full))
                self.lb_vals["iq"].configure(text="%.2f A" % (f["iq"] * P.CTRL_IQ_LSB))
                self.lb_vals["vbus"].configure(text="%.1f V" % (f["vbus"] * P.CTRL_VBUS_LSB))
                self.lb_vals["age"].configure(
                    text="%.0f ms%s" % ((time.time() - st.last_ctrl_t) * 1000.0,
                                        "" if st.alive() else " 超时"))
            self.lb_vals["fps"].configure(text="%.0f" % st.fps())
            self.lb_stat.configure(
                text="上行 %d 帧 ｜ 参数 TX %d / RX %d ｜ 重发 %d ｜ 错帧 %d"
                     % (st.ctrl_rx, st.tx_param, st.param_rx, st.retries,
                        st.ctrl_err + st.param_err))
        self.root.after(100, self._tick)

    # ==================================================================== 退出
    def _on_close(self):
        try:
            if self.sender is not None:
                self.sender.set_enable(False)
                time.sleep(0.05)
                self.sender.stop()
            if self.link is not None:
                self.link.close()
        except Exception:
            pass
        self.root.destroy()


def _self_heal():
    """当前解释器缺 tkinter 或 pyserial 时，自动切到具备依赖的解释器重新启动。

    典型场景：双击 .py 由文件关联的解释器拉起，那个解释器不一定装了 pyserial。
    用环境变量做一次性保护，避免来回重启。
    """
    if os.environ.get("MPH_RELAUNCH"):
        return False
    if sys.platform != "win32":
        return False

    need = "tkinter" if not TK_OK else "serial"
    probe = "import sys, tkinter, serial; print(sys.executable)" if need == "serial" \
        else "import sys, tkinter; print(sys.executable)"
    seen = set()

    for cand in (["py", "-3.12"], ["py", "-3"], ["python"]):
        try:
            out = subprocess.run(cand + ["-c", probe], capture_output=True, text=True, timeout=8)
        except Exception:
            continue
        if out.returncode != 0:
            continue
        lines = (out.stdout or "").strip().splitlines()
        if not lines:
            continue
        exe = lines[-1].strip()
        if os.path.normcase(exe) == os.path.normcase(sys.executable):
            continue
        if os.path.normcase(exe) in seen:
            continue
        seen.add(os.path.normcase(exe))
        env = dict(os.environ)
        env["MPH_RELAUNCH"] = "1"
        try:
            os.execve(exe, [exe, os.path.abspath(__file__)] + sys.argv[1:], env)
        except Exception:
            continue
    return False


def main():
    if "--check" in sys.argv or "-c" in sys.argv:
        # 环境自检：解释器 / tkinter / pyserial / 串口枚举，不打开界面
        print("解释器 : %s" % sys.executable)
        print("版本   : %s" % sys.version.split()[0])
        print("tkinter: %s" % ("OK" if TK_OK else "缺失 -> %s" % TK_ERR))
        if L is None:
            print("pyserial: 模块加载失败 %s" % _LINK_ERR)
            return 2
        print("pyserial: %s" % (L.PYSERIAL_VER if L.PYSERIAL_OK else "未安装 -> %s" % L.PYSERIAL_ERR))
        ports = L.list_ports()
        print("串口   : %d 个" % len(ports))
        for dev, desc in ports:
            print("   %-6s %s" % (dev, desc))
        if not ports:
            print("   （若确认有设备，请检查驱动；也可在界面里直接输入端口名）")
        return 0 if (ports and TK_OK and L.PYSERIAL_OK) else 1

    # 依赖不齐时先尝试换解释器自愈，失败再按原解释器给出可读提示
    if (not TK_OK) or (L is None) or (not L.PYSERIAL_OK):
        if "--no-relaunch" not in sys.argv:
            _self_heal()

    if not TK_OK:
        sys.stderr.write(
            "当前解释器没有 tkinter：\n  %s\n  %s\n"
            "请改用官方安装版 Python，或运行 run.bat。\n"
            "诊断：python host_gui.py --check\n" % (sys.executable, TK_ERR))
        return 2

    _dpi_aware()
    root = tk.Tk()
    HostApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
