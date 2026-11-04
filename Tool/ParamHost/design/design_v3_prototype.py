# -*- coding: utf-8 -*-
"""
界面重构方案 v3.2 渲染原型（三页：参数配置 / 实时监视 / 控制·诊断）

相对 v3.1：
  · 参数页改紧凑密度：每字段 物理值+原始值 双输入，卡片高度贴内容；
    新增「待锁存清单」（逐项撤销）与「参数文件」（导出/导入）卡片补足下方空间
  · 控制页充实：设定值卡加下行帧实时预览，操作卡加找零电压输入，
    诊断区新增控制字速查与状态位全量卡，通信统计卡加重置按钮
"""
import math
import sys

sys.path.insert(0, r"E:\Data_Project\Offical_Prj\Motor_Prj\Tool\ParamHost")

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QFrame, QGridLayout, QHBoxLayout, QLabel,
    QLineEdit, QPushButton, QScrollArea, QSlider, QVBoxLayout, QWidget,
)

import fluent as F

try:
    from qfluentwidgets import FluentWindow, FluentIcon as FIF, Slider
    HAS_FW = True
except Exception:
    HAS_FW = False

app = QApplication(sys.argv)
F.setTheme(F.Theme.LIGHT)
F.setThemeColor(F.ACCENT)
app.setStyleSheet(F.style_sheet())
app.setFont(QFont(F.FONT_UI, 9))

EDIT_QSS = """
QLineEdit {
    background:#FFFFFF; border:1px solid #C8C8C8; border-bottom:2px solid #C8C8C8;
    border-radius:4px; padding:2px 7px; min-height:22px; font-family:'Consolas'; font-size:9pt;
}
QLineEdit:focus { border-bottom:2px solid #0067C0; }
QLineEdit[modified="true"] { border-bottom:2px solid #E9A700; background:#FFFDF5; }
QLineEdit[ro="true"] { background:#F7F7F7; color:#9A9A9A; border-bottom:1px solid #DCDCDC; }
"""


def key(t):
    lb = QLabel(t)
    lb.setObjectName("MetricKey")
    return lb


def line_edit(text, ro=False, modified=False, w=120):
    e = QLineEdit(text)
    e.setReadOnly(ro)
    e.setFixedWidth(w)
    e.setStyleSheet(EDIT_QSS)
    if ro:
        e.setProperty("ro", "true")
    if modified:
        e.setProperty("modified", "true")
    return e


def pill(text):
    lb = QLabel(text)
    lb.setStyleSheet("background:#FFF1D6;color:#B26B00;border-radius:9px;"
                     "padding:2px 8px;font-size:8pt;font-weight:600;")
    lb.setFixedHeight(20)
    return lb


def small_btn(text):
    b = F.PushButton(text)
    b.setFixedHeight(24)
    return b


# ================================================================ 字段行
def field(label, phys, raw, note="", ro=False, modified=False):
    """参数页紧凑字段行：名称 | 物理值 | 原始值 | 注记 | 待锁存徽标"""
    h = QHBoxLayout()
    h.setSpacing(8)
    k = key(label)
    k.setFixedWidth(72)
    h.addWidget(k)
    h.addWidget(line_edit(phys, ro=ro, modified=modified, w=104))
    h.addWidget(line_edit(raw, ro=ro, w=96))
    note_lb = QLabel(note)
    note_lb.setObjectName("Caption")
    note_lb.setFont(QFont(F.FONT_MONO, 8))
    h.addWidget(note_lb, 1)
    if modified:
        h.addWidget(pill("待锁存"))
    return h


def group(title, desc, rows):
    card, lay = F.card()
    t = QLabel(title)
    t.setObjectName("SectionTitle")
    lay.addWidget(t)
    if desc:
        lay.addWidget(F.CaptionLabel(desc))
        lay.addSpacing(2)
    for r in rows:
        lay.addLayout(r)
    return card


