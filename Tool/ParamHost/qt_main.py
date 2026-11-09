# -*- coding: utf-8 -*-
"""
驱动板上位机 · Qt6 入口

依赖：PySide6（必需）、PySide6-Fluent-Widgets（可选，缺失时自动降级为原生控件）、
      pyserial（必需）

用法：
    python qt_main.py            启动界面
    python qt_main.py --check    环境自检（解释器 / Qt / Fluent / pyserial / 串口清单）
"""

import os
import subprocess
import sys
import time

PROBE_FULL = "import sys, PySide6, serial; print(sys.executable)"
PROBE_QT = "import sys, PySide6; print(sys.executable)"


def _self_heal():
    """当前解释器缺 PySide6 / pyserial 时，自动换一个具备依赖的解释器重启。"""
    if getattr(sys, "frozen", False):      # 打包成 exe 后依赖已内置，无需自愈
        return False
    if os.environ.get("MPH_RELAUNCH") or sys.platform != "win32":
        return False

    need_serial = _ports_ok_is_false()
    probe = PROBE_FULL if need_serial else PROBE_QT
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
        key = os.path.normcase(exe)
        if key == os.path.normcase(sys.executable) or key in seen:
            continue
        seen.add(key)
        env = dict(os.environ)
        env["MPH_RELAUNCH"] = "1"
        try:
            os.execve(exe, [exe, os.path.abspath(__file__)] + sys.argv[1:], env)
        except Exception:
            continue
    return False


def _ports_ok_is_false():
    try:
        import link as L
        return not L.PYSERIAL_OK
    except Exception:
        return True


def _check():
    import platform
    print("解释器 : %s" % sys.executable)
    print("版本   : %s" % sys.version.split()[0])
    print("系统   : %s" % platform.platform())

    rc = 0
    try:
        import PySide6
        print("PySide6: %s" % PySide6.__version__)
    except Exception as exc:
        print("PySide6: 缺失 -> %s" % exc)
        rc = 1
        PySide6 = None

    try:
        import fluent as F
        print("Fluent : %s" % ("qfluentwidgets 已启用"
                               if F.HAS_FLUENT else "未安装，降级为原生控件 -> %s" % F.FLUENT_ERR))
    except Exception as exc:
        print("Fluent : 加载失败 -> %s" % exc)
        rc = 1

    try:
        import link as L
    except Exception as exc:
        print("pyserial: 模块加载失败 -> %s" % exc)
        return 1

    print("pyserial: %s" % (L.PYSERIAL_VER if L.PYSERIAL_OK else "未安装 -> %s" % L.PYSERIAL_ERR))
    if not L.PYSERIAL_OK:
        rc = 1
    ports = L.list_ports()
    print("串口   : %d 个" % len(ports))
    for dev, desc in ports:
        print("   %-6s %s" % (dev, desc))
    if not ports:
        print("   （若确认有设备，请检查驱动；也可在界面里直接输入端口名）")
        rc = 1
    return rc


def main():
    if "--check" in sys.argv or "-c" in sys.argv:
        return _check()

    try:
        import PySide6  # noqa: F401
    except Exception:
        if "--no-relaunch" not in sys.argv:
            _self_heal()
        sys.stderr.write(
            "当前解释器没有 PySide6：\n  %s\n\n请执行：\n  \"%s\" -m pip install PySide6\n"
            "诊断：python qt_main.py --check\n" % (sys.executable, sys.executable))
        return 2

    if _ports_ok_is_false() and "--no-relaunch" not in sys.argv:
        _self_heal()

    from PySide6.QtCore import Qt
    from PySide6.QtGui import QFont
    from PySide6.QtWidgets import QApplication

    import fluent as F
    from qt_window import MainWindow

    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    app = QApplication(sys.argv)
    app.setApplicationName("Motor ParamHost")

    if F.HAS_FLUENT:
        try:
            F.setTheme(F.Theme.LIGHT)
            F.setThemeColor(F.ACCENT)
        except Exception:
            pass
    # 固定 Fusion + 浅色调色板：避免其他电脑的系统深色/高对比度主题让未着色控件文字发白
    from PySide6.QtGui import QColor, QPalette
    try:    # Qt 6.8+：显式锁定浅色方案，使系统深色模式不再影响原生对话框/标题栏
        app.styleHints().setColorScheme(Qt.ColorScheme.Light)
    except Exception:
        pass
    app.setStyle("Fusion")
    pal = QPalette()
    for role, col in ((QPalette.Window, "#F3F3F3"), (QPalette.WindowText, F.TEXT),
                      (QPalette.Base, "#FFFFFF"), (QPalette.AlternateBase, "#FAFAFA"),
                      (QPalette.Text, F.TEXT), (QPalette.Button, "#FDFDFD"),
                      (QPalette.ButtonText, F.TEXT), (QPalette.ToolTipBase, "#FFFFFF"),
                      (QPalette.ToolTipText, F.TEXT), (QPalette.PlaceholderText, "#666666"),
                      (QPalette.Highlight, F.ACCENT), (QPalette.HighlightedText, "#FFFFFF")):
        pal.setColor(role, QColor(col))
    app.setPalette(pal)
    app.setStyleSheet(F.style_sheet())
    app.setFont(QFont(F.FONT_UI, 9))

    win = MainWindow()
    win.show()
    return app.exec()


def _write_crash_log(exc_text):
    """打包成窗口程序后没有控制台，异常必须落文件，否则只能看到'闪退'。"""
    try:
        base = os.environ.get("TEMP") or os.path.expanduser("~")
        path = os.path.join(base, "MotorParamHost_error.log")
        with open(path, "w", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S\n"))
            f.write("argv: %r\n" % (sys.argv,))
            f.write("frozen: %r\n\n" % (getattr(sys, "frozen", False),))
            f.write(exc_text)
        return path
    except Exception:
        return None


def _excepthook(exc_type, exc_value, exc_tb):
    import traceback
    _write_crash_log("".join(traceback.format_exception(exc_type, exc_value, exc_tb)))


if __name__ == "__main__":
    sys.excepthook = _excepthook
    try:
        code = main()
    except SystemExit:
        raise
    except BaseException:
        import traceback
        path = _write_crash_log(traceback.format_exc())
        sys.stderr.write("启动失败，详见 %s\n" % path)
        code = 3
    sys.exit(code)
