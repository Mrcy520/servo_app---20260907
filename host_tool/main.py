"""Standalone GSA200 simulator/sender for Windows."""

from __future__ import annotations

import math
import random
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from protocol import build_frame, format_hex

try:
    import serial
    from serial import SerialException
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - exercised by a clean install
    serial = None
    SerialException = Exception
    list_ports = None


DEFAULT_BAUD = 921600
DEFAULT_RATE = 2000.0


class Gsa200HostApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("GSA200 模拟数据上位机")
        self.root.geometry("1000x720")
        self.root.minsize(900, 620)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        self.serial_port = None
        self.send_thread: threading.Thread | None = None
        self.stop_event = threading.Event()
        self.counter = 1
        self.sent_frames = 0
        self.sent_bytes = 0
        self.started_at = 0.0
        self.ui_queue_lock = threading.Lock()
        self.pending_preview: str | None = None

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.rate_var = tk.StringVar(value=str(DEFAULT_RATE))
        self.mode_var = tk.StringVar(value="固定值")
        self.endian_var = tk.StringVar(value="大端（当前固件）")
        self.counter_var = tk.StringVar(value="自动递增")
        self.status_var = tk.StringVar(value="未连接")
        self.stats_var = tk.StringVar(value="已发送 0 帧 / 0 字节")
        self.preview_var = tk.StringVar(value="请先生成一帧")
        self.value_vars: dict[str, list[tk.StringVar]] = {
            "gyro": [tk.StringVar(value=value) for value in ("0.0", "0.0", "0.0")],
            "accel": [tk.StringVar(value=value) for value in ("0.0", "0.0", "1.0")],
        }
        self.temp_var = tk.StringVar(value="25.0")
        self.amplitude_vars = {
            "gyro": tk.StringVar(value="30.0"),
            "accel": tk.StringVar(value="0.5"),
            "temperature": tk.StringVar(value="5.0"),
        }
        self.frequency_var = tk.StringVar(value="1.0")

        self.build_ui()
        self.refresh_ports()
        self.update_preview()
        self.root.after(100, self.update_status)

    def build_ui(self) -> None:
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(2, weight=1)

        connection = ttk.LabelFrame(self.root, text="串口连接")
        connection.grid(row=0, column=0, padx=12, pady=(12, 6), sticky="ew")
        connection.columnconfigure(8, weight=1)
        ttk.Label(connection, text="端口").grid(row=0, column=0, padx=(10, 4), pady=8)
        self.port_combo = ttk.Combobox(connection, textvariable=self.port_var, width=16, state="readonly")
        self.port_combo.grid(row=0, column=1, padx=4, pady=8)
        ttk.Button(connection, text="刷新", command=self.refresh_ports).grid(row=0, column=2, padx=4)
        ttk.Label(connection, text="波特率").grid(row=0, column=3, padx=(16, 4))
        ttk.Combobox(connection, textvariable=self.baud_var, values=("921600", "460800", "230400", "115200"), width=10).grid(row=0, column=4, padx=4)
        ttk.Label(connection, text="数据格式 8N1").grid(row=0, column=5, padx=(16, 4))
        ttk.Label(connection, text="协议序号 0x01（固定）").grid(row=0, column=6, padx=(16, 4))
        self.connect_button = ttk.Button(connection, text="打开串口", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=7, padx=10)
        ttk.Label(connection, textvariable=self.status_var, foreground="#176b3a").grid(row=0, column=8, padx=10, sticky="e")

        settings = ttk.LabelFrame(self.root, text="帧与模拟设置")
        settings.grid(row=1, column=0, padx=12, pady=6, sticky="ew")
        for column in range(8):
            settings.columnconfigure(column, weight=1 if column in (1, 3, 5, 7) else 0)
        ttk.Label(settings, text="模拟模式").grid(row=0, column=0, padx=(10, 4), pady=8)
        mode = ttk.Combobox(settings, textvariable=self.mode_var, values=("固定值", "正弦模拟", "随机扰动"), state="readonly", width=12)
        mode.grid(row=0, column=1, padx=4)
        mode.bind("<<ComboboxSelected>>", lambda _event: self.update_preview())
        ttk.Label(settings, text="发送频率 Hz").grid(row=0, column=2, padx=(16, 4))
        ttk.Entry(settings, textvariable=self.rate_var, width=12).grid(row=0, column=3, padx=4)
        ttk.Label(settings, text="字节序").grid(row=0, column=4, padx=(16, 4))
        endian = ttk.Combobox(settings, textvariable=self.endian_var, values=("大端（当前固件）", "小端（低字节在前）"), state="readonly", width=18)
        endian.grid(row=0, column=5, padx=4)
        endian.bind("<<ComboboxSelected>>", lambda _event: self.update_preview())
        ttk.Label(settings, text="采样计数").grid(row=0, column=6, padx=(16, 4))
        ttk.Combobox(settings, textvariable=self.counter_var, values=("自动递增", "固定 0x01"), state="readonly", width=12).grid(row=0, column=7, padx=(4, 10))

        data = ttk.LabelFrame(self.root, text="传感器数据")
        data.grid(row=2, column=0, padx=12, pady=6, sticky="nsew")
        data.columnconfigure(0, weight=1)
        data.columnconfigure(1, weight=1)
        data.rowconfigure(1, weight=1)
        self.make_axis_panel(data, "陀螺仪 Gyro（°/s）", "gyro", 0)
        self.make_axis_panel(data, "加速度 Acc（g）", "accel", 1)

        temp_panel = ttk.Frame(data)
        temp_panel.grid(row=1, column=0, columnspan=2, padx=10, pady=(0, 8), sticky="ew")
        ttk.Label(temp_panel, text="温度（°C）").grid(row=0, column=0, padx=(0, 8))
        ttk.Entry(temp_panel, textvariable=self.temp_var, width=12).grid(row=0, column=1)
        ttk.Separator(temp_panel, orient="vertical").grid(row=0, column=2, padx=18, sticky="ns")
        ttk.Label(temp_panel, text="正弦/随机幅值：Gyro").grid(row=0, column=3, padx=(0, 4))
        ttk.Entry(temp_panel, textvariable=self.amplitude_vars["gyro"], width=8).grid(row=0, column=4)
        ttk.Label(temp_panel, text="Acc").grid(row=0, column=5, padx=(12, 4))
        ttk.Entry(temp_panel, textvariable=self.amplitude_vars["accel"], width=8).grid(row=0, column=6)
        ttk.Label(temp_panel, text="Temp").grid(row=0, column=7, padx=(12, 4))
        ttk.Entry(temp_panel, textvariable=self.amplitude_vars["temperature"], width=8).grid(row=0, column=8)
        ttk.Label(temp_panel, text="模拟频率 Hz").grid(row=0, column=9, padx=(12, 4))
        ttk.Entry(temp_panel, textvariable=self.frequency_var, width=8).grid(row=0, column=10)

        actions = ttk.Frame(self.root)
        actions.grid(row=3, column=0, padx=12, pady=6, sticky="ew")
        ttk.Button(actions, text="生成预览", command=self.update_preview).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="发送单帧", command=self.send_one).pack(side="left", padx=6)
        self.start_button = ttk.Button(actions, text="开始连续发送", command=self.toggle_sending)
        self.start_button.pack(side="left", padx=6)
        ttk.Label(actions, textvariable=self.stats_var).pack(side="right", padx=4)

        preview = ttk.LabelFrame(self.root, text="协议帧预览 / 运行日志")
        preview.grid(row=4, column=0, padx=12, pady=(6, 12), sticky="ew")
        preview.columnconfigure(0, weight=1)
        ttk.Label(preview, textvariable=self.preview_var, font=("Consolas", 10), anchor="w").grid(row=0, column=0, padx=10, pady=8, sticky="ew")
        self.log = ScrolledText(preview, height=7, state="disabled", font=("Consolas", 9))
        self.log.grid(row=1, column=0, padx=10, pady=(0, 10), sticky="ew")

    def make_axis_panel(self, parent: ttk.Widget, title: str, key: str, column: int) -> None:
        panel = ttk.LabelFrame(parent, text=title)
        panel.grid(row=0, column=column, padx=10, pady=10, sticky="ew")
        labels = ("X", "Y", "Z")
        for row, label in enumerate(labels):
            ttk.Label(panel, text=label, width=3).grid(row=row, column=0, padx=(10, 4), pady=5)
            ttk.Entry(panel, textvariable=self.value_vars[key][row], width=16).grid(row=row, column=1, padx=(0, 10), pady=5)
        panel.columnconfigure(1, weight=1)

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.port_combo["values"] = ()
            self.port_var.set("未安装 pyserial")
            return
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and self.port_var.get() not in ports:
            self.port_var.set(ports[0])
        elif not ports:
            self.port_var.set("")

    def toggle_connection(self) -> None:
        if self.serial_port is not None:
            self.close_serial()
            return
        if serial is None:
            messagebox.showerror("缺少依赖", "未安装 pyserial，请在 host_tool 目录执行：python -m pip install -r requirements.txt")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning("请选择串口", "请先刷新并选择一个 COM 口。")
            return
        try:
            self.serial_port = serial.Serial(port=port, baudrate=int(self.baud_var.get()), bytesize=8, parity=serial.PARITY_NONE, stopbits=1, timeout=0)
            self.status_var.set(f"已连接 {port}")
            self.connect_button.configure(text="关闭串口")
            self.write_log(f"打开 {port} @ {self.baud_var.get()} 8N1")
        except (ValueError, SerialException) as exc:
            self.serial_port = None
            messagebox.showerror("串口打开失败", str(exc))

    def close_serial(self) -> None:
        if self.serial_port is not None:
            try:
                self.serial_port.close()
            finally:
                self.serial_port = None
        self.status_var.set("未连接")
        self.connect_button.configure(text="打开串口")

    def selected_endian(self) -> str:
        return "little" if self.endian_var.get().startswith("小端") else "big"

    def read_values(self) -> tuple[list[float], list[float], float]:
        try:
            gyro = [float(var.get()) for var in self.value_vars["gyro"]]
            accel = [float(var.get()) for var in self.value_vars["accel"]]
            temperature = float(self.temp_var.get())
            return gyro, accel, temperature
        except ValueError as exc:
            raise ValueError("传感器数据必须是数字") from exc

    def capture_config(self) -> dict[str, object]:
        gyro, accel, temperature = self.read_values()
        return {
            "gyro": gyro,
            "accel": accel,
            "temperature": temperature,
            "mode": self.mode_var.get(),
            "gyro_amp": float(self.amplitude_vars["gyro"].get()),
            "accel_amp": float(self.amplitude_vars["accel"].get()),
            "temperature_amp": float(self.amplitude_vars["temperature"].get()),
            "frequency": float(self.frequency_var.get()),
            "endian": self.selected_endian(),
            "fixed_counter": self.counter_var.get().startswith("固定"),
        }

    @staticmethod
    def simulated_values(config: dict[str, object], elapsed: float) -> tuple[list[float], list[float], float]:
        gyro = list(config["gyro"])  # type: ignore[arg-type]
        accel = list(config["accel"])  # type: ignore[arg-type]
        temperature = float(config["temperature"])
        if config["mode"] == "固定值":
            return gyro, accel, temperature
        if config["mode"] == "随机扰动":
            gyro_amp = float(config["gyro_amp"])
            accel_amp = float(config["accel_amp"])
            temp_amp = float(config["temperature_amp"])
            return ([value + random.uniform(-gyro_amp, gyro_amp) for value in gyro], [value + random.uniform(-accel_amp, accel_amp) for value in accel], temperature + random.uniform(-temp_amp, temp_amp))
        frequency = float(config["frequency"])
        phase = 2.0 * math.pi * frequency * elapsed
        gyro_amp = float(config["gyro_amp"])
        accel_amp = float(config["accel_amp"])
        temp_amp = float(config["temperature_amp"])
        return ([gyro_amp * math.sin(phase + axis * 2.0 * math.pi / 3.0) for axis in range(3)], [accel_amp * math.sin(phase + axis * 2.0 * math.pi / 3.0) for axis in range(3)], temperature + temp_amp * math.sin(phase))

    def make_frame(self, elapsed: float = 0.0) -> bytes:
        config = self.capture_config()
        return self.make_frame_from_config(config, elapsed, self.counter)

    @classmethod
    def make_frame_from_config(cls, config: dict[str, object], elapsed: float, counter: int) -> bytes:
        gyro, accel, temperature = cls.simulated_values(config, elapsed)
        frame_counter = 1 if config["fixed_counter"] else counter
        return build_frame(gyro, accel, temperature, frame_counter, str(config["endian"]))

    def update_preview(self) -> None:
        try:
            frame = self.make_frame()
            self.preview_var.set(f"32 字节：{format_hex(frame)}")
        except ValueError as exc:
            self.preview_var.set(f"数据错误：{exc}")

    def send_one(self) -> None:
        if self.serial_port is None:
            messagebox.showwarning("串口未连接", "请先打开串口；预览可在未连接时使用。")
            return
        try:
            config = self.capture_config()
            frame = self.make_frame_from_config(config, 0.0, self.counter)
            self.serial_port.write(frame)
            self.record_send(frame)
            if not config["fixed_counter"]:
                self.counter = (self.counter + 1) & 0xFF or 1
        except (ValueError, SerialException) as exc:
            self.write_log(f"发送失败：{exc}")

    def toggle_sending(self) -> None:
        if self.send_thread and self.send_thread.is_alive():
            self.stop_event.set()
            self.start_button.configure(text="正在停止...")
            return
        if self.serial_port is None:
            messagebox.showwarning("串口未连接", "请先打开串口。")
            return
        try:
            rate = float(self.rate_var.get())
            if not 0.1 <= rate <= 2000.0:
                raise ValueError("发送频率范围为 0.1 到 2000 Hz")
            config = self.capture_config()
        except ValueError as exc:
            messagebox.showerror("频率设置错误", str(exc))
            return
        self.stop_event.clear()
        self.started_at = time.perf_counter()
        self.send_thread = threading.Thread(target=self.send_loop, args=(rate, config), daemon=True)
        self.send_thread.start()
        self.start_button.configure(text="停止连续发送")
        self.write_log(f"开始连续发送，目标频率 {rate:g} Hz")

    def send_loop(self, rate: float, config: dict[str, object]) -> None:
        period = 1.0 / rate
        next_deadline = time.perf_counter()
        counter = self.counter
        while not self.stop_event.is_set():
            elapsed = time.perf_counter() - self.started_at
            try:
                frame = self.make_frame_from_config(config, elapsed, counter)
                if self.serial_port is not None:
                    self.serial_port.write(frame)
                self.record_send(frame)
            except (ValueError, SerialException) as exc:
                self.root.after(0, lambda error=str(exc): self.write_log(f"连续发送停止：{error}"))
                break
            if not config["fixed_counter"]:
                counter = (counter + 1) & 0xFF
                if counter == 0:
                    counter = 1
            next_deadline += period
            delay = next_deadline - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_deadline = time.perf_counter()
        self.root.after(0, self.sending_stopped)

    def sending_stopped(self) -> None:
        self.start_button.configure(text="开始连续发送")
        self.write_log("连续发送已停止")

    def record_send(self, frame: bytes) -> None:
        self.sent_frames += 1
        self.sent_bytes += len(frame)

    def update_status(self) -> None:
        self.stats_var.set(f"已发送 {self.sent_frames:,} 帧 / {self.sent_bytes:,} 字节")
        self.root.after(100, self.update_status)

    def write_log(self, message: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.log.configure(state="normal")
        self.log.insert("end", f"[{timestamp}] {message}\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def close(self) -> None:
        self.stop_event.set()
        self.close_serial()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    Gsa200HostApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()

