APP_STYLE = """
QMainWindow, QWidget {
    background: #111827;
    color: #e5e7eb;
    font-family: "Microsoft YaHei UI";
    font-size: 10pt;
}
QGroupBox {
    border: 1px solid #374151;
    border-radius: 8px;
    margin-top: 12px;
    padding-top: 10px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 5px;
    color: #93c5fd;
}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #1f2937;
    border: 1px solid #4b5563;
    border-radius: 5px;
    padding: 5px 7px;
    min-height: 22px;
    selection-background-color: #2563eb;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border-color: #60a5fa;
}
QPushButton {
    background: #2563eb;
    border: 0;
    border-radius: 5px;
    padding: 7px 14px;
    min-height: 22px;
    font-weight: 600;
}
QPushButton:hover { background: #3b82f6; }
QPushButton:pressed { background: #1d4ed8; }
QPushButton:disabled { background: #374151; color: #6b7280; }
QPushButton#dangerButton { background: #dc2626; }
QPushButton#dangerButton:hover { background: #ef4444; }
QPushButton#stopButton { background: #b45309; }
QPushButton#stopButton:hover { background: #d97706; }
QPushButton#secondaryButton { background: #374151; }
QPushButton#secondaryButton:hover { background: #4b5563; }
QTabWidget::pane { border: 1px solid #374151; border-radius: 6px; }
QTabBar::tab {
    background: #1f2937;
    color: #9ca3af;
    padding: 9px 18px;
    border-top-left-radius: 5px;
    border-top-right-radius: 5px;
    margin-right: 2px;
}
QTabBar::tab:selected { background: #2563eb; color: white; }
QPlainTextEdit {
    background: #0b1220;
    border: 1px solid #374151;
    border-radius: 6px;
    color: #cbd5e1;
    font-family: Consolas;
    font-size: 9pt;
}
QProgressBar {
    background: #1f2937;
    border: 1px solid #374151;
    border-radius: 5px;
    text-align: center;
}
QProgressBar::chunk { background: #22c55e; border-radius: 4px; }
QStatusBar { background: #0b1220; border-top: 1px solid #374151; }
QCheckBox { spacing: 6px; }
QLabel#titleLabel { font-size: 18pt; font-weight: 700; color: #f9fafb; }
QLabel#subtitleLabel { color: #9ca3af; }
QLabel#valueLabel { color: #7dd3fc; font-family: Consolas; font-weight: 600; }
QLabel#okLabel { color: #4ade80; font-weight: 600; }
QLabel#warnLabel { color: #fbbf24; font-weight: 600; }
QLabel#errorLabel { color: #f87171; font-weight: 600; }
"""

