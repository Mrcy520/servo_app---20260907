# -*- coding: utf-8 -*-
"""
Fluent 控件适配层

优先使用 PySide6-Fluent-Widgets（真正的 Fluent Design 组件：卡片、开关、圆角表格、
Acrylic 背景等）；若未安装，则降级为等价的原生 Qt 控件 + 一套手写 QSS，
保证界面在任何环境下都能启动，只是精致度略有差异。

对外只暴露一组同名控件，UI 代码不需要关心当前走的是哪条路径。
"""

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QCheckBox, QComboBox, QDoubleSpinBox, QFrame, QLabel, QLineEdit, QPushButton,
    QRadioButton, QSpinBox, QTableWidget, QTextBrowser, QVBoxLayout, QHBoxLayout,
)

# ---- Fluent 配色（浅色主题，与 IDE 保持一致的明亮观感）----------------------
ACCENT = "#0067C0"
ACCENT_HOVER = "#1975C5"
ACCENT_PRESS = "#005BA1"
CANVAS = "#F3F3F3"
CARD_BG = "#FFFFFF"
BORDER = "#E5E5E5"
TEXT = "#1A1A1A"
TEXT_2 = "#5D5D5D"
TEXT_3 = "#8C8C8C"
OK_GREEN = "#0F7B0F"
WARN_AMBER = "#C87A00"
ERR_RED = "#C42B1C"
PENDING_BG = "#FFF6E5"
RO_BG = "#FAFAFA"

FONT_UI = "Microsoft YaHei UI"
FONT_MONO = "Consolas"

HAS_FLUENT = False
FLUENT_ERR = ""

try:
    import contextlib
    import io as _io
    # 导入时 qfluentwidgets 会往 stdout 打印一条 Pro 推广横幅，静音掉
    with contextlib.redirect_stdout(_io.StringIO()):
        from qfluentwidgets import (  # noqa: F401
            CardWidget, StrongBodyLabel, BodyLabel, CaptionLabel, SubtitleLabel,
            PushButton, PrimaryPushButton, ComboBox, EditableComboBox, SpinBox,
            DoubleSpinBox, SwitchButton, RadioButton, TableWidget, TextBrowser,
            LineEdit, Theme, setTheme, setThemeColor,
        )
    HAS_FLUENT = True
except Exception as _exc:
    FLUENT_ERR = "%s: %s" % (type(_exc).__name__, _exc)

    # ------------------------------------------------------------------ 降级
    class CardWidget(QFrame):
        def __init__(self, parent=None):
            super().__init__(parent)
            self.setObjectName("Card")

    class _Label(QLabel):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)

    class StrongBodyLabel(_Label):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            f = QFont(FONT_UI, 10)
            f.setBold(True)
            self.setFont(f)

    class BodyLabel(_Label):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            self.setFont(QFont(FONT_UI, 9))

    class CaptionLabel(_Label):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            self.setFont(QFont(FONT_UI, 8))
            self.setStyleSheet("color:%s;" % TEXT_3)

    class SubtitleLabel(_Label):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            f = QFont(FONT_UI, 13)
            f.setBold(True)
            self.setFont(f)

    class PushButton(QPushButton):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            self.setCursor(Qt.PointingHandCursor)

    class PrimaryPushButton(PushButton):
        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            self.setObjectName("Primary")

    class ComboBox(QComboBox):
        pass

    class EditableComboBox(QComboBox):
        def __init__(self, parent=None):
            super().__init__(parent)
            self.setEditable(True)
            self.setInsertPolicy(QComboBox.NoInsert)

        def setPlaceholderText(self, text):
            self.lineEdit().setPlaceholderText(text)

    class SpinBox(QSpinBox):
        pass

    class DoubleSpinBox(QDoubleSpinBox):
        pass

    class SwitchButton(QCheckBox):
        checkedChanged = Signal(bool)

        def __init__(self, text="", parent=None):
            super().__init__(text, parent)
            self.setCursor(Qt.PointingHandCursor)
            self.toggled.connect(self.checkedChanged.emit)

    class RadioButton(QRadioButton):
        pass

    class TableWidget(QTableWidget):
        def setBorderVisible(self, *_a, **_k):
            pass

        def setBorderRadius(self, *_a, **_k):
            pass

        def setWordWrap(self, *_a, **_k):
            pass

    class TextBrowser(QTextBrowser):
        pass

    class LineEdit(QLineEdit):
        pass

    class Theme(object):
        LIGHT = 0
        DARK = 1

    def setTheme(*_a, **_k):
        pass

    def setThemeColor(*_a, **_k):
        pass


