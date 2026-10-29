from __future__ import annotations

import queue
import random
import threading
import time
from dataclasses import dataclass
from typing import Any

from servopilot.protocol.codec import Frame, FrameDecoder, encode_frame
from servopilot.protocol.constants import COMMAND_NAMES, Command, FrameType

try:
    import serial
    from serial import SerialException
    from serial.tools import list_ports
except ImportError:  # allows protocol tests without pyserial installed
    serial = None
    SerialException = OSError
    list_ports = None


@dataclass(slots=True)
class ClientEvent:
    kind: str
    data: Any = None


@dataclass(slots=True)
class PendingRequest:
    sequence: int
    command: int
    packet: bytes
    sent_at: float
    deadline: float
    timeout: float
    retries_left: int
    attempts: int = 1


class SerialProtocolClient:
    """Threaded serial transport with request tracking and retry support."""

    def __init__(self) -> None:
        self.events: queue.Queue[ClientEvent] = queue.Queue()
        self._serial = None
        self._decoder = FrameDecoder()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._write_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: dict[int, PendingRequest] = {}
        self._sequence = random.randint(1, 0xFFFE)
        self.port = ""
        self.baudrate = 921600

    @staticmethod
    def available_ports() -> list[str]:
        if list_ports is None:
            return []
        return [item.device for item in list_ports.comports()]

    @property
    def is_open(self) -> bool:
        return bool(self._serial is not None and self._serial.is_open)

    @property
    def pending_count(self) -> int:
        with self._pending_lock:
            return len(self._pending)

    def command_pending(self, command: Command | int) -> bool:
        command_value = int(command)
        with self._pending_lock:
            return any(item.command == command_value for item in self._pending.values())

    def open(self, port: str, baudrate: int = 921600) -> None:
        if serial is None:
            raise RuntimeError("未安装pyserial，请先安装requirements.txt中的依赖")
        self.close()
        self.port = port
        self.baudrate = baudrate
        self._decoder.reset()
        self._stop.clear()
        self._serial = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.02,
            write_timeout=0.25,
        )
        self._serial.reset_input_buffer()
        self._serial.reset_output_buffer()
        self._thread = threading.Thread(target=self._read_loop, daemon=True, name="ServoPilotSerial")
        self._thread.start()
        self.events.put(ClientEvent("connected", {"port": port, "baudrate": baudrate}))

    def close(self) -> None:
        self._stop.set()
        current_thread = threading.current_thread()
        if self._thread is not None and self._thread is not current_thread:
            self._thread.join(timeout=0.5)
        self._thread = None
        if self._serial is not None:
            try:
                self._serial.close()
            except Exception:
                pass
            self._serial = None
        with self._pending_lock:
            self._pending.clear()

    def next_sequence(self) -> int:
        with self._pending_lock:
            for _ in range(0xFFFF):
                self._sequence = (self._sequence + 1) & 0xFFFF
                if self._sequence == 0:
                    self._sequence = 1
                if self._sequence not in self._pending:
                    return self._sequence
        raise RuntimeError("没有可用请求序号")

    def send_request(
        self,
        command: Command | int,
        payload: bytes = b"",
        *,
        timeout: float = 0.30,
        retries: int = 2,
    ) -> int:
        if not self.is_open:
            raise RuntimeError("串口尚未连接")
        sequence = self.next_sequence()
        command_value = int(command)
        packet = encode_frame(FrameType.REQUEST, sequence, command_value, payload)
        now = time.monotonic()
        pending = PendingRequest(
            sequence=sequence,
            command=command_value,
            packet=packet,
            sent_at=now,
            deadline=now + timeout,
            timeout=timeout,
            retries_left=retries,
        )
        with self._pending_lock:
            self._pending[sequence] = pending
        try:
            self._write(packet)
        except Exception:
            with self._pending_lock:
                self._pending.pop(sequence, None)
            raise
        self.events.put(ClientEvent("tx", {"sequence": sequence, "command": command_value, "packet": packet}))
        return sequence

    def _write(self, packet: bytes) -> None:
        if not self.is_open:
            raise RuntimeError("串口已经断开")
        with self._write_lock:
            written = self._serial.write(packet)
            if written != len(packet):
                raise SerialException(f"串口只发送了{written}/{len(packet)}字节")

    def _read_loop(self) -> None:
        try:
            while not self._stop.is_set() and self.is_open:
                waiting = self._serial.in_waiting
                chunk = self._serial.read(waiting if waiting > 0 else 1)
                if chunk:
                    for frame in self._decoder.feed(chunk):
                        self._handle_frame(frame)
                self._check_timeouts()
        except (SerialException, OSError) as exc:
            if not self._stop.is_set():
                self.events.put(ClientEvent("error", f"串口通信异常：{exc}"))
        finally:
            if not self._stop.is_set():
                self.events.put(ClientEvent("disconnected", None))

    def _handle_frame(self, frame: Frame) -> None:
        request = None
        if frame.frame_type == FrameType.RESPONSE:
            with self._pending_lock:
                request = self._pending.pop(frame.sequence, None)
        self.events.put(
            ClientEvent(
                "frame",
                {
                    "frame": frame,
                    "request": request,
                    "received_at": time.monotonic(),
                },
            )
        )

    def _check_timeouts(self) -> None:
        now = time.monotonic()
        retries: list[PendingRequest] = []
        timed_out: list[PendingRequest] = []
        with self._pending_lock:
            for sequence, request in list(self._pending.items()):
                if now < request.deadline:
                    continue
                if request.retries_left > 0:
                    request.retries_left -= 1
                    request.attempts += 1
                    request.sent_at = now
                    request.deadline = now + request.timeout
                    retries.append(request)
                else:
                    timed_out.append(self._pending.pop(sequence))

        for request in retries:
            try:
                self._write(request.packet)
                self.events.put(ClientEvent("retry", request))
            except Exception as exc:
                self.events.put(ClientEvent("error", f"重发失败：{exc}"))
        for request in timed_out:
            self.events.put(ClientEvent("timeout", request))

    @staticmethod
    def command_name(command: int) -> str:
        return COMMAND_NAMES.get(command, f"0x{command:04X}")

