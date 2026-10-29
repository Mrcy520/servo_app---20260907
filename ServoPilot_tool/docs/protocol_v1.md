# ServoPilot Protocol V1

## 串口参数

```text
USART3 / J7
921600 baud
8 data bits, no parity, 1 stop bit
no flow control
```

## 帧格式

```text
A5 5A | VER | TYPE | SEQ | CMD | LEN | PAYLOAD | CRC16
  2      1      1     2     2     2      N        2 bytes
```

- 多字节整数和IEEE754 `float32`均采用小端；
- `TYPE=1`请求，`TYPE=2`应答，`TYPE=3`主动事件；
- Payload最大256字节；
- CRC为CRC16-CCITT-FALSE，多项式`0x1021`、初值`0xFFFF`；
- CRC计算范围从`VER`到Payload最后一个字节，CRC低字节先发送。

## 应答Payload

```text
RESULT:u8 | DETAIL:u8 | DATA...
```

| RESULT | 含义 |
|---:|---|
| 0 | 成功 |
| 1 | 不支持的协议版本 |
| 2 | 未知命令 |
| 3 | Payload长度错误 |
| 4 | 参数非法 |
| 5 | 当前状态不允许 |
| 6 | 无PCU反馈 |
| 7 | PCU故障，DETAIL为PCU状态字 |
| 8 | 陀螺仪未校准 |
| 9 | 无陀螺仪数据 |
| 10 | 设备忙 |
| 11 | 命令发送失败 |
| 12 | 纯电流测试正在运行 |
| 13 | 请求序号冲突 |
| 14 | 内部错误 |
| 15 | 反馈超时 |

## 命令编号

| CMD | 名称 | 请求Payload |
|---:|---|---|
| `0001` | HELLO | `client_nonce:u32` |
| `0002` | PING | 无 |
| `0003` | GET_CAPABILITIES | 无 |
| `0010` | GET_STATUS | `section_mask:u32` |
| `0011` | SET_TELEMETRY | `mask:u32, period_ms:u16` |
| `0012` | SET_WATCHDOG | `timeout_ms:u16` |
| `0100` | SET_MANUAL_COMMAND | `mode:u8, raw:i16` |
| `0101` | MANUAL_ENABLE | 无 |
| `0102` | OUTPUT_DISABLE | 无 |
| `0103` | CLEAR_FAULT | 无 |
| `0104` | AUTO_ZERO | `raw:i16` |
| `0200` | GYRO_BIAS_START | 无 |
| `0300` | POSITION_MOVE_RELATIVE | `degree:f32` |
| `0301` | POSITION_SET_END_MODE | `mode:u8` |
| `0302` | POSITION_SET_KP | `kp:f32` |
| `0303` | POSITION_SET_MAX_SPEED | `rpm:f32` |
| `0304` | POSITION_STOP | 无 |
| `0400` | SPEED_SET_TARGET | `rpm:f32` |
| `0401` | SPEED_SET_FEEDBACK | `source:u8` |
| `0402` | SPEED_START | 无 |
| `0403` | SPEED_STOP | 无 |
| `0404` | SPEED_SET_SINE | `base:f32, amplitude:f32, frequency:f32` |
| `0405` | SPEED_SINE_START | 无 |
| `0406` | SPEED_SINE_STOP | 无 |
| `0407` | SPEED_SET_KP | `kp:f32` |
| `0408` | SPEED_SET_KI | `ki:f32` |
| `0409` | SPEED_SET_MAX_CURRENT | `ampere:f32` |

手动模式：`1=电压开环`、`2=电流闭环`。反馈源：`0=PCU差分`、`1=陀螺仪`。位置结束模式：`0=到位失能`、`1=保持`。

## HELLO成功应答DATA

```text
protocol_major:u8, protocol_minor:u8,
firmware_major:u8, firmware_minor:u8, firmware_patch:u8,
reserved:u8, max_payload:u16, capabilities:u32,
boot_id:u32, server_nonce:u32
```

除应答开头的`RESULT, DETAIL`外，HELLO DATA固定为20字节。

## GET_STATUS

请求掩码：bit0 PCU、bit1手动、bit2位置、bit3速度、bit4陀螺仪、bit5通信；掩码0表示全部。

返回DATA由若干状态区组成：

```text
SECTION_ID:u8 | SECTION_LEN:u16 | SECTION_DATA
```

### PCU区 `01`，39字节

```text
valid:u8, status:u8, device_id:u8, reserved:u8,
valid_frames:u32, angle_raw:u32, angle_deg:f32,
checksum_errors:u32, format_errors:u32,
tx_frames:u32, tx_errors:u32, last_tx_frame[7]
```

### 手动区 `02`，4字节

```text
mode:u8, output_requested:u8, raw:i16
```

### 位置区 `03`，28字节

```text
state:u8, end_mode:u8, active:u8, reserved:u8,
current_mdeg:i32, target_mdeg:i32, error_mdeg:i32,
kp:f32, max_speed_rpm:f32, speed_reference_rpm:f32
```

### 速度区 `04`，80字节

```text
state:u8, loop_mode:u8, sine_active:u8, feedback_source:u8,
target_rpm:f32, reference_rpm:f32, feedback_rpm:f32,
error_rpm:f32, pcu_differential_rpm:f32, gyro_rpm:f32,
feedback_age_ms:u32,
sine_base_rpm:f32, sine_amplitude_rpm:f32, sine_frequency_hz:f32,
kp:f32, ki:f32, max_current_a:f32,
current_command_a:f32, output_norm:f32,
current_raw:i16, reserved:u16,
control_updates:u32, feedback_timeouts:u32, command_errors:u32
```

### 陀螺仪区 `05`，44字节

```text
data_valid:u8, bias_state:u8, bias_done:u8, reserved:u8,
valid_frames:u32,
gyro_x_raw_dps:f32, gyro_x_corrected_dps:f32, gyro_x_bias_dps:f32,
calibration_samples:u32, calibration_total:u32,
checksum_errors:u32, format_errors:u32,
dropped_frames:u32, uart_overflow_count:u32
```

### 通信区 `06`，16字节

```text
command_count:u32, error_count:u32, uptime_ms:u32, boot_id:u32
```

STM32实现所有状态区时必须逐字段编码，不能直接发送可能含有填充字节的C结构体。

## 重发和重复执行保护

- 上位机对超时请求使用相同SEQ和相同完整帧重发；
- STM32必须缓存最近至少16个已执行请求；
- 收到相同SEQ和CMD时返回原应答，不再次执行；
- 相同SEQ对应不同CMD时返回`SEQUENCE_CONFLICT`；
- 这对相对位置运动和自动找零等非幂等命令是强制要求。

## 安全要求

- `OUTPUT_DISABLE`必须停止速度环、位置环、正弦速度和纯电流测试，然后失能PCU；
- `AUTO_ZERO`必须在所有控制模式停止后执行；
- 通信看门狗超时后必须执行全局停止；
- 进入二进制协议模式后，USART3不能混入普通`printf`文本。