# ---- 全局 QSS（降级路径与通用精修都用它，Fluent 库存在时只做少量覆盖）------
STYLE = """
QWidget {
    color: %(text)s;
    font-family: "%(font)s";
}
QWidget#Root {
    background: %(canvas)s;
}
QFrame#Card, QWidget#Card {
    background: %(card)s;
    border: 1px solid %(border)s;
    border-radius: 8px;
}
QLabel#SectionTitle {
    font-size: 10pt;
    font-weight: 600;
    color: %(text)s;
}
QLabel#Caption { color: %(text3)s; font-size: 8pt; }
QLabel#Metric   { font-family: "%(mono)s"; font-size: 10pt; color: %(text)s; }
QLabel#MetricKey { color: %(text2)s; font-size: 9pt; }

QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: #FBFBFB;
    border: 1px solid #DCDCDC;
    border-bottom: 1px solid #C8C8C8;
    border-radius: 4px;
    padding: 4px 8px;
    min-height: 24px;
    selection-background-color: %(accent)s;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border-bottom: 2px solid %(accent)s;
    background: #FFFFFF;
}
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView {
    background: #FFFFFF;
    border: 1px solid %(border)s;
    border-radius: 6px;
    selection-background-color: #EBF3FB;
    selection-color: %(text)s;
    outline: 0;
}

QPushButton {
    background: #FDFDFD;
    border: 1px solid #D6D6D6;
    border-radius: 5px;
    padding: 5px 14px;
    min-height: 24px;
}
QPushButton:hover  { background: #F5F5F5; }
QPushButton:pressed{ background: #EDEDED; }
QPushButton:disabled { color: #A0A0A0; background: #F7F7F7; border-color: #E8E8E8; }
QPushButton#Primary {
    background: %(accent)s;
    color: #FFFFFF;
    border: 1px solid %(accent)s;
    font-weight: 600;
}
QPushButton#Primary:hover   { background: %(accent_hover)s; border-color: %(accent_hover)s; }
QPushButton#Primary:pressed { background: %(accent_press)s; }
QPushButton#Primary:disabled { background: #C7DCF0; border-color: #C7DCF0; color: #FFFFFF; }

QRadioButton { spacing: 6px; }
QCheckBox { spacing: 6px; }

QTableWidget {
    background: %(card)s;
    alternate-background-color: #FAFAFA;
    border: none;
    gridline-color: transparent;
    selection-background-color: #E8F1FB;
    selection-color: %(text)s;
    outline: 0;
}
QHeaderView::section {
    background: transparent;
    border: none;
    border-bottom: 1px solid %(border)s;
    padding: 6px 8px;
    color: %(text2)s;
    font-weight: 600;
}
QTableWidget::item { padding: 4px 8px; }

QTextBrowser {
    background: #FCFCFC;
    border: 1px solid %(border)s;
    border-radius: 6px;
    font-family: "%(mono)s";
    font-size: 9pt;
}
QSplitter::handle { background: transparent; }
QScrollBar:vertical {
    background: transparent; width: 10px; margin: 2px;
}
QScrollBar::handle:vertical {
    background: #C9C9C9; border-radius: 4px; min-height: 28px;
}
QScrollBar::handle:vertical:hover { background: #B0B0B0; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: #C9C9C9; border-radius: 4px; min-width: 28px; }
""" % {
    "text": TEXT, "text2": TEXT_2, "text3": TEXT_3,
    "canvas": CANVAS, "card": CARD_BG, "border": BORDER,
    "accent": ACCENT, "accent_hover": ACCENT_HOVER, "accent_press": ACCENT_PRESS,
    "font": FONT_UI, "mono": FONT_MONO,
}

# Fluent 库存在时只补窗口底色、标题与等宽数值字体，避免全局 QSS 破坏其自绘外观
STYLE_MIN = """
QWidget#Root { background: %(canvas)s; }
QLabel#SectionTitle { font-size: 10pt; font-weight: 600; color: %(text)s; }
QLabel#Caption { color: %(text3)s; font-size: 8pt; }
QLabel#Metric { font-family: "%(mono)s"; font-size: 10pt; color: %(text)s; }
QLabel#MetricKey { color: %(text2)s; font-size: 9pt; }
QTextBrowser {
    font-family: "%(mono)s";
    font-size: 9pt;
    border: 1px solid %(border)s;
    border-radius: 6px;
    background: #FCFCFC;
}
QSplitter::handle { background: transparent; }
""" % {
    "text": TEXT, "text2": TEXT_2, "text3": TEXT_3,
    "canvas": CANVAS, "border": BORDER, "mono": FONT_MONO,
}


def style_sheet():
    """按当前路径返回应挂到 QApplication 上的样式表。"""
    return STYLE_MIN if HAS_FLUENT else STYLE


