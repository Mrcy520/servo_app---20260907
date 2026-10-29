from __future__ import annotations

import sys

from PySide6.QtGui import QFont
from PySide6.QtWidgets import QApplication

from servopilot import __version__
from servopilot.ui.main_window import MainWindow
from servopilot.ui.styles import APP_STYLE


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("ServoPilot")
    app.setApplicationVersion(__version__)
    app.setOrganizationName("力寻")
    app.setFont(QFont("Microsoft YaHei UI", 10))
    app.setStyleSheet(APP_STYLE)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())

