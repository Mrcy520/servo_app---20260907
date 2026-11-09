# -*- coding: utf-8 -*-
"""
驱动板上位机 · Qt6 / Fluent 界面（v3.2 三页方案）

页面结构（FluentWindow 导航壳，参数配置为默认首页）：
    1. 参数配置 —— 按功能分组的表单卡片，物理值/原始值双输入，修改即写暂存区；
       待锁存清单逐项撤销；参数文件导出/导入/恢复出厂默认
    2. 实时监视 —— 仪表卡 + 10 s 滚动波形（电角度 / Iq）+ 系统状态聚合
    3. 控制·诊断 —— 使能 / 模式选择卡 / 设定值滑条（含下行帧预览）/ 操作，
       以及状态位全量、控制字速查、通信统计、原始帧调试、报文日志

协议语义（与固件一致，协议层 protocol.py / link.py 未改动）：
    写参数只进驱动器暂存区，控制字 bit3 上升沿才锁存生效；
    读取返回生效值；下行控制帧需常驻发送维持 200 ms 链路保护。

注意：不要用 Qt 虚函数名命名窗口属性（如 metric / style / rect），
否则会覆盖 C++ 侧调用导致整个渲染崩溃。
"""

import html
import json
import math
import os
import sys
import threading
import time
from collections import deque

from PySide6.QtCore import QObject, QPointF, QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QApplication, QFileDialog, QFrame, QGridLayout, QHBoxLayout,
    QLabel, QLineEdit, QMessageBox, QScrollArea, QSplitter, QStackedWidget,
    QTextBrowser, QVBoxLayout, QWidget,
)

import protocol as P
import fluent as F

try:
    from qfluentwidgets import FluentIcon as FIF
    from qfluentwidgets import FluentWindow, Slider as FluentSlider
except Exception:
    FIF = None
    FluentWindow = None
    FluentSlider = QSlider

try:
    import link as L
    LINK_ERR = None
except Exception as exc:
    L = None
    LINK_ERR = exc

BAUDS = ["9600", "57600", "115200", "230400", "460800", "921600", "1500000"]
RATES = ["100", "200", "500", "1000"]
CHART_SECONDS = 10.0
CHART_INTERVAL_MS = 50
NAV_EXPAND_WIDTH = 214

# 出厂默认值（与固件 comm_app.c 一致，供「恢复出厂默认」使用）
FACTORY_DEFAULTS = {
    0x01: 4, 0x02: 1, 0x03: 1,
    0x04: 16, 0x05: 16, 0x06: 16, 0x07: 16,
    0x08: 0, 0x09: 193, 0x0A: 16, 0x0B: 200,
}

LOG_COLORS = {"info": "#6B6B6B", "ok": "#0F7B0F", "err": "#C42B1C",
              "tx": "#0B5CAD", "rx": "#4A6B1F"}

EDIT_QSS = """
QLineEdit {
    background:#FFFFFF; border:1px solid #C8C8C8; border-bottom:2px solid #C8C8C8;
    border-radius:4px; padding:2px 7px; min-height:22px;
    font-family:'Consolas'; font-size:9pt; color:#1A1A1A;
}
QLineEdit:focus { border-bottom:2px solid #0067C0; background:#FFFFFF; }
QLineEdit[modified="true"] { border-bottom:2px solid #E9A700; background:#FFFDF5; }
QLineEdit[invalid="true"]  { border-bottom:2px solid #C42B1C; background:#FFF6F5; }
QLineEdit[ro="true"] { background:#F7F7F7; color:#666666; border-bottom:1px solid #DCDCDC; }
"""


# ================================================================ 跨线程桥
class Bridge(QObject):
    log = Signal(str, str, str)     # 文本, 级别, 页面(both/param/ctrl)
    value = Signal(int, int)        # idx, 生效值
    write_ok = Signal(int, int)     # idx, 暂存值
    write_fail = Signal(int, str)
    readall_done = Signal(int)


# ================================================================ 通用小件
def key_label(text):
    lb = QLabel(text)
    lb.setObjectName("MetricKey")
    return lb


def caption(text, mono=False):
    lb = QLabel(text)
    lb.setObjectName("Caption")
    if mono:
        lb.setFont(QFont(F.FONT_MONO, 8))
    return lb


def pill(text):
    lb = QLabel(text)
    lb.setStyleSheet("background:#FFF1D6;color:#B26B00;border-radius:9px;"
                     "padding:2px 8px;font-size:8pt;font-weight:600;")
    lb.setFixedHeight(20)
    return lb


def card(title=None, desc=None):
    c = F.CardWidget()
    outer = QVBoxLayout(c)
    outer.setContentsMargins(14, 12, 14, 14)
    outer.setSpacing(8)
    if title:
        t = QLabel(title)
        t.setObjectName("SectionTitle")
        outer.addWidget(t)
    if desc:
        outer.addWidget(caption(desc))
    inner = QVBoxLayout()
    inner.setContentsMargins(0, 0, 0, 0)
    inner.setSpacing(7)
    outer.addLayout(inner)
    return c, inner


def row(*widgets, spacing=8):
    h = QHBoxLayout()
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(spacing)
    for w in widgets:
        if w is None:
            h.addStretch(1)
        elif isinstance(w, int):
            h.addSpacing(w)
        else:
            h.addWidget(w)
    return h


def make_switch(checked=False):
    sw = F.switch()
    sw.setChecked(checked)
    return sw


# ================================================================ 参数字段
class ParamField(QWidget):
    """单个参数编辑行：0xIdx 名称 | 物理值 | 原始值 | 注记 | 待锁存徽标"""

    commit = Signal(int, int)       # idx, 原始值（用户确认提交）

    def __init__(self, pdef, parent=None):
        super().__init__(parent)
        self.pdef = pdef
        self._syncing = False
        self._locked = False
        self._last_raw = None

        lay = QHBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(8)

        name = QLabel("0x%02X %s" % (pdef.idx, pdef.name))
        name.setObjectName("MetricKey")
        name.setFixedWidth(116)
        lay.addWidget(name)

        self.ed_phys = QLineEdit()
        self.ed_phys.setReadOnly(pdef.ro)
        self.ed_phys.setFixedWidth(104)
        self.ed_phys.setStyleSheet(EDIT_QSS)
        self.ed_phys.setPlaceholderText("物理值")
        self.ed_phys.editingFinished.connect(self._phys_committed)
        lay.addWidget(self.ed_phys)

        self.ed_raw = QLineEdit()
        self.ed_raw.setReadOnly(pdef.ro)
        self.ed_raw.setFixedWidth(92)
        self.ed_raw.setStyleSheet(EDIT_QSS)
        self.ed_raw.setPlaceholderText("原始值")
        self.ed_raw.editingFinished.connect(self._raw_committed)
        lay.addWidget(self.ed_raw)

        note = QLabel(pdef.hint if not pdef.ro else "只读，由固件给出")
        note.setObjectName("Caption")
        note.setFont(QFont(F.FONT_MONO, 8))
        lay.addWidget(note, 1)

        self.pill = pill("待锁存")
        self.pill.hide()
        lay.addWidget(self.pill)

        if pdef.ro:
            for ed in (self.ed_phys, self.ed_raw):
                ed.setProperty("ro", "true")
                ed.setStyleSheet(EDIT_QSS)

    def set_display(self, raw):
        self._syncing = True
        try:
            self.ed_phys.setText(self.pdef.phys_text(raw))
            self.ed_raw.setText(str(raw))
            self._last_raw = raw
        finally:
            self._syncing = False

    def set_pending(self, on, new_raw=None):
        self.pill.setVisible(on)
        for ed in (self.ed_phys, self.ed_raw):
            ed.setProperty("modified", on)
            ed.setStyleSheet(EDIT_QSS)
        if on and new_raw is not None:
            self._syncing = True
            try:
                self.ed_phys.setText(self.pdef.phys_text(new_raw))
                self.ed_raw.setText(str(new_raw))
            finally:
                self._syncing = False

    def refresh(self):
        if self._last_raw is not None:
            self.set_display(self._last_raw)

    def set_invalid(self, on):
        """越界等非法输入：行内红框提示，不用模态弹窗打断。"""
        for ed in (self.ed_phys, self.ed_raw):
            ed.setProperty("invalid", on)
            ed.setStyleSheet(EDIT_QSS)

    def set_locked(self, locked):
        """未连接 / 驱动器离线时锁定编辑，并清空显示——
        旧数据已不可信（可能已被驱动器断电丢失），留着会误导操作者。"""
        self._locked = locked
        for ed, ph in ((self.ed_phys, "物理值"), (self.ed_raw, "原始值")):
            ed.setEnabled(not locked and not self.pdef.ro)
            if locked:
                ed.clear()
                ed.setPlaceholderText("未连接")
            else:
                ed.setPlaceholderText(ph)
        if not locked and self._last_raw is not None:
            self.set_display(self._last_raw)

    def _phys_committed(self):
        if self._syncing or self.pdef.ro or self._locked:
            return
        try:
            phys = float(self.ed_phys.text().strip().split()[0])
        except Exception:
            self.refresh()
            return
        self.commit.emit(self.pdef.idx, self.pdef.from_phys(phys))

    def _raw_committed(self):
        if self._syncing or self.pdef.ro or self._locked:
            return
        try:
            raw = int(self.ed_raw.text().strip(), 0)
        except Exception:
            self.refresh()
            return
        self.commit.emit(self.pdef.idx, raw)