def card(title=None):
    """建一个 Fluent 卡片，可选左上角标题；返回 (卡片, 内容布局)。"""
    c = CardWidget()
    outer = QVBoxLayout(c)
    outer.setContentsMargins(14, 12, 14, 14)
    outer.setSpacing(10)
    if title:
        lb = QLabel(title)
        lb.setObjectName("SectionTitle")
        outer.addWidget(lb)
    inner = QVBoxLayout()
    inner.setContentsMargins(0, 0, 0, 0)
    inner.setSpacing(10)
    outer.addLayout(inner)
    return c, inner


def row(*widgets, spacing=10, stretch_last=False):
    """横向排布一行，返回 QHBoxLayout。"""
    h = QHBoxLayout()
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(spacing)
    for i, w in enumerate(widgets):
        if w is None:
            h.addStretch(1)
        elif isinstance(w, int):
            h.addSpacing(w)
        else:
            h.addWidget(w)
    if stretch_last:
        h.addStretch(1)
    return h


def chip(text, tooltip=""):
    """状态胶囊：默认灰底，激活时由调用方改色。"""
    lb = QLabel(text)
    lb.setAlignment(Qt.AlignCenter)
    lb.setToolTip(tooltip)
    lb.setMinimumWidth(52)
    lb.setFixedHeight(22)
    lb.setStyleSheet(
        "background:#EDEDED; color:#8C8C8C; border-radius:11px;"
        "font-family:'%s'; font-size:8pt; font-weight:600; padding:0 8px;" % FONT_MONO)
    return lb


def set_chip(lb, active, kind="fault"):
    if not active:
        lb.setStyleSheet(
            "background:#EDEDED; color:#8C8C8C; border-radius:11px;"
            "font-family:'%s'; font-size:8pt; font-weight:600; padding:0 8px;" % FONT_MONO)
        return
    bg = {"en": OK_GREEN, "fault": ERR_RED, "warn": WARN_AMBER}.get(kind, WARN_AMBER)
    lb.setStyleSheet(
        "background:%s; color:#FFFFFF; border-radius:11px;"
        "font-family:'%s'; font-size:8pt; font-weight:600; padding:0 8px;" % (bg, FONT_MONO))


# ---- 串口输入框（两条路径下 API 不同，这里统一封装）--------------------------
def port_box():
    """可编辑串口框：既能下拉选择，也能直接输入 COM38。"""
    b = EditableComboBox()
    b.setPlaceholderText("COM38")
    b.setMinimumWidth(130)
    return b


def fill_ports(box, devices):
    box.clear()
    box.addItems(list(devices))


def port_text(box):
    if isinstance(box, QComboBox):
        return (box.currentText() or "").strip()
    return (box.text() or "").strip()


def set_port(box, text):
    if isinstance(box, QComboBox):
        box.setEditText(text)
    else:
        box.setText(text)


# ---- 数值输入框：统一改用原生控件 -------------------------------------------
# qfluentwidgets 1.11.3 的 SpinBox / DoubleSpinBox 在 Qt 6.12 下不绘制文本
# （程序里 text() 取得到值，界面上却是空的），这里改用原生控件 + 自绘 QSS，
# 任何版本都能可靠显示数值，观感与 Fluent 输入框保持一致。
SPIN_QSS = """
QSpinBox, QDoubleSpinBox {
    background: #FBFBFB;
    border: 1px solid #DCDCDC;
    border-bottom: 1px solid #C8C8C8;
    border-radius: 4px;
    padding: 3px 4px 3px 8px;
    min-height: 20px;
    color: %(text)s;
    selection-background-color: %(accent)s;
    selection-color: #FFFFFF;
}
QSpinBox:focus, QDoubleSpinBox:focus {
    background: #FFFFFF;
    border-bottom: 2px solid %(accent)s;
}
QSpinBox:disabled, QDoubleSpinBox:disabled { color: #A8A8A8; background: #F4F4F4; }
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 15px;
    margin: 2px 2px 0 0;
    border: none;
    background: transparent;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 15px;
    margin: 0 2px 2px 0;
    border: none;
    background: transparent;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background: #EDEDED;
    border-radius: 3px;
}
""" % {"text": TEXT, "accent": ACCENT}


class SpinBox(QSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(SPIN_QSS)


class DoubleSpinBox(QDoubleSpinBox):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet(SPIN_QSS)


def switch():
    """无文字的开关：Fluent 版默认带 On/Off 字样，这里清掉，标签另行放置。"""
    sw = SwitchButton()
    for name in ("setOnText", "setOffText"):
        if hasattr(sw, name):
            try:
                getattr(sw, name)("")
            except Exception:
                pass
    if isinstance(sw, QCheckBox):
        sw.setText("")
    sw.setFixedHeight(24)
    return sw
