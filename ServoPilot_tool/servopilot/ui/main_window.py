from __future__ import annotations

import queue
import random
import struct
import time
from typing import Callable

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QCloseEvent
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from servopilot.protocol.commands import (
    decode_response,
    pack_end_mode,
    pack_feedback,
    pack_float,
    pack_hello,
    pack_manual,
    pack_raw,
    pack_sine,
    pack_status_mask,
    pack_watchdog,
)
from servopilot.protocol.constants import (
    COMMAND_NAMES,
    RESULT_NAMES,
    Command,
    FeedbackSource,
    FrameType,
    ManualMode,
    PositionEndMode,
    ResultCode,
    SectionId,
    SectionMask,
)
from servopilot.protocol.status import StatusSnapshot, parse_status_sections
from servopilot.transport.serial_client import ClientEvent, PendingRequest, SerialProtocolClient


SPEED_STATE = {0: "空闲", 1: "运行", 2: "故障", 3: "反馈超时"}
POSITION_STATE = {0: "空闲", 1: "运动", 2: "已到位", 3: "保持", 4: "故障", 5: "超时"}
LOOP_MODE = {0: "无", 1: "速度环", 2: "位置环"}
BIAS_STATE = {0: "未开始", 1: "校准中", 2: "已完成"}


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("ServoPilot V1.0 — 伺服运动控制与调试平台")
        self.resize(1220, 820)
        self.setMinimumSize(1050, 720)

        self.client = SerialProtocolClient()
        self.protocol_ready = False
        self.last_snapshot = StatusSnapshot()
        self.last_rx_time = 0.0
        self._closing_after_disable = False

        self._build_ui()
        self._refresh_ports()

        self.event_timer = QTimer(self)
        self.event_timer.timeout.connect(self._process_events)
        self.event_timer.start(20)

        self.heartbeat_timer = QTimer(self)
        self.heartbeat_timer.timeout.connect(self._heartbeat)
        self.heartbeat_timer.start(100)

        self.status_timer = QTimer(self)
        self.status_timer.timeout.connect(self._periodic_status)
        self.status_timer.start(100)

    def _build_ui(self) -> None:
        central = QWidget()
        root = QVBoxLayout(central)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(10)

        heading = QHBoxLayout()
        title_box = QVBoxLayout()
        title = QLabel("ServoPilot")
        title.setObjectName("titleLabel")
        subtitle = QLabel("伺服运动控制与调试平台 · Protocol V1")
        subtitle.setObjectName("subtitleLabel")
        title_box.addWidget(title)
        title_box.addWidget(subtitle)
        heading.addLayout(title_box)
        heading.addStretch()
        self.link_label = QLabel("● 未连接")
        self.link_label.setObjectName("errorLabel")
        heading.addWidget(self.link_label)
        root.addLayout(heading)

        connection = QGroupBox("连接与安全")
        connection_layout = QHBoxLayout(connection)
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(120)
        refresh = QPushButton("刷新串口")
        refresh.setObjectName("secondaryButton")
        refresh.clicked.connect(self._refresh_ports)
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["921600", "460800", "230400", "115200"])
        self.connect_button = QPushButton("连接")
        self.connect_button.clicked.connect(self._toggle_connection)
        self.watchdog_check = QCheckBox("通信看门狗")
        self.watchdog_check.setChecked(True)
        self.watchdog_spin = QSpinBox()
        self.watchdog_spin.setRange(100, 5000)
        self.watchdog_spin.setValue(500)
        self.watchdog_spin.setSuffix(" ms")
        self.auto_status_check = QCheckBox("自动刷新状态")
        self.auto_status_check.setChecked(True)
        self.status_period_spin = QSpinBox()
        self.status_period_spin.setRange(50, 2000)
        self.status_period_spin.setValue(100)
        self.status_period_spin.setSuffix(" ms")
        disable = QPushButton("全局停止 / 失能")
        disable.setObjectName("dangerButton")
        disable.clicked.connect(lambda: self._send(Command.OUTPUT_DISABLE, retries=2))

        connection_layout.addWidget(QLabel("串口"))
        connection_layout.addWidget(self.port_combo)
        connection_layout.addWidget(refresh)
        connection_layout.addWidget(QLabel("波特率"))
        connection_layout.addWidget(self.baud_combo)
        connection_layout.addWidget(self.connect_button)
        connection_layout.addSpacing(12)
        connection_layout.addWidget(self.watchdog_check)
        connection_layout.addWidget(self.watchdog_spin)
        connection_layout.addWidget(self.auto_status_check)
        connection_layout.addWidget(self.status_period_spin)
        connection_layout.addStretch()
        connection_layout.addWidget(disable)
        root.addWidget(connection)

        self.tabs = QTabWidget()
        self.tabs.addTab(self._build_overview_tab(), "总览")
        self.tabs.addTab(self._build_manual_tab(), "手动控制")
        self.tabs.addTab(self._build_gyro_tab(), "陀螺仪")
        self.tabs.addTab(self._build_speed_tab(), "速度环")
        self.tabs.addTab(self._build_position_tab(), "位置环")
        self.tabs.addTab(self._build_log_tab(), "通信日志")
        root.addWidget(self.tabs, 1)

        self.setCentralWidget(central)
        self.statusBar().showMessage("请连接控制板。当前STM32固件需要实现Protocol V1后才能通信。")

    def _value_label(self, initial: str = "--") -> QLabel:
        label = QLabel(initial)
        label.setObjectName("valueLabel")
        label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        return label

    def _build_overview_tab(self) -> QWidget:
        tab = QWidget()
        grid = QGridLayout(tab)

        device = QGroupBox("设备与PCU")
        device_form = QFormLayout(device)
        self.protocol_value = self._value_label()
        self.firmware_value = self._value_label()
        self.boot_value = self._value_label()
        self.pcu_valid_value = self._value_label()
        self.pcu_enabled_value = self._value_label()
        self.pcu_angle_value = self._value_label()
        self.pcu_status_value = self._value_label()
        self.pcu_fault_value = self._value_label()
        device_form.addRow("协议版本", self.protocol_value)
        device_form.addRow("固件版本", self.firmware_value)
        device_form.addRow("Boot ID", self.boot_value)
        device_form.addRow("PCU反馈", self.pcu_valid_value)
        device_form.addRow("PCU使能", self.pcu_enabled_value)
        device_form.addRow("编码器角度", self.pcu_angle_value)
        device_form.addRow("状态字", self.pcu_status_value)
        device_form.addRow("故障", self.pcu_fault_value)

        control = QGroupBox("控制状态")
        control_form = QFormLayout(control)
        self.loop_mode_value = self._value_label()
        self.speed_state_value = self._value_label()
        self.position_state_value = self._value_label()
        self.target_speed_value = self._value_label()
        self.feedback_speed_value = self._value_label()
        self.speed_error_value = self._value_label()
        self.current_command_value = self._value_label()
        self.position_value = self._value_label()
        self.position_error_value = self._value_label()
        control_form.addRow("当前控制模式", self.loop_mode_value)
        control_form.addRow("速度环", self.speed_state_value)
        control_form.addRow("位置环", self.position_state_value)
        control_form.addRow("目标/参考速度", self.target_speed_value)
        control_form.addRow("反馈速度", self.feedback_speed_value)
        control_form.addRow("速度误差", self.speed_error_value)
        control_form.addRow("电流指令", self.current_command_value)
        control_form.addRow("当前位置", self.position_value)
        control_form.addRow("位置误差", self.position_error_value)

        gyro = QGroupBox("陀螺仪摘要")
        gyro_form = QFormLayout(gyro)
        self.gyro_valid_value = self._value_label()
        self.gyro_bias_state_value = self._value_label()
        self.gyro_raw_value = self._value_label()
        self.gyro_corrected_value = self._value_label()
        self.gyro_bias_value = self._value_label()
        self.gyro_frames_value = self._value_label()
        gyro_form.addRow("数据状态", self.gyro_valid_value)
        gyro_form.addRow("零偏校准", self.gyro_bias_state_value)
        gyro_form.addRow("原始X角速度", self.gyro_raw_value)
        gyro_form.addRow("校正X角速度", self.gyro_corrected_value)
        gyro_form.addRow("X轴零偏", self.gyro_bias_value)
        gyro_form.addRow("有效帧数", self.gyro_frames_value)

        communication = QGroupBox("通信统计")
        communication_form = QFormLayout(communication)
        self.pending_value = self._value_label("0")
        self.last_rx_value = self._value_label()
        self.command_count_value = self._value_label()
        self.error_count_value = self._value_label()
        self.uptime_value = self._value_label()
        refresh = QPushButton("立即读取全部状态")
        refresh.clicked.connect(self._request_status)
        capabilities = QPushButton("读取设备能力")
        capabilities.setObjectName("secondaryButton")
        capabilities.clicked.connect(lambda: self._send(Command.GET_CAPABILITIES))
        communication_form.addRow("等待应答", self.pending_value)
        communication_form.addRow("最近接收", self.last_rx_value)
        communication_form.addRow("板端命令计数", self.command_count_value)
        communication_form.addRow("板端错误计数", self.error_count_value)
        communication_form.addRow("板端运行时间", self.uptime_value)
        communication_form.addRow(refresh)
        communication_form.addRow(capabilities)

        grid.addWidget(device, 0, 0)
        grid.addWidget(control, 0, 1)
        grid.addWidget(gyro, 1, 0)
        grid.addWidget(communication, 1, 1)
        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(2, 1)
        return tab

    def _build_manual_tab(self) -> QWidget:
        tab = QWidget()
        layout = QHBoxLayout(tab)

        command = QGroupBox("手动电压/电流")
        form = QFormLayout(command)
        self.manual_mode_combo = QComboBox()
        self.manual_mode_combo.addItem("电流闭环", ManualMode.CURRENT_CLOSED_LOOP)
        self.manual_mode_combo.addItem("电压开环", ManualMode.VOLTAGE_OPEN_LOOP)
        self.manual_raw_spin = QSpinBox()
        self.manual_raw_spin.setRange(-32767, 32767)
        stage = QPushButton("暂存指令")
        stage.clicked.connect(self._stage_manual)
        enable = QPushButton("使能并输出")
        enable.clicked.connect(lambda: self._send(Command.MANUAL_ENABLE, retries=1))
        disable = QPushButton("立即停止输出")
        disable.setObjectName("dangerButton")
        disable.clicked.connect(lambda: self._send(Command.OUTPUT_DISABLE, retries=2))
        form.addRow("控制模式", self.manual_mode_combo)
        form.addRow("原始指令", self.manual_raw_spin)
        form.addRow(stage)
        form.addRow(enable)
        form.addRow(disable)

        maintenance = QGroupBox("PCU维护")
        maintenance_form = QFormLayout(maintenance)
        self.zero_raw_spin = QSpinBox()
        self.zero_raw_spin.setRange(-32767, 32767)
        clear = QPushButton("清除PCU故障")
        clear.setObjectName("stopButton")
        clear.clicked.connect(lambda: self._send(Command.CLEAR_FAULT, retries=1))
        zero = QPushButton("开始PCU自动找零")
        zero.setObjectName("stopButton")
        zero.clicked.connect(self._auto_zero)
        note = QLabel("自动找零前板端必须停止速度环、位置环和纯电流测试，并关闭输出。")
        note.setWordWrap(True)
        note.setObjectName("warnLabel")
        maintenance_form.addRow("找零原始值", self.zero_raw_spin)
        maintenance_form.addRow(clear)
        maintenance_form.addRow(zero)
        maintenance_form.addRow(note)

        layout.addWidget(command)
        layout.addWidget(maintenance)
        layout.addStretch()
        return tab

    def _build_gyro_tab(self) -> QWidget:
        tab = QWidget()
        layout = QVBoxLayout(tab)
        state = QGroupBox("GSA200 X轴零偏校准")
        form = QFormLayout(state)
        self.gyro_tab_valid = self._value_label()
        self.gyro_tab_raw = self._value_label()
        self.gyro_tab_corrected = self._value_label()
        self.gyro_tab_bias = self._value_label()
        self.gyro_progress = QProgressBar()
        self.gyro_progress.setRange(0, 2000)
        self.gyro_progress.setValue(0)
        start = QPushButton("开始零偏校准")
        start.clicked.connect(lambda: self._send(Command.GYRO_BIAS_START, retries=1))
        note = QLabel("校准期间必须保持电机和机构完全静止。速度环、位置环及PCU输出必须关闭。")
        note.setWordWrap(True)
        note.setObjectName("warnLabel")
        form.addRow("数据有效", self.gyro_tab_valid)
        form.addRow("原始角速度", self.gyro_tab_raw)
        form.addRow("校正角速度", self.gyro_tab_corrected)
        form.addRow("当前零偏", self.gyro_tab_bias)
        form.addRow("校准进度", self.gyro_progress)
        form.addRow(start)
        form.addRow(note)
        layout.addWidget(state)
        layout.addStretch()
        return tab

    def _double_spin(self, minimum: float, maximum: float, value: float, decimals: int = 3) -> QDoubleSpinBox:
        spin = QDoubleSpinBox()
        spin.setRange(minimum, maximum)
        spin.setDecimals(decimals)
        spin.setValue(value)
        spin.setKeyboardTracking(False)
        return spin

    def _build_speed_tab(self) -> QWidget:
        tab = QWidget()
        layout = QGridLayout(tab)

        basic = QGroupBox("恒定速度环")
        form = QFormLayout(basic)
        self.speed_feedback_combo = QComboBox()
        self.speed_feedback_combo.addItem("PCU编码器差分", FeedbackSource.PCU_DIFFERENTIAL)
        self.speed_feedback_combo.addItem("GSA200陀螺仪", FeedbackSource.GYRO)
        apply_feedback = QPushButton("应用反馈源")
        apply_feedback.clicked.connect(self._set_feedback_source)
        self.speed_target_spin = self._double_spin(-100000.0, 100000.0, 0.0, 3)
        self.speed_target_spin.setSuffix(" rpm")
        set_target = QPushButton("设置目标速度")
        set_target.clicked.connect(lambda: self._send(Command.SPEED_SET_TARGET, pack_float(self.speed_target_spin.value(), "目标速度")))
        start = QPushButton("启动速度环")
        start.clicked.connect(lambda: self._send(Command.SPEED_START, retries=1))
        stop = QPushButton("停止速度环")
        stop.setObjectName("stopButton")
        stop.clicked.connect(lambda: self._send(Command.SPEED_STOP, retries=2))
        form.addRow("反馈来源", self.speed_feedback_combo)
        form.addRow(apply_feedback)
        form.addRow("目标速度", self.speed_target_spin)
        form.addRow(set_target)
        form.addRow(start)
        form.addRow(stop)

        tuning = QGroupBox("速度环参数")
        tuning_form = QFormLayout(tuning)
        self.speed_kp_spin = self._double_spin(0.0, 100000.0, 0.001, 6)
        self.speed_ki_spin = self._double_spin(0.0, 100000.0, 0.01, 6)
        self.speed_current_spin = self._double_spin(0.001, 1000.0, 0.5, 3)
        self.speed_current_spin.setSuffix(" A")
        apply = QPushButton("应用速度环参数")
        apply.clicked.connect(self._apply_speed_parameters)
        tuning_form.addRow("Kp (A/rpm)", self.speed_kp_spin)
        tuning_form.addRow("Ki (A/rpm/s)", self.speed_ki_spin)
        tuning_form.addRow("最大电流", self.speed_current_spin)
        tuning_form.addRow(apply)

        sine = QGroupBox("正弦速度模式")
        sine_form = QFormLayout(sine)
        self.sine_base_spin = self._double_spin(-100000.0, 100000.0, 0.0, 3)
        self.sine_amplitude_spin = self._double_spin(0.0, 100000.0, 1.0, 3)
        self.sine_frequency_spin = self._double_spin(0.01, 50.0, 1.0, 3)
        set_sine = QPushButton("设置正弦参数")
        set_sine.clicked.connect(self._set_sine_parameters)
        start_sine = QPushButton("启动正弦速度环")
        start_sine.clicked.connect(lambda: self._send(Command.SPEED_SINE_START, retries=1))
        stop_sine = QPushButton("停止正弦速度环")
        stop_sine.setObjectName("stopButton")
        stop_sine.clicked.connect(lambda: self._send(Command.SPEED_SINE_STOP, retries=2))
        sine_form.addRow("基准速度 (rpm)", self.sine_base_spin)
        sine_form.addRow("幅值 (rpm)", self.sine_amplitude_spin)
        sine_form.addRow("频率 (Hz)", self.sine_frequency_spin)
        sine_form.addRow(set_sine)
        sine_form.addRow(start_sine)
        sine_form.addRow(stop_sine)

        monitor = QGroupBox("速度环监视")
        monitor_form = QFormLayout(monitor)
        self.speed_tab_state = self._value_label()
        self.speed_tab_reference = self._value_label()
        self.speed_tab_feedback = self._value_label()
        self.speed_tab_error = self._value_label()
        self.speed_tab_current = self._value_label()
        self.speed_tab_raw = self._value_label()
        self.speed_tab_age = self._value_label()
        monitor_form.addRow("状态", self.speed_tab_state)
        monitor_form.addRow("参考速度", self.speed_tab_reference)
        monitor_form.addRow("反馈速度", self.speed_tab_feedback)
        monitor_form.addRow("速度误差", self.speed_tab_error)
        monitor_form.addRow("电流指令", self.speed_tab_current)
        monitor_form.addRow("PCU原始指令", self.speed_tab_raw)
        monitor_form.addRow("反馈年龄", self.speed_tab_age)

        layout.addWidget(basic, 0, 0)
        layout.addWidget(tuning, 0, 1)
        layout.addWidget(sine, 1, 0)
        layout.addWidget(monitor, 1, 1)
        layout.setColumnStretch(0, 1)
        layout.setColumnStretch(1, 1)
        return tab

    def _build_position_tab(self) -> QWidget:
        tab = QWidget()
        layout = QHBoxLayout(tab)
        command = QGroupBox("相对位置控制")
        form = QFormLayout(command)
        self.position_delta_spin = self._double_spin(-3600.0, 3600.0, 0.0, 3)
        self.position_delta_spin.setSuffix(" °")
        self.position_kp_spin = self._double_spin(0.0, 100000.0, 1.0, 6)
        self.position_max_speed_spin = self._double_spin(0.001, 100000.0, 5.0, 3)
        self.position_max_speed_spin.setSuffix(" rpm")
        self.position_end_combo = QComboBox()
        self.position_end_combo.addItem("到位后失能", PositionEndMode.DISABLE)
        self.position_end_combo.addItem("到位后保持", PositionEndMode.HOLD)
        apply = QPushButton("应用位置环参数")
        apply.clicked.connect(self._apply_position_parameters)
        move = QPushButton("开始相对位置运动")
        move.clicked.connect(lambda: self._send(Command.POSITION_MOVE_RELATIVE, pack_float(self.position_delta_spin.value(), "相对角度"), retries=1))
        stop = QPushButton("停止位置环")
        stop.setObjectName("stopButton")
        stop.clicked.connect(lambda: self._send(Command.POSITION_STOP, retries=2))
        form.addRow("相对运动角度", self.position_delta_spin)
        form.addRow("位置Kp (rpm/deg)", self.position_kp_spin)
        form.addRow("最大速度", self.position_max_speed_spin)
        form.addRow("到位动作", self.position_end_combo)
        form.addRow(apply)
        form.addRow(move)
        form.addRow(stop)

        monitor = QGroupBox("位置环监视")
        monitor_form = QFormLayout(monitor)
        self.position_tab_state = self._value_label()
        self.position_tab_current = self._value_label()
        self.position_tab_target = self._value_label()
        self.position_tab_error = self._value_label()
        self.position_tab_speed = self._value_label()
        self.position_tab_end = self._value_label()
        monitor_form.addRow("状态", self.position_tab_state)
        monitor_form.addRow("当前位置", self.position_tab_current)
        monitor_form.addRow("目标位置", self.position_tab_target)
        monitor_form.addRow("位置误差", self.position_tab_error)
        monitor_form.addRow("速度环参考", self.position_tab_speed)
        monitor_form.addRow("到位模式", self.position_tab_end)

        layout.addWidget(command)
        layout.addWidget(monitor)
        layout.addStretch()
        return tab

    def _build_log_tab(self) -> QWidget:
        tab = QWidget()
        layout = QVBoxLayout(tab)
        controls = QHBoxLayout()
        self.raw_log_check = QCheckBox("显示完整十六进制帧")
        clear = QPushButton("清空日志")
        clear.setObjectName("secondaryButton")
        clear.clicked.connect(lambda: self.log_edit.clear())
        controls.addWidget(self.raw_log_check)
        controls.addStretch()
        controls.addWidget(clear)
        self.log_edit = QPlainTextEdit()
        self.log_edit.setReadOnly(True)
        self.log_edit.document().setMaximumBlockCount(3000)
        layout.addLayout(controls)
        layout.addWidget(self.log_edit)
        return tab

    def _refresh_ports(self) -> None:
        current = self.port_combo.currentText()
        ports = self.client.available_ports()
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if current in ports:
            self.port_combo.setCurrentText(current)
        if not ports:
            self.port_combo.addItem("未发现串口")

    def _toggle_connection(self) -> None:
        if self.client.is_open:
            self._disconnect_safely()
            return
        port = self.port_combo.currentText().strip()
        if not port or port == "未发现串口":
            QMessageBox.warning(self, "串口", "没有可用串口，请连接设备后刷新。")
            return
        try:
            self.client.open(port, int(self.baud_combo.currentText()))
        except Exception as exc:
            QMessageBox.critical(self, "连接失败", str(exc))

    def _disconnect_safely(self) -> None:
        if self.protocol_ready:
            try:
                self.client.send_request(Command.OUTPUT_DISABLE, retries=0, timeout=0.1)
            except Exception:
                pass
        QTimer.singleShot(100, self._close_transport)

    def _close_transport(self) -> None:
        self.client.close()
        self.protocol_ready = False
        self.connect_button.setText("连接")
        self.link_label.setText("● 未连接")
        self.link_label.setObjectName("errorLabel")
        self.link_label.style().unpolish(self.link_label)
        self.link_label.style().polish(self.link_label)

    def _send(self, command: Command, payload: bytes = b"", retries: int = 2) -> int | None:
        if not self.client.is_open:
            self.statusBar().showMessage("请先连接控制板", 3000)
            return None
        if not self.protocol_ready and command != Command.HELLO:
            self.statusBar().showMessage("协议握手尚未完成", 3000)
            return None
        try:
            return self.client.send_request(command, payload, retries=retries)
        except (RuntimeError, ValueError, OSError) as exc:
            self._log(f"发送失败：{exc}", error=True)
            self.statusBar().showMessage(str(exc), 4000)
            return None

    def _stage_manual(self) -> None:
        mode = self.manual_mode_combo.currentData()
        self._send(Command.SET_MANUAL_COMMAND, pack_manual(mode, self.manual_raw_spin.value()))

    def _auto_zero(self) -> None:
        self._send(Command.AUTO_ZERO, pack_raw(self.zero_raw_spin.value()), retries=1)

    def _set_feedback_source(self) -> None:
        self._send(Command.SPEED_SET_FEEDBACK, pack_feedback(self.speed_feedback_combo.currentData()))

    def _apply_speed_parameters(self) -> None:
        self._send(Command.SPEED_SET_KP, pack_float(self.speed_kp_spin.value(), "速度Kp"))
        self._send(Command.SPEED_SET_KI, pack_float(self.speed_ki_spin.value(), "速度Ki"))
        self._send(Command.SPEED_SET_MAX_CURRENT, pack_float(self.speed_current_spin.value(), "最大电流"))

    def _set_sine_parameters(self) -> None:
        try:
            payload = pack_sine(
                self.sine_base_spin.value(),
                self.sine_amplitude_spin.value(),
                self.sine_frequency_spin.value(),
            )
        except ValueError as exc:
            QMessageBox.warning(self, "正弦参数", str(exc))
            return
        self._send(Command.SPEED_SET_SINE, payload)

    def _apply_position_parameters(self) -> None:
        self._send(Command.POSITION_SET_KP, pack_float(self.position_kp_spin.value(), "位置Kp"))
        self._send(Command.POSITION_SET_MAX_SPEED, pack_float(self.position_max_speed_spin.value(), "位置最大速度"))
        self._send(Command.POSITION_SET_END_MODE, pack_end_mode(self.position_end_combo.currentData()))

    def _request_status(self) -> None:
        if self.client.command_pending(Command.GET_STATUS):
            return
        self._send(Command.GET_STATUS, pack_status_mask(SectionMask.ALL), retries=1)

    def _heartbeat(self) -> None:
        if not self.protocol_ready or self.client.command_pending(Command.PING):
            return
        self._send(Command.PING, retries=0)

    def _periodic_status(self) -> None:
        self.pending_value.setText(str(self.client.pending_count))
        if not self.protocol_ready or not self.auto_status_check.isChecked():
            return
        period = self.status_period_spin.value() / 1000.0
        if time.monotonic() - getattr(self, "_last_status_request", 0.0) >= period:
            self._last_status_request = time.monotonic()
            self._request_status()

    def _process_events(self) -> None:
        while True:
            try:
                event = self.client.events.get_nowait()
            except queue.Empty:
                break
            self._handle_event(event)

    def _handle_event(self, event: ClientEvent) -> None:
        if event.kind == "connected":
            self.connect_button.setText("断开")
            self.link_label.setText("● 串口已打开，正在握手")
            self.link_label.setObjectName("warnLabel")
            self._refresh_label_style(self.link_label)
            self._log(f"已打开 {event.data['port']} @ {event.data['baudrate']} 8N1")
            self.protocol_ready = False
            self._send_hello()
        elif event.kind == "disconnected":
            self._log("串口连接已断开", error=True)
            self._close_transport()
        elif event.kind == "error":
            self._log(str(event.data), error=True)
            self.statusBar().showMessage(str(event.data), 5000)
        elif event.kind == "tx":
            data = event.data
            name = COMMAND_NAMES.get(data["command"], f"0x{data['command']:04X}")
            suffix = f"  {data['packet'].hex(' ').upper()}" if self.raw_log_check.isChecked() else ""
            self._log(f"TX seq={data['sequence']} {name}{suffix}")
        elif event.kind == "retry":
            request: PendingRequest = event.data
            self._log(f"重发 seq={request.sequence} {self.client.command_name(request.command)}，第{request.attempts}次", error=True)
        elif event.kind == "timeout":
            request = event.data
            self._log(f"应答超时 seq={request.sequence} {self.client.command_name(request.command)}", error=True)
            self.statusBar().showMessage(f"命令应答超时：{self.client.command_name(request.command)}", 4000)
        elif event.kind == "frame":
            self._handle_frame_event(event.data)

    def _send_hello(self) -> None:
        try:
            self.client.send_request(Command.HELLO, pack_hello(random.getrandbits(32)), timeout=0.5, retries=3)
        except Exception as exc:
            self._log(f"握手发送失败：{exc}", error=True)

    def _handle_frame_event(self, data: dict) -> None:
        frame = data["frame"]
        self.last_rx_time = time.monotonic()
        self.last_rx_value.setText(time.strftime("%H:%M:%S"))
        name = COMMAND_NAMES.get(frame.command, f"0x{frame.command:04X}")
        raw = frame.to_bytes().hex(" ").upper()
        suffix = f"  {raw}" if self.raw_log_check.isChecked() else ""
        self._log(f"RX seq={frame.sequence} {name} type={frame.frame_type.name}{suffix}")

        if frame.frame_type == FrameType.EVENT:
            if frame.command == Command.GET_STATUS:
                self._accept_status(frame.payload)
            elif frame.command == Command.FAULT_EVENT:
                self._log("控制板主动上报故障事件", error=True)
                self.statusBar().showMessage("控制板故障，请查看PCU状态", 8000)
            return
        if frame.frame_type != FrameType.RESPONSE:
            return

        try:
            result_raw, detail, response_data = decode_response(frame.payload)
        except ValueError as exc:
            self._log(f"应答格式错误：{exc}", error=True)
            return
        try:
            result = ResultCode(result_raw)
            result_text = RESULT_NAMES[result]
        except ValueError:
            result = None
            result_text = f"未知结果码0x{result_raw:02X}"

        if result_raw != ResultCode.OK:
            self._log(f"命令失败 {name}：{result_text}，detail=0x{detail:02X}", error=True)
            self.statusBar().showMessage(f"{name}：{result_text}", 5000)
            return

        self.statusBar().showMessage(f"{name}：执行成功", 1800)
        if frame.command == Command.HELLO:
            self._accept_hello(response_data)
        elif frame.command == Command.GET_STATUS:
            self._accept_status(response_data)
        elif frame.command == Command.GET_CAPABILITIES and len(response_data) >= 4:
            capabilities = struct.unpack_from("<I", response_data)[0]
            self._log(f"设备能力位：0x{capabilities:08X}")

    def _accept_hello(self, data: bytes) -> None:
        fmt = "<BBBBBBHIII"
        if len(data) < struct.calcsize(fmt):
            self._log("HELLO应答长度不足", error=True)
            return
        protocol_major, protocol_minor, fw_major, fw_minor, fw_patch, _, max_payload, capabilities, boot_id, server_nonce = struct.unpack_from(fmt, data)
        self.protocol_ready = True
        self.protocol_value.setText(f"{protocol_major}.{protocol_minor}")
        self.firmware_value.setText(f"{fw_major}.{fw_minor}.{fw_patch}")
        self.boot_value.setText(f"0x{boot_id:08X}")
        self.link_label.setText("● 已连接")
        self.link_label.setObjectName("okLabel")
        self._refresh_label_style(self.link_label)
        self._log(f"握手成功：Protocol {protocol_major}.{protocol_minor}，FW {fw_major}.{fw_minor}.{fw_patch}，能力=0x{capabilities:08X}，nonce=0x{server_nonce:08X}")
        if self.watchdog_check.isChecked():
            self._send(Command.SET_WATCHDOG, pack_watchdog(self.watchdog_spin.value()), retries=1)
        self._request_status()

    def _accept_status(self, data: bytes) -> None:
        try:
            snapshot = parse_status_sections(data)
        except ValueError as exc:
            self._log(f"状态解析错误：{exc}", error=True)
            return
        self.last_snapshot = snapshot
        self._update_status_widgets(snapshot)

    def _update_status_widgets(self, snapshot: StatusSnapshot) -> None:
        pcu = snapshot.get(SectionId.PCU)
        if pcu:
            valid = bool(pcu["data_valid"])
            status = int(pcu["status_bits"])
            self.pcu_valid_value.setText("有效" if valid else "无效")
            self.pcu_enabled_value.setText("已使能" if status & 0x04 else "已失能")
            self.pcu_angle_value.setText(f"{pcu['angle_deg']:.3f} °")
            self.pcu_status_value.setText(f"0x{status:02X}")
            fault_names = []
            for bit, text in ((0x01, "角度"), (0x02, "过流"), (0x08, "过温"), (0x10, "过载"), (0x40, "欠压"), (0x80, "过压")):
                if status & bit:
                    fault_names.append(text)
            self.pcu_fault_value.setText("、".join(fault_names) if fault_names else "无")

        position = snapshot.get(SectionId.POSITION)
        if position:
            state = int(position["state"])
            self.position_state_value.setText(POSITION_STATE.get(state, str(state)))
            self.position_value.setText(f"{position['current_mdeg'] / 1000.0:.3f} °")
            self.position_error_value.setText(f"{position['error_mdeg'] / 1000.0:.3f} °")
            self.position_tab_state.setText(POSITION_STATE.get(state, str(state)))
            self.position_tab_current.setText(f"{position['current_mdeg'] / 1000.0:.3f} °")
            self.position_tab_target.setText(f"{position['target_mdeg'] / 1000.0:.3f} °")
            self.position_tab_error.setText(f"{position['error_mdeg'] / 1000.0:.3f} °")
            self.position_tab_speed.setText(f"{position['speed_reference_rpm']:.3f} rpm")
            self.position_tab_end.setText("保持" if position["end_mode"] else "到位失能")

        speed = snapshot.get(SectionId.SPEED)
        if speed:
            state = int(speed["state"])
            self.loop_mode_value.setText(LOOP_MODE.get(int(speed["loop_mode"]), str(speed["loop_mode"])))
            self.speed_state_value.setText(SPEED_STATE.get(state, str(state)))
            self.target_speed_value.setText(f"{speed['target_rpm']:.3f} / {speed['reference_rpm']:.3f} rpm")
            self.feedback_speed_value.setText(f"{speed['feedback_rpm']:.3f} rpm")
            self.speed_error_value.setText(f"{speed['error_rpm']:.3f} rpm")
            self.current_command_value.setText(f"{speed['current_command_a']:.3f} A")
            self.speed_tab_state.setText(SPEED_STATE.get(state, str(state)))
            self.speed_tab_reference.setText(f"{speed['reference_rpm']:.3f} rpm")
            self.speed_tab_feedback.setText(f"{speed['feedback_rpm']:.3f} rpm")
            self.speed_tab_error.setText(f"{speed['error_rpm']:.3f} rpm")
            self.speed_tab_current.setText(f"{speed['current_command_a']:.3f} A")
            self.speed_tab_raw.setText(str(speed["current_raw"]))
            self.speed_tab_age.setText(f"{speed['feedback_age_ms']} ms")

        gyro = snapshot.get(SectionId.GYRO)
        if gyro:
            valid_text = "有效" if gyro["data_valid"] else "无效"
            bias_text = BIAS_STATE.get(int(gyro["bias_state"]), str(gyro["bias_state"]))
            self.gyro_valid_value.setText(valid_text)
            self.gyro_bias_state_value.setText(bias_text)
            self.gyro_raw_value.setText(f"{gyro['gyro_x_raw_dps']:.6f} °/s")
            self.gyro_corrected_value.setText(f"{gyro['gyro_x_corrected_dps']:.6f} °/s")
            self.gyro_bias_value.setText(f"{gyro['gyro_x_bias_dps']:.6f} °/s")
            self.gyro_frames_value.setText(str(gyro["valid_frames"]))
            self.gyro_tab_valid.setText(valid_text)
            self.gyro_tab_raw.setText(f"{gyro['gyro_x_raw_dps']:.6f} °/s")
            self.gyro_tab_corrected.setText(f"{gyro['gyro_x_corrected_dps']:.6f} °/s")
            self.gyro_tab_bias.setText(f"{gyro['gyro_x_bias_dps']:.6f} °/s")
            total = max(1, int(gyro["calibration_total"]))
            self.gyro_progress.setRange(0, total)
            self.gyro_progress.setValue(min(total, int(gyro["calibration_samples"])))

        communication = snapshot.get(SectionId.COMMUNICATION)
        if communication:
            self.command_count_value.setText(str(communication["command_count"]))
            self.error_count_value.setText(str(communication["error_count"]))
            self.uptime_value.setText(f"{communication['uptime_ms'] / 1000.0:.1f} s")

    def _refresh_label_style(self, label: QLabel) -> None:
        label.style().unpolish(label)
        label.style().polish(label)

    def _log(self, message: str, error: bool = False) -> None:
        timestamp = time.strftime("%H:%M:%S")
        prefix = "ERR" if error else "INF"
        self.log_edit.appendPlainText(f"[{timestamp}] {prefix} {message}")

    def closeEvent(self, event: QCloseEvent) -> None:
        if self.client.is_open and self.protocol_ready:
            try:
                self.client.send_request(Command.OUTPUT_DISABLE, retries=0, timeout=0.1)
            except Exception:
                pass
        self.client.close()
        event.accept()

