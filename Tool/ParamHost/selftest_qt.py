# -*- coding: utf-8 -*-
"""
Qt 界面功能自检（无需硬件，无窗口弹出）

用假串口驱动真实的 SerialLink + MainWindow，走完：
    连接 -> 自动读表 -> 字段提交写入(暂存) -> 单项撤销 -> 再次写入 ->
    锁存(bit3) -> 回读确认 -> 设定值滑条/模式/使能 -> 断开

运行：python selftest_qt.py
"""

import os
import sys
import time

os.environ["QT_QPA_PLATFORM"] = "offscreen"          # 无窗口，文本渲染无关紧要

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import selftest_link as S                            # 复用假串口驱动器模拟
S.install_fake_serial()

import fluent as F
from PySide6.QtWidgets import QApplication

import protocol as P
from qt_window import MainWindow


def pump(app, ms):
    """处理事件循环 ms 毫秒，让后台线程与定时器都有机会跑。"""
    end = time.perf_counter() + ms / 1000.0
    while time.perf_counter() < end:
        app.processEvents()
        time.sleep(0.005)


def main():
    # QWidget 必须挂在 QApplication 上（QCoreApplication 会让 Qt 直接退出）。
    app = QApplication.instance() or QApplication(sys.argv)
    win = MainWindow()

    ok = True

    def check(cond, name, detail=""):
        nonlocal ok
        print("  [%s] %-42s %s" % ("PASS" if cond else "FAIL", name, detail))
        ok = ok and bool(cond)

    # ---- 连接 ---------------------------------------------------------------
    win.cb_baud.setCurrentText("921600")
    F.set_port(win.cb_port, "FAKE")
    win._connect()
    pump(app, 800)

    check(win._connected(), "已建立连接")
    check(len(win.values) >= 13, "连接后自动读取全部参数", "读到 %d 项" % len(win.values))
    check(win.values.get(0x09) == 193, "0x09 电压增益 = 193")
    check(win.ctrl is not None and win.ctrl.is_alive(), "控制帧发送线程在跑")

    # ---- 字段提交（暂存） ----------------------------------------------------
    win._commit_field(0x0A, 18)
    pump(app, 300)
    check(win.pending.get(0x0A, (None, None))[0] == 18, "0x0A 写入进暂存区",
          str(win.pending.get(0x0A)))
    check(win.link.read_param(0x0A)["value"] == 16, "未锁存前生效值仍为 16")
    check(not win.fields[0x0A].pill.isHidden(), "字段行挂出待锁存徽标")  # offscreen 下用 isHidden 判断
    check("锁存生效 (1)" in win.btn_latch.text(), "锁存按钮计数", win.btn_latch.text())

    # ---- 单项撤销 ------------------------------------------------------------
    win._undo_one(0x0A)
    pump(app, 300)
    check(0x0A not in win.pending, "撤销后移出待锁存清单")
    check(S._sim.staged.get(0x0A) == 16, "撤销把旧值写回暂存区", str(S._sim.staged.get(0x0A)))

    # ---- 再次写入 + 锁存 ------------------------------------------------------
    win._commit_field(0x0A, 18)
    pump(app, 300)
    S._sim.latch()                                   # 模拟驱动器响应 bit3
    win._latch()
    pump(app, 900)                                   # 350 ms 等待 + 自动回读
    check(not win.pending, "锁存后待锁存清空")
    check(win.values.get(0x0A) == 18, "回读确认生效值 = 18", str(win.values.get(0x0A)))

    # ---- 非法提交 ------------------------------------------------------------
    win._commit_field(0x0A, 99)                      # 超量程：应拒绝且不进 pending
    pump(app, 200)
    check(0x0A not in win.pending, "超量程提交被拒收")

    # ---- 设定值 / 模式 / 使能 --------------------------------------------------
    win.ref_sliders["ref1"].setValue(P.pct_to_ref(10.0))
    pump(app, 100)
    st = win.ctrl.state()
    check(abs(st["ref1"] - P.pct_to_ref(10.0)) < 1e-6, "设定值1 滑条 -> %d" % P.pct_to_ref(10.0))
    check(st["mode"] == P.MODE_VOLTAGE, "默认电压开环模式")
    check("AA 55 00 01 0C CD" in win.lb_frame.text(), "下行帧预览已更新", win.lb_frame.text()[:60])

    win._pick_mode(P.MODE_CURRENT)
    pump(app, 100)
    check(win.ctrl.state()["mode"] == P.MODE_CURRENT, "点击模式卡切换电流闭环")

    win.sw_enable.setChecked(True)
    pump(app, 100)
    check(win.ctrl.state()["enable"] is True, "使能开关生效")

    # ---- 待锁存多项 + 全部撤销 -------------------------------------------------
    win._commit_field(0x09, 250)
    win._commit_field(0x0A, 19)
    pump(app, 400)
    check(len(win.pending) == 2, "两项修改同时待锁存", str(sorted(win.pending)))
    win._undo_all()
    pump(app, 400)
    check(not win.pending, "全部撤销完成")
    check(S._sim.staged.get(0x09) == 193 and S._sim.staged.get(0x0A) == 18,
          "撤销把各自旧值写回暂存区")

    # ---- 重新读取放弃未锁存修改 -------------------------------------------------
    win._commit_field(0x0A, 19)
    pump(app, 300)
    check(win.pending.get(0x0A, (None, None))[0] == 19, "修改 19 待锁存")
    win._read_all()
    pump(app, 1200)
    check(not win.pending, "重新读取后待锁存清空")
    check(win.values.get(0x0A) == 18, "读回值覆盖界面", str(win.values.get(0x0A)))
    check(win.fields[0x0A].ed_raw.text() == "18", "字段行显示生效值",
          win.fields[0x0A].ed_raw.text())
    check(S._sim.staged.get(0x0A) == 18, "设备暂存也被生效值覆盖（防误锁存）",
          str(S._sim.staged.get(0x0A)))

    # ---- 上行帧看门狗：驱动器断电（串口仍在线）判定离线 --------------------------
    S._sim.auto_ctrl = False            # 停发 1 kHz 上行帧
    end = time.perf_counter() + 2.8
    while time.perf_counter() < end:
        app.processEvents()
        time.sleep(0.01)
    check(win.offline, "2 s 无上行帧判定驱动器离线")
    check(not win.fields[0x01].ed_raw.isEnabled(), "参数页进入锁定（不可修改）")
    check(win.fields[0x01].ed_phys.text() == "" and win.fields[0x01].ed_raw.text() == "",
          "锁定后清空输入框旧值（避免误读）")
    check(win.fields[0x01].ed_phys.placeholderText() == "未连接", "占位符标明未连接")
    check(win.btn_latch.isEnabled() is False, "锁存按钮同步禁用")

    S._sim.auto_ctrl = True             # 恢复供电，上行帧恢复
    S._sim.params[0x0A] = 22
    pump(app, 1500)
    check(not win.offline, "上行帧恢复后自动重新联机")
    check(win.values.get(0x0A) == 22, "重联后重新读取芯片数据", str(win.values.get(0x0A)))
    check(win.fields[0x01].ed_raw.isEnabled(), "参数页解锁")

    # ---- 波形存储：不限时长 + 渐进抽稀控制内存 ---------------------------------
    ch = win.chart
    ch.clear()
    ch.MAX_POINTS = 1000               # 缩小上限加速测试
    for i in range(5000):              # 5000 点 >> 1000，应触发两次抽稀
        ch.feed(i * 0.05, (i % 360) / 360.0, 0.5)
    check(ch.points() <= 1200, "超限时自动抽稀", "点数 %d" % ch.points())
    check(ch.t[-1] >= 5000 * 0.05 - 1.0, "时间轴覆盖全部历史",
          "%.0f s" % ch.t[-1])
    ch.clear()
    check(ch.points() == 0, "清空波形")

    # ---- 断开 -----------------------------------------------------------------
    win._disconnect()
    pump(app, 200)
    check(not win._connected(), "已断开连接")
    check(win.ctrl is None, "控制帧线程已停止")

    # ---- 重新连接（模拟驱动器重新上电）：宿主待锁存状态必须失效 -----------------
    # _connect 会创建一个全新的 FakeSerial 实例 —— 恰好等价于“驱动器重新上电”：
    # RAM 暂存区丢失、参数回落到芯片内的保存值（这里是出厂默认 16，再改成 21）
    F.set_port(win.cb_port, "FAKE")
    win._connect()
    S._sim.params[0x0A] = 21          # 新实例 = 重新上电后的芯片数据
    pump(app, 900)
    check(not win.pending, "重连后宿主端待锁存状态已失效")
    check(win.values.get(0x0A) == 21, "重新读取显示芯片内数据 21", str(win.values.get(0x0A)))
    check(win.fields[0x0A].ed_raw.text() == "21", "字段行显示驱动器实际值",
          win.fields[0x0A].ed_raw.text())
    check(not win.fields[0x0A].pill.isHidden() is False, "字段无残留待锁存徽标")
    win._disconnect()

    print("\n%s" % ("ALL PASS" if ok else "SOME CHECKS FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