# ================================================================ 实时曲线
class StripChart(QWidget):
    """实时波形：历史不限时长（渐进抽稀控内存），支持滚轮缩放与拖拽平移。

    视图模型：view_span（显示时长秒，None=全部历史） + view_off（距最新数据的回退秒）。
    view_off == 0 即"跟随实时"；拖拽离开实时后可双击或点「回到实时」恢复。
    Y 轴按可见数据自适应缩放。
    """

    MAX_POINTS = 240_000
    PAINT_MAX = 1800
    MIN_SPAN = 0.5

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(280)
        self.setMouseTracking(True)
        from array import array
        self.t = array('d')
        self.s1 = array('d')
        self.s2 = array('d')
        self._origin = None
        self._wall0 = None                  # 首样本的墙钟时间（绝对时刻）
        self.frozen = False
        self._snap = None
        self.view_span = None               # None = 显示全部历史
        self.view_off = 0.0                 # 0 = 右缘贴最新数据
        self._drag_x = None
        self._hover_x = None

    # ------------------------------------------------------------ 数据
    def clear(self):
        from array import array
        self.t = array('d')
        self.s1 = array('d')
        self.s2 = array('d')
        self._origin = None
        self._wall0 = None
        self._snap = None
        self._hover_x = None
        self.update()

    def points(self):
        return len(self.t)

    @property
    def live(self):
        return self.view_off <= 1e-9

    def go_live(self):
        self.view_off = 0.0
        self.update()

    def feed(self, t, n1, n2):
        if self.frozen:
            return
        if self._origin is None:
            self._origin = t
            self._wall0 = t                  # feed 收到的就是绝对墙钟时间
        self.t.append(t - self._origin)
        self.s1.append(max(0.0, min(1.0, n1)))
        self.s2.append(max(0.0, min(1.0, n2)))
        if len(self.t) > self.MAX_POINTS:
            self._decimate()
        if self.isVisible():
            self.update()

    def _decimate(self):
        half = len(self.t) // 2
        self.t[:half] = self.t[0:half:2]
        self.s1[:half] = self.s1[0:half:2]
        self.s2[:half] = self.s2[0:half:2]

    def freeze(self, on):
        self.frozen = on
        if on:
            self._snap = (list(self.t), list(self.s1), list(self.s2))
        else:
            self._snap = None
        self.update()

    # ------------------------------------------------------------ 视图
    def _t_last(self):
        return self.t[-1] if self.t else 0.0

    def _view_range(self, ts):
        t_last = ts[-1] if len(ts) else 0.0
        t_right = t_last - self.view_off
        t_left = (t_right - self.view_span) if self.view_span else ts[0]
        t_left = max(t_left, ts[0] if len(ts) else 0.0)
        return t_left, max(t_right, t_left + 0.05)

    def _zoom(self, ax, factor):
        full = (self.t[-1] - self.t[0]) if len(self.t) > 1 else 1.0
        old = self.view_span if self.view_span else full
        new = max(self.MIN_SPAN, min(old * factor, max(full, old)))
        if self.live:
            self.view_span = None if new >= full else new
        else:
            t_last = self._t_last()
            t_right = t_last - self.view_off
            anchor = t_right - old * (1.0 - ax)
            new_off = t_last - anchor - new * (1.0 - ax)
            self.view_span = None if new >= full else new
            self.view_off = max(0.0, min(new_off, max(0.0, t_last - self.t[0])))
        self.update()

    def _pan(self, dt):
        t_last = self._t_last()
        total = max(0.0, t_last - (self.t[0] if self.t else 0.0))
        self.view_off = max(0.0, min(self.view_off - dt, total))
        self.update()

    # ------------------------------------------------------------ 鼠标
    def wheelEvent(self, ev):
        if self.frozen:
            return
        delta = ev.angleDelta().y()
        if delta == 0:
            return
        ax = max(0.0, min(1.0, ev.position().x() / max(1, self.width())))
        self._zoom(ax, 0.8 if delta > 0 else 1.25)

    def mousePressEvent(self, ev):
        if ev.button() == Qt.LeftButton and not self.frozen:
            self._drag_x = ev.position().x()
            self.setCursor(Qt.ClosedHandCursor)

    def mouseMoveEvent(self, ev):
        if self._drag_x is not None:
            dx = ev.position().x() - self._drag_x
            self._drag_x = ev.position().x()
            span = self.view_span or max(1.0, (self.t[-1] - self.t[0]) if len(self.t) > 1 else 1.0)
            self._pan(dx * span / max(1, self.width()))
            self._hover_x = None
            return
        self._hover_x = ev.position().x()
        self.update()

    def leaveEvent(self, ev):
        self._hover_x = None
        self.update()

    def mouseReleaseEvent(self, ev):
        self._drag_x = None
        self.setCursor(Qt.ArrowCursor)

    def mouseDoubleClickEvent(self, ev):
        self.go_live()

    # ------------------------------------------------------------ 绘制
    def _span_text(self, seconds):
        m, sec = divmod(int(seconds), 60)
        h, m = divmod(m, 60)
        return "%d:%02d:%02d" % (h, m, sec) if h else "%02d:%02d" % (m, sec)

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        p.fillRect(self.rect(), QColor("#FDFDFD"))

        if self.frozen and self._snap is not None:
            ts, s1, s2 = self._snap
        else:
            ts, s1, s2 = self.t, self.s1, self.s2

        n = len(ts)
        p.setPen(QColor("#666666"))
        p.setFont(QFont(F.FONT_MONO, 7))
        if n < 2:
            p.drawText(self.rect().adjusted(44, 0, 0, 0), Qt.AlignCenter, "等待数据…")
            p.end()
            return

        from bisect import bisect_left, bisect_right
        t_left, t_right = self._view_range(ts)
        span = max(0.05, t_right - t_left)
        i0 = max(0, bisect_left(ts, t_left) - 1)
        i1 = min(n, bisect_right(ts, t_right) + 1)

        # ---- 底部时间轴：竖网格 + 绝对时刻标签（HH:MM:SS） ----
        x0a, x1a = 44, w - 8
        step = _nice_step(span)
        first = (math.ceil((t_left - 0.0) / step)) * step
        p.setFont(QFont(F.FONT_MONO, 7))
        tick = first
        while tick <= t_right:
            x = x0a + (x1a - x0a) * (tick - t_left) / span
            p.setPen(QPen(QColor("#ECECEC"), 1))
            p.drawLine(QPointF(x, 6), QPointF(x, h - 18))
            if self._wall0 is not None:
                p.setPen(QColor("#666666"))
                p.drawText(QRectF(x - 46, h - 16, 92, 12), Qt.AlignCenter,
                           time.strftime("%H:%M:%S", time.localtime(self._wall0 + tick)))
            tick += step

        # Y 自适应（可见范围）
        lo, hi = 1e9, -1e9
        for i in range(i0, i1):
            lo = min(lo, s1[i], s2[i])
            hi = max(hi, s1[i], s2[i])
        if hi - lo < 0.02:
            mid = (hi + lo) / 2
            lo, hi = mid - 0.05, mid + 0.05
        pad = (hi - lo) * 0.08
        lo, hi = lo - pad, hi + pad
        y0, y1 = 6, h - 18
        x0, x1 = 44, w - 8

        stride = max(1, (i1 - i0) // self.PAINT_MAX)
        for series, color, break_wrap in ((s1, "#0067C0", True), (s2, "#E9A700", False)):
            path = None
            prev_y = None
            for i in range(i0, i1, stride):
                x = x0 + (x1 - x0) * (ts[i] - t_left) / span
                y = y0 + (y1 - y0) * (1.0 - (max(lo, min(hi, series[i])) - lo) / (hi - lo))
                if path is None:
                    path = QPainterPath(QPointF(x, y))
                elif break_wrap and prev_y is not None and abs(y - prev_y) > (y1 - y0) * 0.6:
                    path.moveTo(x, y)          # 角度 360°→0° 回绕处断开
                else:
                    path.lineTo(x, y)
                prev_y = y
            p.setPen(QPen(QColor(color), 2))
            p.drawPath(path)

        # ---- Y 轴：精度随缩放自适应（平线也能看出量级） ----
        p.setPen(QColor("#666666"))
        deg_rng = (hi - lo) * 360.0
        fmt = "%.0f°" if deg_rng >= 10 else ("%.1f°" if deg_rng >= 1 else "%.2f°")
        p.drawText(QRectF(0, 0, 40, 14), Qt.AlignRight | Qt.AlignVCenter, fmt % (hi * 360.0))
        p.drawText(QRectF(0, h - 30, 40, 14), Qt.AlignRight | Qt.AlignVCenter, fmt % (lo * 360.0))

        # ---- 悬停十字线 + 就近采样读数 ----
        if self._hover_x is not None and self._drag_x is None and not self.frozen:
            hx = self._hover_x
            t_h = t_left + span * (hx - x0a) / max(1, x1a - x0a)
            p.setPen(QPen(QColor("#B0B0B0"), 1, Qt.DashLine))
            p.drawLine(QPointF(hx, 6), QPointF(hx, h - 18))
            idx = max(0, min(n - 1, bisect_left(ts, t_h)))
            if idx < n:
                p.setPen(QColor("#1A1A1A"))
                p.setFont(QFont(F.FONT_MONO, 8))
                txt = "%s  %.2f°  %.3f A" % (
                    time.strftime("%H:%M:%S", time.localtime(self._wall0 + ts[idx])) if self._wall0 else "?",
                    s1[idx] * 360.0, (s2[idx] - 0.5) * 20.0)
                tw = p.fontMetrics().horizontalAdvance(txt) + 12
                bx = min(hx + 6, w - tw - 4)
                p.fillRect(QRectF(bx, 6, tw, 18), QColor("#FFFFFFEE"))
                p.setPen(QColor("#1A1A1A"))
                p.drawText(QRectF(bx, 6, tw, 18), Qt.AlignCenter, txt)

        tag = "实时" if self.live and not self.frozen else (
            "已冻结" if self.frozen else "回看 %s 前" % self._span_text(self.view_off))
        p.drawText(QRectF(w - 250, h - 16, 240, 14), Qt.AlignRight,
                   "%s ｜ 视野 %s ｜ 滚轮缩放 · 拖拽平移" % (tag, self._span_text(span)))
        p.end()


def _nice_step(span, target=7):
    """把 span 分成约 target 段的"1/2/5"整洁步长。"""
    raw = max(1e-3, span / max(1, target))
    mag = 10 ** math.floor(math.log10(raw))
    for m in (1, 2, 5, 10):
        if raw <= m * mag:
            return m * mag
    return 10 * mag


# ================================================================ 降级导航壳
class _FallbackShell(QWidget):
    """qfluentwidgets 缺失时的简易导航壳，暴露与 FluentWindow 相同的少量接口。"""

    def __init__(self):
        super().__init__()
        self.setObjectName("Root")
        self._pages = []
        self.stack = QStackedWidget()
        nav = QFrame()
        nav.setFixedWidth(NAV_EXPAND_WIDTH)
        nav.setStyleSheet("QFrame{background:#FAFAFA;border-right:1px solid #E5E5E5;}")
        self._nav_lay = QVBoxLayout(nav)
        self._nav_lay.setContentsMargins(10, 12, 10, 12)
        self._nav_lay.setSpacing(4)
        self._nav_group = QButtonGroup(self)
        self._nav_group.setExclusive(True)
        self._nav_btns = []
        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        root.addWidget(nav)
        root.addWidget(self.stack, 1)

    def addSubInterface(self, page, _icon, text):
        page.setObjectName(page.__class__.__name__ + text)
        self.stack.addWidget(page)
        b = QPushButton(text)
        b.setCheckable(True)
        b.setFixedHeight(34)
        b.setStyleSheet("""
            QPushButton { text-align:left; padding:0 12px; border:none; border-radius:6px;
                          background:transparent; color:#1A1A1A; }
            QPushButton:checked { background:#E8F1FB; color:#0067C0; font-weight:600; }
            QPushButton:hover { background:#F1F6FB; }
        """)
        b.clicked.connect(lambda: self.switchTo(page))
        self._nav_group.addButton(b)
        self._nav_lay.addWidget(b)
        self._nav_btns.append(b)
        if len(self._nav_btns) == 1:
            b.setChecked(True)

    def switchTo(self, page):
        self.stack.setCurrentWidget(page)
        idx = self.stack.currentIndex()
        if 0 <= idx < len(self._nav_btns):
            self._nav_btns[idx].setChecked(True)


_ShellBase = FluentWindow if FluentWindow is not None else _FallbackShell


# ================================================================ 主窗口
class MainWindow(_ShellBase):

    GROUPS = [
        ("电机与编码器",
         "极对数 / 方向 / 零位须在输出关闭时写入；改零位后需重新自动找零",
         [0x01, 0x02, 0x03, 0x08]),
        ("电流环 PI",
         "物理值 = 写入值 ÷ 16，范围 1.0 ~ 200000.0",
         [0x04, 0x05, 0x06, 0x07]),
        ("电压与限幅",
         "随母线电压确定：增益 ≈ 5408/U · 限幅 ≈ U/1.732",
         [0x09, 0x0A]),
        ("通信与版本",
         "超时门限同时用于下行链路保护",
         [0x0B, 0x0C, 0x0D]),
    ]

    def __init__(self):
        super().__init__()
        self.setWindowTitle("驱动板参数上位机 · Qt6 / Fluent")

        self.link = None
        self.ctrl = None                 # ControlSender（命名避开 QObject.sender()）
        self.values = {}                 # idx -> 生效值
        self.pending = {}                # idx -> (新值, 旧值)
        self.fields = {}                 # idx -> ParamField
        self.offline = False             # 上行帧看门狗判定：驱动器离线
        self.cur_mode = P.MODE_VOLTAGE
        self._syncing = False
        self._busy = False
        self._hist_t = deque()
        self._hist_ang = deque()

        self.bridge = Bridge()
        self.bridge.log.connect(self._log)
        self.bridge.value.connect(self._apply_value)
        self.bridge.write_ok.connect(self._on_write_ok)
        self.bridge.write_fail.connect(self._on_write_fail)
        self.bridge.readall_done.connect(self._read_all_done)

        self.page_params = self._build_params_page()
        self.page_monitor = self._build_monitor_page()
        self.page_control = self._build_control_page()
        if F.HAS_FLUENT:
            self.addSubInterface(self.page_params, FIF.SETTING, "参数配置")
            self.addSubInterface(self.page_monitor, FIF.HOME, "实时监视")
            self.addSubInterface(self.page_control, FIF.POWER_BUTTON, "控制 · 诊断")
            try:
                ni = self.navigationInterface
                ni.setMinimumExpandWidth(1200)   # 宽窗口时导航常驻展开
                ni.setExpandWidth(NAV_EXPAND_WIDTH)
            except Exception:
                pass
        else:
            self.addSubInterface(self.page_params, None, "参数配置")
            self.addSubInterface(self.page_monitor, None, "实时监视")
            self.addSubInterface(self.page_control, None, "控制 · 诊断")
        try:
            self.stackedWidget.setAnimationEnabled(False)   # 关闭淡入淡出，切页零延迟
        except Exception:
            pass
        self.resize(1500, 920)
        self.setMinimumSize(1440, 720)
        self.setMinimumSize(1160, 720)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(CHART_INTERVAL_MS)

        self._set_ui_locked(True)            # 未连接时参数页/控制页不可修改
        QTimer.singleShot(0, self._startup_log)
        QTimer.singleShot(150, self._refresh_ports)

    # ================================================================ 页 1
    def _build_params_page(self):
        w = QWidget()
        w.setObjectName("params")
        v = QVBoxLayout(w)
        v.setContentsMargins(20, 14, 20, 14)
        v.setSpacing(10)

        conn, cl = card()
        self.cb_port = F.port_box()
        self.btn_refresh = F.PushButton("刷新")
        self.btn_refresh.setFixedHeight(28)
        self.btn_refresh.clicked.connect(self._refresh_ports)
        self.cb_baud = F.ComboBox()
        self.cb_baud.addItems(BAUDS)
        self.cb_baud.setCurrentText("921600")
        self.cb_baud.setFixedWidth(104)
        self.sp_addr = F.SpinBox()
        self.sp_addr.setRange(1, 247)
        self.sp_addr.setValue(1)
        self.sp_addr.setFixedWidth(74)
        self.cb_hz = F.ComboBox()
        self.cb_hz.addItems(RATES)
        self.cb_hz.setCurrentText("500")
        self.cb_hz.setFixedWidth(84)
        self.btn_conn = F.PrimaryPushButton("连接")
        self.btn_conn.setFixedHeight(32)
        self.btn_conn.clicked.connect(self._toggle_conn)
        self.lb_conn = QLabel("● 未连接")
        self.lb_conn.setStyleSheet("color:%s;font-weight:600;" % F.ERR_RED)

        cl.addLayout(row(
            key_label("串口"), self.cb_port, self.btn_refresh, 10,
            key_label("波特率"), self.cb_baud, 10,
            key_label("站号"), self.sp_addr, 10,
            key_label("控制帧"), self.cb_hz, key_label("Hz"), 12,
            self.btn_conn, self.lb_conn, None))
        self.lb_stat = caption("")
        self.lb_stat.setFont(QFont(F.FONT_MONO, 8))
        cl.addLayout(row(self.lb_stat, None))
        v.addWidget(conn)
        act = QHBoxLayout()
        act.setSpacing(8)
        self.pend_pill = pill("待锁存 0 项")
        self.pend_pill.hide()
        act.addWidget(self.pend_pill)
        act.addWidget(caption("修改即写入暂存区；「重新读取」放弃未锁存修改；「锁存生效」后参与运算并自动回读"))
        act.addStretch(1)
        self.btn_reload = F.PushButton("重新读取")
        self.btn_reload.setFixedHeight(30)
        self.btn_reload.clicked.connect(self._read_all)
        self.btn_undo_all = F.PushButton("全部撤销")
        self.btn_undo_all.setFixedHeight(30)
        self.btn_undo_all.clicked.connect(self._undo_all)
        self.btn_latch = F.PrimaryPushButton("锁存生效")
        self.btn_latch.setFixedHeight(32)
        self.btn_latch.clicked.connect(self._latch)
        act.addWidget(self.btn_reload)
        act.addWidget(self.btn_undo_all)
        act.addWidget(self.btn_latch)
        v.addLayout(act)

        body = QWidget()
        grid = QGridLayout(body)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(10)

        for gi, (title, desc, idxs) in enumerate(self.GROUPS):
            c, lay = card(title, desc)
            for idx in idxs:
                f = ParamField(P.PARAM_BY_IDX[idx])
                f.commit.connect(self._commit_field)
                self.fields[idx] = f
                lay.addWidget(f)
            r, cidx = divmod(gi, 2)
            grid.addWidget(c, r, cidx)

        self.pend_card, self.pend_lay = card(
            "待锁存清单",
            "锁存前不参与运算，读参数仍返回旧值；「撤销」把旧值重新写回暂存区。\n重新连接或驱动器重新上电后，暂存状态以驱动器读回值为准，本清单自动清空")
        self._pend_rows = []
        grid.addWidget(self.pend_card, 2, 0)

        fc, fl = card("参数文件")
        btns = QHBoxLayout()
        btns.setSpacing(8)
        self._file_btns = []
        for text, slot in (("导出 JSON", self._export), ("导入 JSON", self._import),
                           ("恢复出厂默认", self._factory_reset)):
            b = F.PushButton(text)
            b.setFixedHeight(30)
            b.clicked.connect(slot)
            self._file_btns.append(b)
            btns.addWidget(b)
        btns.addStretch(1)
        btns.addWidget(caption("导出内容为当前生效值；导入会逐项写入暂存区"))
        fl.addLayout(btns)
        grid.addWidget(fc, 2, 1)

        grid.setRowStretch(3, 1)
        sc = QScrollArea()
        sc.setWidgetResizable(True)
        sc.setFrameShape(QFrame.NoFrame)
        sc.setWidget(body)
        v.addWidget(sc, 1)

        lgc, lgl = card("报文日志", "参数读写、锁存与连接事件")
        lb_row = QHBoxLayout()
        lb_row.addWidget(caption("自动滚动"))
        self.sw_auto_p = make_switch()
        self.sw_auto_p.setChecked(True)
        lb_row.addWidget(self.sw_auto_p)
        lb_row.addStretch(1)
        b_clr = F.PushButton("清空")
        b_clr.setFixedHeight(28)
        b_clr.clicked.connect(lambda: self.txt_params.clear())
        lb_row.addWidget(b_clr)
        lgl.addLayout(lb_row)
        self.txt_params = QTextBrowser()
        self.txt_params.setMinimumHeight(140)
        self.txt_params.document().setMaximumBlockCount(1200)
        lgl.addWidget(self.txt_params, 1)
        v.addWidget(lgc)
        return w

    def _toggle_angle_mode(self, event=None):
        self.angle_use_cal = not getattr(self, "angle_use_cal", True)
        if hasattr(self, "lb_ang_mode"):
            if self.angle_use_cal:
                self.lb_ang_mode.setText("[映射 0~360°]")
                self.lb_ang_mode.setStyleSheet("color:#0078D4;font-size:8pt;font-weight:600;")
            else:
                self.lb_ang_mode.setText("[原始 270~360°]")
                self.lb_ang_mode.setStyleSheet("color:#D83B01;font-size:8pt;font-weight:600;")

    # ================================================================ 页 2
    def _build_monitor_page(self):
        w = QWidget()
        w.setObjectName("monitor")
        v = QVBoxLayout(w)
        v.setContentsMargins(20, 14, 20, 14)
        v.setSpacing(10)

        top_w = QWidget()
        g = QHBoxLayout(top_w)
        g.setContentsMargins(0, 0, 0, 0)
        g.setSpacing(10)
        self.metrics = {}
        self.angle_use_cal = True
        for cap_text, unit, attr in (("电角度", "°", "angle"), ("Iq 电流", "A", "iq"),
                                     ("母线电压", "V", "vbus"), ("转速估算", "rpm", "rpm")):
            c, lay = card()
            lay.addWidget(caption(cap_text))
            h = QHBoxLayout()
            h.setSpacing(6)
            lb = QLabel("—")
            lb.setFont(QFont(F.FONT_MONO, 22, QFont.DemiBold))
            lb.setAlignment(Qt.AlignVCenter)
            h.addWidget(lb)
            u = QLabel(unit)
            u.setStyleSheet("color:%s;font-size:11pt;" % F.TEXT_2)
            u.setAlignment(Qt.AlignVCenter)
            h.addWidget(u)
            h.addStretch(1)
            lay.addLayout(h)
            if attr == "angle":
                self.lb_ang_mode = QLabel("[映射 0~360°]")
                self.lb_ang_mode.setStyleSheet("color:#0078D4;font-size:8pt;font-weight:600;")
                self.lb_ang_mode.setToolTip("点击切换口径：调试映射(后1/4段270~360°映射到0~360°) / 原始满量程(270~360°)")
                self.lb_ang_mode.mousePressEvent = self._toggle_angle_mode
                lay.addWidget(self.lb_ang_mode)
            self.metrics[attr] = lb
            g.addWidget(c, 1)

        st_card, st_lay = card()
        st_lay.addWidget(caption("系统状态"))
        self.lb_state = QLabel("等待连接")
        self.lb_state.setFont(QFont(F.FONT_UI, 18, QFont.DemiBold))
        self.lb_state.setStyleSheet("color:%s;" % F.TEXT_3)
        st_lay.addWidget(self.lb_state)
        self.dots_lay = QHBoxLayout()
        self.dots_lay.setSpacing(10)
        self.dot_en = QLabel("● 停止")
        self.dot_fault = QLabel("● 无故障")
        self.dot_link = QLabel("● 链路 —")
        for d in (self.dot_en, self.dot_fault, self.dot_link):
            d.setStyleSheet("color:#8A8A8A;font-size:9pt;font-weight:600;")
            self.dots_lay.addWidget(d)
        self.dots_lay.addStretch(1)
        st_lay.addLayout(self.dots_lay)
        self.lb_live = caption("")
        self.lb_live.setFont(QFont(F.FONT_MONO, 8))
        st_lay.addWidget(self.lb_live)
        self.lb_frame_info = caption("")
        self.lb_frame_info.setFont(QFont(F.FONT_MONO, 8))
        st_lay.addWidget(self.lb_frame_info)
        g.addWidget(st_card, 1)
        v.addLayout(g)

        cc, cl = card("实时波形（不限时长，历史自动抽稀以控制内存）")
        lg = QHBoxLayout()
        lg.setSpacing(16)
        self.lb_leg_ang = QLabel("▬ 电角度 —")
        self.lb_leg_iq = QLabel("▬ Iq —")
        self.lb_leg_ang.setStyleSheet("color:#0067C0;font-size:9pt;font-weight:600;")
        self.lb_leg_iq.setStyleSheet("color:#E9A700;font-size:9pt;font-weight:600;")
        lg.addWidget(self.lb_leg_ang)
        lg.addWidget(self.lb_leg_iq)
        self.lb_hist = caption("")
        self.lb_hist.setFont(QFont(F.FONT_MONO, 8))
        lg.addWidget(self.lb_hist)
        lg.addStretch(1)
        lg.addWidget(caption("冻结"))
        self.sw_freeze = make_switch()
        self.sw_freeze.checkedChanged.connect(self.chart_freeze)
        lg.addWidget(self.sw_freeze)
        self.btn_live = F.PushButton("回到实时")
        self.btn_live.setFixedHeight(28)
        self.btn_live.setEnabled(False)
        lg.addWidget(self.btn_live)
        b_clear_chart = F.PushButton("清空波形")
        b_clear_chart.setFixedHeight(28)
        lg.addWidget(b_clear_chart)
        cl.addLayout(lg)
        self.chart = StripChart()
        b_clear_chart.clicked.connect(self.chart.clear)
        self.btn_live.clicked.connect(self.chart.go_live)
        cl.addWidget(self.chart, 1)

        split = QSplitter(Qt.Vertical)
        split.setChildrenCollapsible(False)
        split.setHandleWidth(10)
        split.addWidget(top_w)
        split.addWidget(cc)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        split.setSizes([230, 620])
        v.addWidget(split, 1)
        return w

    def chart_freeze(self, on):
        self.chart.freeze(bool(on))

    # ================================================================ 页 3
    def _build_control_page(self):
        w = QWidget()
        w.setObjectName("control")
        page_lay = QVBoxLayout(w)
        page_lay.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        inner = QWidget()
        v = QVBoxLayout(inner)
        v.setContentsMargins(20, 14, 20, 14)
        v.setSpacing(10)
        page_lay.addWidget(scroll)
        scroll.setWidget(inner)

        enc, el = card()
        er = QHBoxLayout()
        er.setSpacing(14)
        self.sw_enable = make_switch()
        self.sw_enable.checkedChanged.connect(self._on_enable)
        self._ctrl_lockables = [self.sw_enable]
        er.addWidget(self.sw_enable)
        tv = QVBoxLayout()
        t1 = QLabel("输出使能")
        t1.setFont(QFont(F.FONT_UI, 14, QFont.DemiBold))
        tv.addWidget(t1)
        tv.addWidget(caption("bit0 电平有效 · 断开连接 200 ms 后自动关闭 · 故障锁存期间拒绝使能"))
        er.addLayout(tv)
        er.addStretch(1)
        self.lb_enable = QLabel("输出关闭")
        self.lb_enable.setFont(QFont(F.FONT_UI, 13, QFont.DemiBold))
        self.lb_enable.setStyleSheet("color:%s;" % F.TEXT_3)
        er.addWidget(self.lb_enable)
        el.addLayout(er)
        v.addWidget(enc)

        mc, ml = card("控制模式")
        mr = QHBoxLayout()
        mr.setSpacing(10)
        self.mode_cards = {}
        for mode, title, sub in ((P.MODE_VOLTAGE, "电压开环", "值1 = Uq ｜ 值2 = Ud"),
                                 (P.MODE_CURRENT, "电流闭环", "值1 = Iq ｜ 值2 = Id")):
            c = QFrame()
            c.setObjectName("ModeCard")
            c.setFixedHeight(58)
            c.setCursor(Qt.PointingHandCursor)
            lay = QVBoxLayout(c)
            lay.setContentsMargins(14, 7, 14, 7)
            t = QLabel(title)
            t.setFont(QFont(F.FONT_UI, 11, QFont.DemiBold))
            s = QLabel(sub)
            s.setStyleSheet("color:%s;font-size:8pt;" % F.TEXT_3)
            lay.addWidget(t)
            lay.addWidget(s)
            c.mousePressEvent = (lambda ev, m=mode: self._pick_mode(m))   # 点击整卡切换
            self.mode_cards[mode] = (c, t)
            mr.addWidget(c)
        self._style_mode(self.cur_mode)
        ml.addLayout(mr)
        v.addWidget(mc)

        rc, rl = card("设定值（-100 % ~ +100 % ↔ ±32767，四舍五入取整）")
        self.ref_sliders = {}
        self.ref_values = {}
        self.ref_raws = {}
        for name, attr in (("设定值1", "ref1"), ("设定值2", "ref2")):
            h = QHBoxLayout()
            h.setSpacing(12)
            k = key_label(name)
            k.setFixedWidth(52)
            sl = FluentSlider(Qt.Horizontal)
            sl.setRange(-P.CTRL_REF_MAX, P.CTRL_REF_MAX)
            sl.setValue(0)
            v_lb = QLabel("+0.00 %")
            v_lb.setFont(QFont(F.FONT_MONO, 13, QFont.DemiBold))
            v_lb.setFixedWidth(96)
            v_lb.setAlignment(Qt.AlignCenter)
            e = QLineEdit("0")
            e.setFixedWidth(76)
            e.setStyleSheet(EDIT_QSS)
            e.setAlignment(Qt.AlignCenter)
            sl.valueChanged.connect(lambda val, a=attr: self._slider_moved(a, val))
            e.editingFinished.connect(lambda a=attr: self._raw_edited(a))
            h.addWidget(k)
            h.addWidget(sl, 1)
            h.addWidget(v_lb)
            h.addWidget(e)
            self.ref_sliders[attr] = sl
            self.ref_values[attr] = v_lb
            self.ref_raws[attr] = e
            self._ctrl_lockables += [sl, e]
            rl.addLayout(h)
        self.lb_frame = caption("")
        self.lb_frame.setFont(QFont(F.FONT_MONO, 8))
        rl.addWidget(self.lb_frame)
        v.addWidget(rc)

        oc, ol = card("操作")
        orow = QHBoxLayout()
        orow.setSpacing(8)
        b_clear = F.PushButton("清故障 (bit1)")
        b_clear.setFixedHeight(30)
        b_clear.clicked.connect(self._clear_fault)
        orow.addWidget(b_clear)
        orow.addWidget(key_label("找零电压"))
        self.ed_zero = QLineEdit("10.0 %")
        self.ed_zero.setFixedWidth(92)
        self.ed_zero.setStyleSheet(EDIT_QSS)
        self.ed_zero.setAlignment(Qt.AlignCenter)
        orow.addWidget(self.ed_zero)
        b_zero = F.PushButton("自动找零 (bit2)")
        b_zero.setFixedHeight(30)
        b_zero.clicked.connect(self._zero)
        orow.addWidget(b_zero)
        self._ctrl_lockables += [b_clear, self.ed_zero, b_zero]
        orow.addStretch(1)
        b_stop = F.PrimaryPushButton("停止输出")
        b_stop.setFixedHeight(32)
        b_stop.clicked.connect(self._stop_output)
        orow.addWidget(b_stop)
        self._ctrl_lockables.append(b_stop)
        ol.addLayout(orow)
        v.addWidget(oc)

        stc, stl = card("驱动器状态位", "激活亮色，未激活置灰；控制字 bit4~7 为保留位")
        chips = QHBoxLayout()
        chips.setSpacing(6)
        self.chips = {}
        for bit, short, desc in P.STATUS_BITS:
            lb = QLabel("0x%02X %s %s" % (bit, short, desc))
            lb.setToolTip(desc)
            lb.setStyleSheet("background:#EDEDED;color:#666666;border-radius:4px;"
                             "padding:4px 9px;font-size:8pt;")
            self.chips[bit] = lb
            chips.addWidget(lb)
        chips.addStretch(1)
        stl.addLayout(chips)
        stl.addWidget(caption("控制字：bit0 使能(电平) · bit1 清故障(上升沿) · bit2 自动找零(上升沿，bit0 须为 0)"
                              " · bit3 参数锁存(上升沿) · bit4~7 保留填 0"))
        v.addWidget(stc)

        dgc, dgl = card("通信统计")
        dh = QHBoxLayout()
        dh.addStretch(1)
        b_reset = F.PushButton("重置统计")
        b_reset.setFixedHeight(28)
        b_reset.clicked.connect(self._reset_stats)
        dh.addWidget(b_reset)
        dgl.addLayout(dh)
        grid = QGridLayout()
        grid.setHorizontalSpacing(30)
        grid.setVerticalSpacing(6)
        self.diag = {}
        for i, k in enumerate(("上行帧率", "参数往返", "累计上行", "参数 TX/RX",
                               "超时重发", "错帧", "链龄", "站号")):
            r, c = divmod(i, 2)
            grid.addWidget(key_label(k), r, c * 2)
            lb = QLabel("—")
            lb.setFont(QFont(F.FONT_MONO, 9))
            grid.addWidget(lb, r, c * 2 + 1)
            self.diag[k] = lb
        grid.setColumnStretch(1, 1)
        grid.setColumnStretch(3, 1)
        dgl.addLayout(grid)
        v.addWidget(dgc)

        rwc, rwl = card("原始帧调试")
        rh = QHBoxLayout()
        rh.setSpacing(8)
        self.ed_raw = QLineEdit()
        self.ed_raw.setPlaceholderText("十六进制字节，例如 5A A5 01 01 09 00 00 00 00 00 00 00 00 00 1D 0F")
        b_send = F.PushButton("发送")
        b_send.setFixedHeight(30)
        b_send.clicked.connect(self._send_raw)
        rh.addWidget(self.ed_raw, 1)
        rh.addWidget(b_send)
        self._ctrl_lockables += [self.ed_raw, b_send]
        rwl.addLayout(rh)
        v.addWidget(rwc)

        lgc, lgl = card("报文日志")
        lb_row = QHBoxLayout()
        lb_row.addWidget(caption("自动滚动"))
        self.sw_auto = make_switch()
        self.sw_auto.setChecked(True)
        lb_row.addWidget(self.sw_auto)
        lb_row.addStretch(1)
        b_clr = F.PushButton("清空")
        b_clr.setFixedHeight(28)
        b_clr.clicked.connect(lambda: self.txt_ctrl.clear())
        lb_row.addWidget(b_clr)
        lgl.addLayout(lb_row)
        self.txt_ctrl = QTextBrowser()
        self.txt_ctrl.setMinimumHeight(160)
        self.txt_ctrl.document().setMaximumBlockCount(1200)
        lgl.addWidget(self.txt_ctrl, 1)
        v.addWidget(lgc)
        v.addStretch(1)
        return w

    # ============================================================ 启动信息
    def _startup_log(self):
        self._log("就绪。在串口栏选择端口后点击「连接」，连接后自动读取全部参数。", "info")
        self._log("解释器：%s" % sys.executable, "info")
        self._log("Qt：PySide6 %s ｜ 组件：%s"
                  % (getattr(__import__("PySide6"), "__version__", "?"),
                     "Fluent Widgets" if F.HAS_FLUENT else "原生控件降级"), "info")
        if L is None:
            self._log("串口模块加载失败：%s" % LINK_ERR, "err")
        elif L.PYSERIAL_OK:
            self._log("pyserial %s 就绪" % L.PYSERIAL_VER, "ok")
        else:
            self._log("pyserial 未安装：%s" % L.PYSERIAL_ERR, "err")
            self._log('请执行："%s" -m pip install pyserial' % sys.executable, "err")

    # ============================================================ 串口
    def _refresh_ports(self):
        if L is None:
            self._log("串口模块加载失败：%s" % LINK_ERR, "err")
            return
        ports = L.list_ports()
        devices = [d for d, _ in ports]
        if ports:
            cur = F.port_text(self.cb_port).upper()
            F.fill_ports(self.cb_port, devices)
            if cur not in devices:
                pick = devices[0]
                for dev, desc in ports:
                    if any(x in (desc or "").upper()
                           for x in ("USB", "WCH", "CH34", "CP210", "FTDI", "SILICON")):
                        pick = dev
                        break
                F.set_port(self.cb_port, pick)
            else:
                F.set_port(self.cb_port, cur)
            self._log("发现 %d 个串口：%s"
                      % (len(ports),
                         ", ".join("%s(%s)" % (d, s) if s else d for d, s in ports)), "ok")
            return
        F.fill_ports(self.cb_port, ["COM%d" % i for i in range(1, 33)])
        if not L.PYSERIAL_OK:
            self._log("未安装 pyserial：%s" % L.PYSERIAL_ERR, "err")
            self._log("当前解释器：%s" % sys.executable, "err")
            self._log('请执行："%s" -m pip install pyserial' % sys.executable, "err")
        else:
            self._log("系统未枚举到任何串口（pyserial %s 已就绪）。"
                      "请检查设备与驱动，或直接输入端口名（如 COM38）。" % L.PYSERIAL_VER, "err")

    def _connected(self):
        return (self.link is not None and self.link.is_open
                and not self.offline)

    # ------------------------------------------------------------ 联机状态
    def _set_ui_locked(self, locked):
        """未连接 / 驱动器离线时，参数页与控制页整体进入只读锁定。"""
        for f in self.fields.values():
            f.set_locked(locked)
        for b in (self.btn_reload, self.btn_undo_all, self.btn_latch):
            b.setEnabled(not locked)
        for b in getattr(self, "_file_btns", []):
            b.setEnabled(not locked)
        for wdg in getattr(self, "_ctrl_lockables", []):
            wdg.setEnabled(not locked)
        if locked:
            self.sw_enable.setChecked(False)
        self.btn_latch.setText("锁存生效 (%d)" % len(self.pending)
                               if self.pending else "锁存生效")

    def _enter_offline(self):
        """上行帧看门狗：驱动器断电后 USB 转串口仍在线，但上行帧停止，
        据此判定离线并锁定参数页；恢复供电（上行帧恢复）后自动重联重读。"""
        if self.offline:
            return
        self.offline = True
        self._log("通信中断：已 2 s 未收到驱动器上行帧，判定驱动器离线（串口本身仍在线）。"
                  "参数页已锁定，恢复供电后将自动重新联机。", "err", "both")
        self._invalidate_pending("驱动器离线：暂存状态已失效，重新联机后以芯片数据为准")
        self._set_ui_locked(True)
        self.sw_enable.setChecked(False)
        if self.ctrl is not None:
            self.ctrl.set_enable(False)
        self.btn_conn.setText("重连")
        self.lb_conn.setText("● 通信中断（驱动器无响应）")
        self.lb_conn.setStyleSheet("color:%s;font-weight:600;" % F.ERR_RED)

    def _exit_offline(self):
        if not self.offline:
            return
        self.offline = False
        self._log("检测到驱动器上行帧恢复，自动重新联机并读取参数。", "ok", "both")
        self._set_ui_locked(False)
        self.btn_conn.setText("断开")
        self.lb_conn.setText("● 已连接 %s @ %s"
                             % (F.port_text(self.cb_port), self.cb_baud.currentText()))
        self.lb_conn.setStyleSheet("color:%s;font-weight:600;" % F.OK_GREEN)
        self._invalidate_pending("重新联机：待锁存状态已失效，以下方读回值为准")
        self._read_all()

    def _toggle_conn(self):
        if self.offline:
            self._disconnect()
            self._connect()
            return
        if self._connected():
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        if L is None:
            QMessageBox.critical(self, "缺少依赖", "串口模块加载失败：\n%s" % LINK_ERR)
            return
        if not L.PYSERIAL_OK:
            QMessageBox.critical(
                self, "缺少 pyserial",
                "当前解释器没有安装 pyserial：\n%s\n\n%s\n\n请执行：\n\"%s\" -m pip install pyserial"
                % (sys.executable, L.PYSERIAL_ERR, sys.executable))
            return
        port = F.port_text(self.cb_port).upper()
        if port.isdigit():
            port = "COM" + port
        if not port:
            QMessageBox.warning(self, "未选择串口", "请选择或直接输入串口名（如 COM38）。")
            return
        try:
            baud = int(self.cb_baud.currentText())
            addr = int(self.sp_addr.value())
            hz = float(self.cb_hz.currentText())
        except Exception:
            QMessageBox.critical(self, "参数错误", "波特率 / 站号 / 控制帧频率填写有误。")
            return
        if not 1 <= addr <= 247:
            QMessageBox.critical(self, "站号越界", "站号必须为 1 ~ 247（表 2-1）。")
            return

        self.link = L.SerialLink(port, baud, addr, log=self._log_t)
        try:
            self.link.open()
        except Exception as exc:
            self.link = None
            QMessageBox.critical(self, "打开串口失败", "%s\n\n%s" % (port, exc))
            return

        self.ctrl = L.ControlSender(self.link, hz=hz, mode=self.cur_mode)
        self.ctrl.set_enable(False)
        self.ctrl.set_refs(self._ref_pct("ref1"), self._ref_pct("ref2"))
        self.ctrl.start()

        self.offline = False
        self._set_ui_locked(False)
        self.btn_conn.setText("断开")
        self.lb_conn.setText("● 已连接 %s @ %d" % (port, baud))
        self.lb_conn.setStyleSheet("color:%s;font-weight:600;" % F.OK_GREEN)
        self.sp_addr.setEnabled(False)
        self.btn_refresh.setEnabled(False)
        self._log("已连接 %s @ %d 8N1，站号 %d，控制帧 %d Hz" % (port, baud, addr, int(hz)), "ok")
        self._log("提示：驱动板若为调试档（COMM_DEBUG_REPORT=1），串口只输出文本、参数链路静默。", "info")
        # 重新连接 = 新会话：驱动器可能已重新上电（暂存区是 RAM，掉电即丢），
        # 宿主端记录的“待锁存”不再可信，一律失效，以读回的生效值（芯片/NVM 数据）为准。
        self._invalidate_pending("重新连接：待锁存状态已失效，以下方读回值为准")
        self._update_frame_preview()
        QTimer.singleShot(200, self._read_all)

    def _disconnect(self):
        self.sw_enable.setChecked(False)
        if self.ctrl is not None:
            self.ctrl.set_enable(False)
            time.sleep(0.05)
            self.ctrl.stop()
            self.ctrl = None
        if self.link is not None:
            self.link.close()
            self.link = None
        self.offline = False
        self._set_ui_locked(True)
        self.btn_conn.setText("连接")
        self.lb_conn.setText("● 未连接")
        self.lb_conn.setStyleSheet("color:%s;font-weight:600;" % F.ERR_RED)
        self.sp_addr.setEnabled(True)
        self.btn_refresh.setEnabled(True)
        self.lb_stat.setText("")
        self.lb_state.setText("等待连接")
        self.lb_state.setStyleSheet("color:%s;" % F.TEXT_3)
        self._log("已断开。驱动器应在 200 ms 后关闭输出。", "info")
        self._invalidate_pending("已断开：待锁存状态已失效，重新连接后以读回值为准")
        self._update_frame_preview()

    # ============================================================ 日志
    def _log(self, msg, level="info", page="both"):
        color = LOG_COLORS.get(level, LOG_COLORS["info"])
        ts = time.strftime("%H:%M:%S")
        html_line = ('<span style="color:#666666">[%s]</span> '
                     '<span style="color:%s">%s</span>' % (ts, color, html.escape(msg)))
        targets = []
        if page in ("both", "param"):
            targets.append((self.txt_params, self.sw_auto_p))
        if page in ("both", "ctrl"):
            targets.append((self.txt_ctrl, self.sw_auto))
        for box, sw in targets:
            box.append(html_line)
            if sw.isChecked():
                sb = box.verticalScrollBar()
                sb.setValue(sb.maximum())

    def _log_t(self, msg, level="info", page="both"):
        self.bridge.log.emit(msg, level, page)

    def _invalidate_pending(self, reason=""):
        """宿主端“待锁存”只是本会话的记忆：驱动器重新上电会丢失暂存区（RAM），
        因此重新连接/断开后一律失效，参数显示以读回的生效值（芯片/NVM 数据）为准。"""
        had = bool(self.pending)
        self.pending.clear()
        for f in self.fields.values():
            f.set_pending(False)
        self._refresh_pending_list()
        if had and reason:
            self._log(reason, "info", "param")

    # ============================================================ 参数读取
    def _read_all(self):
        if not self._connected() or self._busy:
            return
        self._busy = True
        self.btn_reload.setEnabled(False)
        # 用户语义：重新读取 = 放弃全部未锁存修改，读回值直接覆盖界面。
        # 同时必须把生效值写回设备暂存区——否则设备侧残留的未锁存值
        # 在之后误按「锁存生效」时仍会被应用。
        discard = dict(self.pending)
        if discard:
            self.pending.clear()
            for f in self.fields.values():
                f.set_pending(False)
            self._refresh_pending_list()
            self._log("重新读取：放弃 %d 项未锁存修改，以驱动器生效值为准" % len(discard),
                      "info", "param")
        # 批量读时把控制帧降到 100 Hz 让出总线，降低参数应答被挤丢的概率
        self._saved_hz = None
        if self.ctrl is not None:
            self._saved_hz = float(self.cb_hz.currentText())
            self.ctrl.set_rate(100)
        self._log("开始读取全部参数（0x01 ~ 0x0D）…", "info", "param")

        def work():
            for idx, (_new, _old) in discard.items():
                eff = self.values.get(idx)
                if eff is None or not self._connected():
                    continue
                rsp = self.link.write_param(idx, eff)
                if rsp is not None and rsp["status"] == P.PARAM_ST_OK:
                    self._log_t("0x%02X 未锁存修改已放弃（生效值 %d 写回暂存区）"
                                % (idx, eff), "info", "param")
            bad = 0
            for p in P.PARAMS:
                if not self._connected():
                    break
                rsp = self.link.read_param(p.idx, timeout=0.12, retries=4)
                if rsp is None:
                    bad += 1
                    self._log_t("读 0x%02X %s：无应答（超时）" % (p.idx, p.name), "err", "param")
                    continue
                if rsp["status"] != P.PARAM_ST_OK:
                    bad += 1
                    self._log_t("读 0x%02X %s：%s（0x%02X）"
                                % (p.idx, p.name, P.STATUS_NAME.get(rsp["status"], "?"),
                                   rsp["status"]), "err", "param")
                    continue
                self.bridge.value.emit(p.idx, rsp["value"])
                self._log_t("读 0x%02X %-8s = %-10d (%s)"
                            % (p.idx, p.name, rsp["value"], p.phys_text(rsp["value"])), "rx", "param")
            self.bridge.readall_done.emit(bad)

        threading.Thread(target=work, daemon=True).start()

    def _read_all_done(self, bad):
        self._busy = False
        self.btn_reload.setEnabled(True)
        if self.ctrl is not None and self._saved_hz:
            self.ctrl.set_rate(self._saved_hz)
            self._saved_hz = None
        extra = ""
        if self.link is not None:
            st = self.link.stats
            extra = "｜重发 %d 次 / 超时 %d 次" % (st.retries, st.timeouts)
        self._log("读取完成，失败 %d 项%s" % (bad, extra),
                  "ok" if bad == 0 else "err", "param")
        if bad == len(P.PARAMS) and self.link is not None and self.link.stats.ctrl_rx == 0:
            self._log("驱动器无任何应答：可能未上电，或处于调试档（协议静默）。"
                      "参数页已锁定；恢复供电后上行帧会触发自动重连。", "err", "param")
            self._enter_offline()

    def _apply_value(self, idx, raw):
        self.values[idx] = raw
        f = self.fields.get(idx)
        if f is not None and idx not in self.pending:
            f.set_display(raw)

    # ============================================================ 参数写入
    def _commit_field(self, idx, raw):
        if not self._connected():
            self._log("未连接，0x%02X 的修改未提交" % idx, "err", "param")
            self.fields[idx].refresh()
            return
        p = P.PARAM_BY_IDX[idx]
        if not p.in_range(raw):
            self._log("0x%02X %s = %d 超出量程 %d ~ %d，已拒绝"
                      % (idx, p.name, raw, p.rmin, p.rmax), "err", "param")
            self.fields[idx].set_invalid(True)
            self.fields[idx].refresh()
            QTimer.singleShot(1200, lambda f=self.fields[idx]: f.set_invalid(False))
            return
        if raw == self.values.get(idx):
            self.fields[idx].set_display(raw)
            return
        self._write_param(idx, raw)

    def _write_param(self, idx, raw):
        p = P.PARAM_BY_IDX[idx]

        def work():
            rsp = self.link.write_param(idx, raw)
            if rsp is None:
                self.bridge.write_fail.emit(idx, "无应答")
            elif rsp["status"] != P.PARAM_ST_OK:
                self.bridge.write_fail.emit(idx, P.STATUS_NAME.get(rsp["status"], "?"))
            else:
                self.bridge.write_ok.emit(idx, raw)
                self._log_t("写 0x%02X %-8s = %-10d 已进暂存区（RAM，断电即丢）——锁存生效后才写入 Flash"
                            % (idx, p.name, raw), "tx", "param")

        threading.Thread(target=work, daemon=True).start()

    def _on_write_ok(self, idx, raw):
        old = self.pending.get(idx, (None, None))[1]
        if old is None:
            old = self.values.get(idx)
        self.pending[idx] = (raw, old)
        f = self.fields.get(idx)
        if f is not None:
            f.set_pending(True, raw)
        self._refresh_pending_list()
        self._log("待锁存 %d 项，点「锁存生效」下发控制字 bit3" % len(self.pending), "info", "param")

    def _on_write_fail(self, idx, why):
        self._log("写 0x%02X %s 失败：%s（已回显生效值）"
                  % (idx, P.PARAM_BY_IDX[idx].name, why), "err", "param")
        f = self.fields.get(idx)
        if f is not None:
            f.refresh()

    def _refresh_pending_list(self):
        n = len(self.pending)
        self.pend_pill.setText("待锁存 %d 项" % n)
        self.pend_pill.setVisible(n > 0)
        self.btn_latch.setText("锁存生效 (%d)" % n if n else "锁存生效")
        self.btn_latch.setEnabled(self._connected())

        for w in self._pend_rows:
            w.deleteLater()
        self._pend_rows = []

        if not self.pending:
            empty = caption("当前没有待锁存的修改")
            self.pend_lay.addWidget(empty)
            self._pend_rows.append(empty)
            return

        for idx in sorted(self.pending):
            new_raw, old_raw = self.pending[idx]
            p = P.PARAM_BY_IDX[idx]
            wrap = QWidget()
            h = QHBoxLayout(wrap)
            h.setContentsMargins(0, 0, 0, 0)
            h.setSpacing(8)
            name = QLabel("0x%02X %s" % (idx, p.name))
            name.setFont(QFont(F.FONT_MONO, 9))
            h.addWidget(name)
            old_s = p.phys_text(old_raw) if old_raw is not None else "?"
            h.addWidget(caption("%s  →  %s (%d)" % (old_s, p.phys_text(new_raw), new_raw)))
            h.addStretch(1)
            if old_raw is not None:
                b = F.PushButton("撤销")
                b.setFixedHeight(28)
                b.clicked.connect(lambda _=False, i=idx: self._undo_one(i))
                h.addWidget(b)
            self.pend_lay.addWidget(wrap)
            self._pend_rows.append(wrap)

    def _undo_one(self, idx):
        """撤销 = 把旧值重新写回暂存区（设备暂存无法按项擦除，用覆盖达到同样效果）。"""
        if idx not in self.pending:
            return
        _, old_raw = self.pending.pop(idx)
        f = self.fields.get(idx)
        if f is not None:
            f.set_pending(False)
            f.refresh()
        self._refresh_pending_list()
        if old_raw is None:
            self._log("0x%02X 没有记录旧值，请手动改回或直接锁存" % idx, "err", "param")
            return

        def work():
            rsp = self.link.write_param(idx, old_raw)
            if rsp is None or rsp["status"] != P.PARAM_ST_OK:
                self._log_t("撤销 0x%02X 失败" % idx, "err", "param")
            else:
                self._log_t("0x%02X 已撤销（旧值 %s 重新写入暂存区）" % (idx, old_raw),
                            "tx", "param")

        threading.Thread(target=work, daemon=True).start()

    def _undo_all(self):
        for idx in list(self.pending):
            self._undo_one(idx)

    def _latch(self):
        if not self._connected() or self.ctrl is None:
            QMessageBox.warning(self, "未连接", "请先连接串口。")
            return
        if not self.pending:
            self._log("没有待锁存参数，仍发送一次 bit3 脉冲（幂等）。", "info")
        self.ctrl.pulse(P.CW_LATCH, 80)
        self._log("TX 控制字 bit3 上升沿 → 参数锁存生效", "tx", "param")
        self._log("掉电保存：驱动器会在输出关闭且稳定约 2 s 后写入 Flash，期间断电可能丢失",
                  "info", "param")
        # 锁存后驱动器可能在输出关闭时写 Flash，主循环停摆 20~40 ms，稍等再回读
        QTimer.singleShot(350, self._after_latch)

    def _after_latch(self):
        self.pending.clear()
        for f in self.fields.values():
            f.set_pending(False)
        self._refresh_pending_list()
        self._log("锁存完成，回读确认生效值…", "info", "param")
        self._read_all()

    # ============================================================ 参数文件
    def _export(self):
        path, _ = QFileDialog.getSaveFileName(self, "导出参数", "motor_params.json", "JSON (*.json)")
        if not path:
            return
        data = {
            "tool": "Motor_Prj ParamHost (Qt6)",
            "saved_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "device_addr": int(self.sp_addr.value()),
            "values": dict((("0x%02X" % k), v) for k, v in sorted(self.values.items())),
        }
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        self._log("已导出 %d 项参数到 %s" % (len(self.values), os.path.basename(path)),
                  "ok", "param")

    def _import(self):
        path, _ = QFileDialog.getOpenFileName(self, "导入参数", "", "JSON (*.json)")
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception as exc:
            QMessageBox.critical(self, "读取失败", str(exc))
            return
        vals = data.get("values", {})
        if not vals:
            QMessageBox.critical(self, "文件无效", "未找到 values 字段。")
            return
        if not self._connected():
            QMessageBox.warning(self, "未连接", "导入后需要写入驱动器，请先连接串口。")
            return
        if QMessageBox.question(self, "确认导入",
                                "将把 %d 项参数依次写入暂存区，完成后请点「锁存生效」。\n\n继续？"
                                % len(vals), QMessageBox.Yes | QMessageBox.No,
                                QMessageBox.No) != QMessageBox.Yes:
            return

        def work():
            n = 0
            for k, v in sorted(vals.items()):
                idx = int(k, 0)
                p = P.PARAM_BY_IDX.get(idx)
                if p is None or p.ro or not p.in_range(v):
                    self._log_t("跳过 0x%02X=%s（只读或超范围）" % (idx, v), "err", "param")
                    continue
                if not self._connected():
                    break
                rsp = self.link.write_param(idx, int(v))
                if rsp is None or rsp["status"] != P.PARAM_ST_OK:
                    self._log_t("写 0x%02X 失败" % idx, "err", "param")
                    continue
                n += 1
                self.bridge.write_ok.emit(idx, int(v))
            self._log_t("导入完成：%d 项已进暂存区，请点「锁存生效」" % n,
                        "ok" if n else "err", "param")

        threading.Thread(target=work, daemon=True).start()

    def _factory_reset(self):
        if not self._connected():
            QMessageBox.warning(self, "未连接", "请先连接串口。")
            return
        if QMessageBox.question(self, "恢复出厂默认",
                                "将把全部读写参数恢复为固件出厂默认值并写入暂存区。\n\n继续？",
                                QMessageBox.Yes | QMessageBox.No,
                                QMessageBox.No) != QMessageBox.Yes:
            return

        def work():
            n = 0
            for idx, val in sorted(FACTORY_DEFAULTS.items()):
                rsp = self.link.write_param(idx, val)
                if rsp is None or rsp["status"] != P.PARAM_ST_OK:
                    self._log_t("恢复 0x%02X 失败" % idx, "err")
                    continue
                self.bridge.write_ok.emit(idx, val)
                n += 1
            self._log_t("出厂默认已写入暂存区 %d 项，请点「锁存生效」" % n, "ok", "param")

        threading.Thread(target=work, daemon=True).start()

    # ============================================================ 输出控制
    def _style_mode(self, active):
        for mode, (c, t) in self.mode_cards.items():
            on = (mode == active)
            c.setStyleSheet("#ModeCard{background:%s;border:%s;border-radius:8px;}" % (
                "#EBF3FB" if on else "#FFFFFF",
                "2px solid #0067C0" if on else "1px solid #E1E1E1"))
            t.setStyleSheet("color:%s;" % (F.ACCENT if on else F.TEXT))

    def _pick_mode(self, mode):
        self.cur_mode = mode
        self._style_mode(mode)
        if self.ctrl is not None:
            self.ctrl.set_mode(mode)
        self._log("控制模式 = %d（%s）" % (mode, "电压开环" if mode == P.MODE_VOLTAGE else "电流闭环"),
                  "tx", "ctrl")
        self._update_frame_preview()

    def _ref_pct(self, attr):
        return self.ref_sliders[attr].value() / P.CTRL_REF_PER_PCT

    def _slider_moved(self, attr, val):
        pct = val / P.CTRL_REF_PER_PCT
        self.ref_values[attr].setText("%+.2f %%" % pct)
        self._syncing = True
        try:
            self.ref_raws[attr].setText(str(val))
        finally:
            self._syncing = False
        if self.ctrl is not None:
            self.ctrl.set_refs(self._ref_pct("ref1"), self._ref_pct("ref2"))
        self._update_frame_preview()

    def _raw_edited(self, attr):
        if self._syncing:
            return
        sl = self.ref_sliders[attr]
        try:
            val = max(-P.CTRL_REF_MAX, min(P.CTRL_REF_MAX, int(self.ref_raws[attr].text(), 0)))
        except Exception:
            self.ref_raws[attr].setText(str(sl.value()))
            return
        sl.setValue(val)                       # 触发 _slider_moved 完成同步
        self._slider_moved(attr, val)

    def _update_frame_preview(self):
        addr = int(self.sp_addr.value())
        ctrl = P.CW_ENABLE if (self.ctrl is not None and self.ctrl.state()["enable"]) else 0
        frame = P.pack_ctrl_down(addr, ctrl, self.cur_mode,
                                 self.ref_sliders["ref1"].value(),
                                 self.ref_sliders["ref2"].value())
        self.lb_frame.setText("下行帧预览：" +
                              " ".join("%02X" % b for b in frame).upper() +
                              "   （常驻发送，维持链路）")

    def _on_enable(self, checked):
        if self.ctrl is None:
            self.sw_enable.setChecked(False)
            return
        if checked:
            st = self.link.stats.last_ctrl if self.link else None
            if st is not None and (st["status"] & P.ST_FAULT_LATCH):
                self.sw_enable.setChecked(False)
                self.ctrl.set_enable(False)
                QMessageBox.warning(self, "故障锁存中",
                                    "状态位 bit7（故障锁存）为 1，驱动器不接受使能。\n"
                                    "请先点击「清故障」下发控制字 bit1。")
                return
        self.ctrl.set_enable(bool(checked))
        self.lb_enable.setText("输出已使能" if checked else "输出关闭")
        self.lb_enable.setStyleSheet("color:%s;" % (F.OK_GREEN if checked else F.TEXT_3))
        self._log("控制字 bit0 = %d（%s）" % (1 if checked else 0,
                                             "使能输出" if checked else "关闭输出"), "tx", "ctrl")
        self._update_frame_preview()

    def _clear_fault(self):
        if not self._connected() or self.ctrl is None:
            QMessageBox.warning(self, "未连接", "请先连接串口。")
            return
        self.ctrl.pulse(P.CW_CLR_FAULT, 80)
        self._log("TX 控制字 bit1 上升沿 → 清除故障锁存", "tx", "ctrl")

    def _zero(self):
        if not self._connected() or self.ctrl is None:
            QMessageBox.warning(self, "未连接", "请先连接串口。")
            return
        try:
            uq = float(self.ed_zero.text().split("%")[0].strip())
        except Exception:
            uq = 0.0
        if QMessageBox.question(
                self, "自动找零",
                "将关闭使能，以找零电压 %+.2f%%（原始值 %d）触发 bit2，约 5 秒后自动结束。\n\n继续？"
                % (uq, P.pct_to_ref(uq)), QMessageBox.Yes | QMessageBox.No,
                QMessageBox.No) != QMessageBox.Yes:
            return
        self.sw_enable.setChecked(False)
        self.ctrl.set_enable(False)
        self.ctrl.set_refs(uq, 0.0)
        self.ctrl.pulse(P.CW_ZERO, 120)
        self._log("TX 控制字 bit2 上升沿 → 自动找零，找零电压 %d，约 5 s 后自动结束"
                  % P.pct_to_ref(uq), "tx", "ctrl")

    def _stop_output(self):
        if self.ctrl is None:
            return
        self.sw_enable.setChecked(False)
        self.ctrl.set_enable(False)
        self._log("已停止输出（bit0 = 0）", "tx", "ctrl")
        self._update_frame_preview()

    def _reset_stats(self):
        if self.link is not None:
            self.link.stats = L.Stats()
            self._log("通信统计已重置", "info", "ctrl")

    def _send_raw(self):
        if not self._connected():
            QMessageBox.warning(self, "未连接", "请先连接串口。")
            return
        s = self.ed_raw.text().replace(" ", "").replace(",", "").replace("0x", "")
        try:
            data = bytes.fromhex(s)
        except Exception:
            QMessageBox.critical(self, "格式错误", "请输入十六进制字节。")
            return
        if data:
            self.link.write(data)
            self._log("TX(原始) " + " ".join("%02X" % b for b in data), "tx", "ctrl")

    # ============================================================ 周期刷新
    def _tick(self):
        if self.link is None or not self.link.is_open:
            if not self.offline and self.link is not None:
                self._disconnect()
            return
        st = self.link.stats

        # 上行帧看门狗：驱动器断电后串口仍在线，但 1 kHz 上行帧会停止
        if st.ctrl_rx > 0:
            silent = time.time() - st.last_ctrl_t
            if not self.offline and silent > 2.0:
                self._enter_offline()
            elif self.offline and st.alive():
                self._exit_offline()
        if self.offline or not self._connected():
            return

        f = st.last_ctrl
        now = time.time()
        mon_visible = self.page_monitor.isVisible()
        ctl_visible = self.page_control.isVisible()

        if f is not None:
            ang_deg = f.get("angle_deg_map" if self.angle_use_cal else "angle_deg_full", f.get("angle_deg_cal", f["angle_deg"]))
            iq_a = f["iq"] * P.CTRL_IQ_LSB
            if mon_visible:
                self.metrics["angle"].setText("%.2f" % ang_deg)
                self.metrics["iq"].setText("%.2f" % iq_a)
                self.metrics["vbus"].setText("%.1f" % (f["vbus"] * P.CTRL_VBUS_LSB))

            # 转速估算：约 0.5 s 角度差分（处理 0/360 回绕）
            self._hist_t.append(now)
            self._hist_ang.append(ang_deg)
            while self._hist_t and self._hist_t[0] < now - 0.6:
                self._hist_t.popleft()
                self._hist_ang.popleft()
            rpm = None
            if len(self._hist_t) >= 2 and now - self._hist_t[0] > 0.2:
                d = ang_deg - self._hist_ang[0]
                if d > 180:
                    d -= 360
                elif d < -180:
                    d += 360
                rpm = d / (now - self._hist_t[0]) / 360.0 * 60.0
            self.metrics["rpm"].setText("—" if rpm is None else "%.0f" % rpm)

            # 状态位胶囊只随控制页可见时刷新
            if ctl_visible:
                for bit, _short, _desc in P.STATUS_BITS:
                    on = bool(f["status"] & bit)
                    lb = self.chips[bit]
                    if not on:
                        lb.setStyleSheet("background:#EDEDED;color:#666666;border-radius:4px;"
                                         "padding:4px 9px;font-size:8pt;")
                    elif bit == P.ST_ENABLED:
                        lb.setStyleSheet("background:%s;color:#FFF;border-radius:4px;"
                                         "padding:4px 9px;font-size:8pt;font-weight:600;" % F.OK_GREEN)
                    elif bit in (P.ST_COMM_LOST, P.ST_FAULT_LATCH):
                        lb.setStyleSheet("background:%s;color:#FFF;border-radius:4px;"
                                         "padding:4px 9px;font-size:8pt;font-weight:600;" % F.ERR_RED)
                    else:
                        lb.setStyleSheet("background:%s;color:#FFF;border-radius:4px;"
                                         "padding:4px 9px;font-size:8pt;font-weight:600;" % F.WARN_AMBER)

            faults = [d for bit, _s, d in P.STATUS_BITS
                      if bit & (P.ST_ANGLE_ERR | P.ST_OVER_CUR | P.ST_OVER_VOLT |
                                P.ST_UNDER_VOLT | P.ST_OVER_TEMP) and (f["status"] & bit)]
            if mon_visible:
                if f["status"] & P.ST_FAULT_LATCH:
                    self.lb_state.setText("故障锁存：" + ("、".join(faults) or "未知故障"))
                    self.lb_state.setStyleSheet("color:%s;" % F.ERR_RED)
                elif f["status"] & P.ST_COMM_LOST:
                    self.lb_state.setText("通讯超时，输出已关闭")
                    self.lb_state.setStyleSheet("color:%s;" % F.ERR_RED)
                elif f["status"] & P.ST_ENABLED:
                    self.lb_state.setText("正常运行")
                    self.lb_state.setStyleSheet("color:%s;" % F.OK_GREEN)
                else:
                    self.lb_state.setText("待机（输出关闭）")
                    self.lb_state.setStyleSheet("color:%s;" % F.TEXT_2)

                # 圆点只改文字与配色，控件不销毁重建（旧实现每 50 ms 重建 3 个 QLabel）
                self.dot_en.setText("● 使能" if f["status"] & P.ST_ENABLED else "● 停止")
                self.dot_en.setStyleSheet("color:%s;font-size:9pt;font-weight:600;"
                                          % (F.OK_GREEN if f["status"] & P.ST_ENABLED else "#8A8A8A"))
                self.dot_fault.setText("● 无故障" if not faults else "● " + "、".join(faults))
                self.dot_fault.setStyleSheet("color:%s;font-size:9pt;font-weight:600;"
                                             % (F.OK_GREEN if not faults else F.ERR_RED))
                self.dot_link.setText("● 链路正常" if st.alive() else "● 链路超时")
                self.dot_link.setStyleSheet("color:%s;font-size:9pt;font-weight:600;"
                                            % (F.OK_GREEN if st.alive() else F.ERR_RED))

                self.lb_leg_ang.setText("▬ 电角度 %.2f°" % ang_deg)
                self.lb_leg_iq.setText("▬ Iq %.2f A" % iq_a)
                if len(self.chart.t) >= 2:
                    self.lb_hist.setText("历史 %s" % self.chart._span_text(self.chart.t[-1]))
                self.btn_live.setEnabled(not self.chart.live)
                if st.last_raw is not None:
                    self.lb_frame_info.setText(
                        "上行 %d fps ｜ 末帧 %s"
                        % (st.fps(), " ".join("%02X" % b for b in st.last_raw)))

            # 波形数据始终入队（隐藏页只入队不重绘）
            self.chart.feed(now, ang_deg / 360.0, (iq_a / 20.0) + 0.5)

            if ctl_visible:
                self.diag["上行帧率"].setText("%.0f fps" % st.fps())
                self.diag["链龄"].setText("%.0f ms" % ((now - st.last_ctrl_t) * 1000.0))
                self.diag["累计上行"].setText("%d 帧" % st.ctrl_rx)
                self.diag["参数 TX/RX"].setText("%d / %d" % (st.tx_param, st.param_rx))
                self.diag["超时重发"].setText("%d 次" % st.retries)
                self.diag["错帧"].setText("%d" % (st.ctrl_err + st.param_err))
                self.diag["站号"].setText(str(self.link.addr))
            self.lb_stat.setText("上行 %d 帧 ｜ 参数 TX %d / RX %d ｜ 重发 %d ｜ 错帧 %d"
                                 % (st.ctrl_rx, st.tx_param, st.param_rx, st.retries,
                                    st.ctrl_err + st.param_err))

    # ============================================================ 退出
    def closeEvent(self, event):
        try:
            if self.ctrl is not None:
                self.ctrl.set_enable(False)
                time.sleep(0.05)
                self.ctrl.stop()
            if self.link is not None:
                self.link.close()
        except Exception:
            pass
        super().closeEvent(event)