# ================================================================ 实时曲线
class StripChart(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(240)

    def paintEvent(self, ev):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        p.fillRect(self.rect(), QColor("#FDFDFD"))
        p.setPen(QPen(QColor("#ECECEC"), 1))
        for i in range(1, 6):
            p.drawLine(QPointF(34, h * i / 6.0), QPointF(w - 8, h * i / 6.0))
        for i in range(1, 10):
            x = 34 + (w - 42) * i / 10.0
            p.drawLine(QPointF(x, 6), QPointF(x, h - 18))
        n = 220
        pa, pi = [], []
        for i in range(n):
            x = 34 + (w - 42) * i / (n - 1.0)
            t = i / (n - 1.0)
            pa.append(QPointF(x, 6 + (h - 24) * (1 - (t * 2.2) % 1.0)))
            pi.append(QPointF(x, 6 + (h - 24) * (1 - (0.5 + 0.38 * math.sin(t * math.pi * 6)))))
        for pts, color in ((pa, "#0067C0"), (pi, "#E9A700")):
            path = QPainterPath(pts[0])
            for pt in pts[1:]:
                path.lineTo(pt)
            p.setPen(QPen(QColor(color), 2))
            p.drawPath(path)
        p.setPen(QColor("#9A9A9A"))
        p.setFont(QFont(F.FONT_MONO, 7))
        p.drawText(QRectF(0, 0, 30, 14), Qt.AlignRight | Qt.AlignVCenter, "360°")
        p.drawText(QRectF(0, h - 30, 30, 14), Qt.AlignRight | Qt.AlignVCenter, "0°")
        p.drawText(QRectF(w - 120, h - 16, 110, 14), Qt.AlignRight, "时间窗 10 s →")
        p.end()


def metric_card(caption, value, unit, color=F.TEXT):
    card, lay = F.card()
    lay.addWidget(F.CaptionLabel(caption))
    row = QHBoxLayout()
    row.setSpacing(6)
    big = QLabel(value)
    big.setFont(QFont(F.FONT_MONO, 22, QFont.DemiBold))
    big.setStyleSheet("color:%s;" % color)
    row.addWidget(big)
    u = QLabel(unit)
    u.setStyleSheet("color:%s;font-size:11pt;" % F.TEXT_2)
    u.setAlignment(Qt.AlignBottom)
    row.addWidget(u)
    row.addStretch(1)
    lay.addLayout(row)
    return card


# ================================================================ 页 1：参数配置
par = QWidget()
par.setObjectName("params")
po = QVBoxLayout(par)
po.setContentsMargins(20, 14, 20, 14)
po.setSpacing(10)

bar = QHBoxLayout()
bar.setSpacing(8)
bar.addWidget(pill("待锁存 2 项"))
bar.addWidget(F.CaptionLabel("修改即写入暂存区，「锁存生效」后参与运算并自动回读"))
bar.addStretch(1)
bar.addWidget(small_btn("重新读取"))
bar.addWidget(small_btn("全部撤销"))
b_latch = F.PrimaryPushButton("锁存生效 (2)")
b_latch.setFixedHeight(28)
bar.addWidget(b_latch)
po.addLayout(bar)

body = QWidget()
grid = QGridLayout(body)
grid.setContentsMargins(0, 0, 0, 0)
grid.setHorizontalSpacing(10)
grid.setVerticalSpacing(10)

grid.addWidget(group("电机与编码器", "极对数 / 方向 / 零位须在输出关闭时写入；改零位后需重新自动找零", [
    field("极对数", "4", "4", "整数 1 ~ 100"),
    field("电机方向", "1", "1", "0 反转 · 1 正转"),
    field("编码器方向", "1", "1", "0 反转 · 1 正转"),
    field("电机零位", "36.0000", "1179648", "1/2^15 度 · 改后重新找零"),
]), 0, 0)

grid.addWidget(group("电流环 PI", "物理值 = 写入值 ÷ 16，范围 1.0 ~ 200000.0", [
    field("Kp / Q 轴", "1.5000", "24", "", modified=True),
    field("Ki / Q 轴", "2.0000", "32", ""),
    field("Kp / D 轴", "1.0000", "16", ""),
    field("Ki / D 轴", "2.0000", "32", ""),
]), 0, 1)

grid.addWidget(group("电压与限幅", "随母线电压确定：增益 ≈ 5408/U · 限幅 ≈ U/1.732", [
    field("电压增益", "193", "193", "母线 28 V → 193"),
    field("电压限幅", "18", "18", "母线 28 V → 16", modified=True),
]), 1, 0)

grid.addWidget(group("通信与版本", "超时门限同时用于下行链路保护", [
    field("通讯超时", "200", "200", "10 ~ 1000 ms"),
    field("固件版本", "1.0", "65536", "高 16 位主 · 低 16 位次", ro=True),
    field("协议版本", "1", "1", "固定为 1", ro=True),
]), 1, 1)

pend_card, pl = F.card("待锁存清单（锁存前不参与运算，读参数仍返回旧值）")
for name, old, new in (("0x04  Q 轴电流比例", "1.0000 (16)", "1.5000 (24)"),
                       ("0x0A  电压限幅", "16", "18")):
    h = QHBoxLayout()
    h.setSpacing(8)
    lb = QLabel(name)
    lb.setFont(QFont(F.FONT_MONO, 9))
    h.addWidget(lb)
    h.addWidget(F.CaptionLabel("%s  →  %s" % (old, new)))
    h.addStretch(1)
    h.addWidget(small_btn("撤销"))
    pl.addLayout(h)
grid.addWidget(pend_card, 2, 0)

file_card, fl = F.card("参数文件")
fh = QHBoxLayout()
fh.setSpacing(8)
fh.addWidget(small_btn("导出 JSON"))
fh.addWidget(small_btn("导入 JSON"))
fh.addWidget(small_btn("恢复出厂默认"))
fh.addStretch(1)
fh.addWidget(F.CaptionLabel("上次导出 2026-11-04 14:02 · motor_params.json"))
fl.addLayout(fh)
grid.addWidget(file_card, 2, 1)

grid.setRowStretch(3, 1)
sc = QScrollArea()
sc.setWidgetResizable(True)
sc.setWidget(body)
sc.setFrameShape(QFrame.NoFrame)
po.addWidget(sc, 1)

# ================================================================ 页 2：实时监视
mon = QWidget()
mon.setObjectName("monitor")
mv = QVBoxLayout(mon)
mv.setContentsMargins(20, 14, 20, 14)
mv.setSpacing(10)

g = QHBoxLayout()
g.setSpacing(10)
g.addWidget(metric_card("电角度", "123.45", "°"))
g.addWidget(metric_card("Iq 电流", "0.36", "A"))
g.addWidget(metric_card("母线电压", "28.5", "V"))
g.addWidget(metric_card("转速估算", "1028", "rpm"))

card, lay = F.card()
lay.addWidget(F.CaptionLabel("系统状态"))
big = QLabel("正常运行")
big.setFont(QFont(F.FONT_UI, 18, QFont.DemiBold))
big.setStyleSheet("color:%s;" % F.OK_GREEN)
lay.addWidget(big)
dots = QHBoxLayout()
dots.setSpacing(10)
for t in ("使能输出", "无故障", "链路正常", "参数已生效"):
    d = QLabel("● " + t)
    d.setStyleSheet("color:%s;font-size:9pt;font-weight:600;" % F.OK_GREEN)
    dots.addWidget(d)
dots.addStretch(1)
lay.addLayout(dots)
lay.addWidget(F.CaptionLabel("帧率 1000 fps · 链龄 1 ms · 参数往返 4.2 ms"))
g.addWidget(card, 1)
mv.addLayout(g)

chart_card, cl = F.card("实时波形（10 s 滚动窗口）")
legend = QHBoxLayout()
legend.setSpacing(16)
for text, color in (("电角度 123.45°", "#0067C0"), ("Iq 0.36 A", "#E9A700")):
    d = QLabel("▬ " + text)
    d.setStyleSheet("color:%s;font-size:9pt;font-weight:600;" % color)
    legend.addWidget(d)
legend.addStretch(1)
legend.addWidget(QCheckBox("冻结画面"))
cl.addLayout(legend)
cl.addWidget(StripChart(), 1)
mv.addWidget(chart_card, 1)

# ================================================================ 页 3：控制 · 诊断
ctl = QWidget()
ctl.setObjectName("control")
cs = QScrollArea()
cs.setWidgetResizable(True)
cs.setFrameShape(QFrame.NoFrame)
inner = QWidget()
cv = QVBoxLayout(inner)
cv.setContentsMargins(20, 14, 20, 14)
cv.setSpacing(10)
ctlv = QVBoxLayout(ctl)
ctlv.setContentsMargins(0, 0, 0, 0)
ctlv.addWidget(cs)
cs.setWidget(inner)

enc, el = F.card()
er = QHBoxLayout()
er.setSpacing(14)
big_sw = F.switch()
big_sw.setChecked(True)
big_sw.setFixedHeight(36)
er.addWidget(big_sw)
tv = QVBoxLayout()
t1 = QLabel("输出使能")
t1.setFont(QFont(F.FONT_UI, 14, QFont.DemiBold))
t2 = F.CaptionLabel("bit0 电平有效 · 断开连接 200 ms 后自动关闭 · 故障锁存期间拒绝使能")
tv.addWidget(t1)
tv.addWidget(t2)
er.addLayout(tv)
er.addStretch(1)
sten = QLabel("输出已使能")
sten.setFont(QFont(F.FONT_UI, 13, QFont.DemiBold))
sten.setStyleSheet("color:%s;" % F.OK_GREEN)
er.addWidget(sten)
el.addLayout(er)
cv.addWidget(enc)

mc, ml = F.card("控制模式")
mr = QHBoxLayout()
mr.setSpacing(10)


def mode_card(title, sub, active):
    c = QFrame()
    c.setObjectName("ModeCard")
    c.setFixedHeight(58)
    c.setStyleSheet("#ModeCard{background:%s;border:%s;border-radius:8px;}" % (
        "#EBF3FB" if active else "#FFFFFF",
        "2px solid #0067C0" if active else "1px solid #E1E1E1"))
    lay = QVBoxLayout(c)
    lay.setContentsMargins(14, 7, 14, 7)
    t = QLabel(title)
    t.setFont(QFont(F.FONT_UI, 11, QFont.DemiBold))
    t.setStyleSheet("color:%s;" % (F.ACCENT if active else F.TEXT))
    s = QLabel(sub)
    s.setStyleSheet("color:%s;font-size:8pt;" % F.TEXT_3)
    lay.addWidget(t)
    lay.addWidget(s)
    return c


mr.addWidget(mode_card("电压开环", "值1 = Uq ｜ 值2 = Ud", True))
mr.addWidget(mode_card("电流闭环", "值1 = Iq ｜ 值2 = Id", False))
ml.addLayout(mr)
cv.addWidget(mc)

rc, rl = F.card("设定值（-100 % ~ +100 % ↔ ±32767，四舍五入取整）")


def big_slider(title, val, color):
    h = QHBoxLayout()
    h.setSpacing(12)
    k = key(title)
    k.setFixedWidth(52)
    sl = Slider(Qt.Horizontal) if HAS_FW else QSlider(Qt.Horizontal)
    sl.setRange(-32767, 32767)
    sl.setValue(int(val * 327.67))
    v = QLabel("%+.2f %%" % val)
    v.setFont(QFont(F.FONT_MONO, 13, QFont.DemiBold))
    v.setStyleSheet("color:%s;" % color)
    v.setFixedWidth(96)
    v.setAlignment(Qt.AlignCenter)
    raw = line_edit(str(int(val * 327.67)), w=76)
    h.addWidget(k)
    h.addWidget(sl, 1)
    h.addWidget(v)
    h.addWidget(raw)
    return h


rl.addLayout(big_slider("设定值1", 10.0, F.ACCENT))
rl.addLayout(big_slider("设定值2", 0.0, F.TEXT_2))
frame_lb = QLabel("下行帧预览：AA 55 01 01 0C CD 00 00 00 DA   （500 Hz 常驻发送，维持链路）")
frame_lb.setFont(QFont(F.FONT_MONO, 8))
frame_lb.setStyleSheet("color:%s;" % F.TEXT_3)
rl.addWidget(frame_lb)
cv.addWidget(rc)

oc, ol = F.card("操作")
orow = QHBoxLayout()
orow.setSpacing(8)
orow.addWidget(F.PushButton("清故障 (bit1)"))
orow.addWidget(key("找零电压"))
ze = line_edit("10.0 %", w=90)
orow.addWidget(ze)
orow.addWidget(F.PushButton("自动找零 (bit2)"))
orow.addStretch(1)
b_stop = F.PrimaryPushButton("停止输出")
b_stop.setFixedHeight(28)
orow.addWidget(b_stop)
ol.addLayout(orow)
cv.addWidget(oc)

st_card, st_lay = F.card("驱动器状态位（激活亮色，未激活置灰）")
srow = QHBoxLayout()
srow.setSpacing(6)
for t in ("0x01 ANG 角度异常", "0x02 OC 过流", "0x04 OV 过压", "0x08 UV 欠压",
          "0x10 OT 过温", "0x40 LOST 超时", "0x80 LATCH 故障锁存"):
    lb = QLabel(t)
    lb.setStyleSheet("background:#EDEDED;color:#8C8C8C;border-radius:4px;padding:4px 9px;font-size:8pt;")
    srow.addWidget(lb)
lb = QLabel("0x20 EN 使能中")
lb.setStyleSheet("background:%s;color:#FFF;border-radius:4px;padding:4px 9px;font-size:8pt;font-weight:600;"
                 % F.OK_GREEN)
srow.addWidget(lb)
srow.addStretch(1)
st_lay.addLayout(srow)
cw = QLabel("控制字：bit0 使能(电平) · bit1 清故障(上升沿) · bit2 自动找零(上升沿, bit0 须为 0) · "
            "bit3 参数锁存(上升沿) · bit4~7 保留")
cw.setObjectName("Caption")
st_lay.addWidget(cw)
cv.addWidget(st_card)

dg, dgl = F.card("通信统计")
head = QHBoxLayout()
head.addStretch(1)
head.addWidget(small_btn("重置统计"))
dgl.addLayout(head)
gr = QGridLayout()
gr.setHorizontalSpacing(30)
gr.setVerticalSpacing(6)
for i, (k, v) in enumerate([("上行帧率", "1000 fps"), ("参数往返", "4.2 ms"),
                            ("累计上行", "2048 帧"), ("参数 TX/RX", "26 / 14"),
                            ("超时重发", "0 次"), ("错帧", "0"),
                            ("链龄", "1 ms"), ("站号", "1")]):
    r, c = divmod(i, 2)
    gr.addWidget(key(k), r, c * 2)
    lb = QLabel(v)
    lb.setFont(QFont(F.FONT_MONO, 9))
    gr.addWidget(lb, r, c * 2 + 1)
gr.setColumnStretch(1, 1)
gr.setColumnStretch(3, 1)
dgl.addLayout(gr)
cv.addWidget(dg)

raw_card, raw_l = F.card("原始帧调试")
rawh = QHBoxLayout()
rawh.setSpacing(8)
rawe = QLineEdit("5A A5 01 01 09 00 00 00 00 00 00 00 00 00 1D 0F")
rawh.addWidget(rawe, 1)
rawh.addWidget(F.PushButton("发送"))
raw_l.addLayout(rawh)
cv.addWidget(raw_card)
cv.addStretch(1)
cs.setWidget(inner)

# ================================================================ 组装
if not HAS_FW:
    sys.exit("fluent widgets missing")

win = FluentWindow()
win.addSubInterface(par, FIF.SETTING, "参数配置")
win.addSubInterface(mon, FIF.HOME, "实时监视")
win.addSubInterface(ctl, FIF.POWER_BUTTON, "控制 · 诊断")
try:
    ni = win.navigationInterface
    ni.setMinimumExpandWidth(1200)
    ni.setCollapsible(False)
    ni.setExpandWidth(214)
    ni.setFixedWidth(214)
except Exception:
    pass
win.resize(1500, 920)
win.show()

import time as _t

out_dir = r"C:\Users\pc\AppData\Local\Temp\param_doc"
for page, name in ((par, "params"), (mon, "monitor"), (ctl, "control")):
    win.switchTo(page)
    end = _t.perf_counter() + 0.5
    while _t.perf_counter() < end:
        app.processEvents()
        _t.sleep(0.01)
    win.grab().save("%s\\v3_%s.png" % (out_dir, name))
    print("saved", name)
