# UART 使用资源

本文档按当前源码整理，重点区分“驱动已编译”“串口已初始化”和“已有业务使用”三个状态。

## 接口与引脚

| 外设 | 接口 | 通道 | MCU 引脚 | 当前业务 |
|---|---|---:|---|---|
| USART2 | J5 | 1 | PA2 / PA3 | GSA200 陀螺仪 |
| UART7 | J6 | 2 | PE8 / PE7 | 当前无业务，预留 |
| USART3 | J7 | 3 | PB10 / PB11 | PCU 终端和 `printf` |
| USART6 | J8 | 4 | PC6 / PC7 | 串口测试 |
| USART1 | J9 | 5 | PA9 / PA10 | 串口烧录引导 |
| UART4 | J10 | 6 | PC10 / PC11 | PCU 伺服驱动器 |
| UART5 | J2 | 7 | PC12 / PD2 | 未初始化，预留 |
| UART8 | J3 | 8 | PE1 / PE0 | 未初始化，预留 |

## 当前使用状态

| 外设 | 状态 | 初始化位置 | 波特率 | 收发/协议 | DMA 与中断资源 | 业务说明 |
|---|---|---|---:|---|---|---|
| **USART1 / J9** | 已使用 | `boot_init()` -> `uart1_init()` | 115200 | DMA 收发，空闲中断接收 | DMA1 Stream0 TX、Stream1 RX；`USART1_IRQn` | 接收连续 3 个 `0x7F` 后，校验 ROM Bootloader 向量并跳转；保留 J9 串口烧录功能 |
| **USART2 / J5** | 已使用 | `main()` -> `uart2_init()` | 初始 460800；无有效帧时轮换 921600/115200/230400 | DMA 循环接收，主循环按字节读取 | DMA1 Stream2 TX、Stream3 RX；无空闲中断业务 | GSA200 32 字节帧接收、校验、陀螺仪/加速度/温度/采样计数解析和丢帧统计 |
| **USART3 / J7** | 已使用 | `main()` -> `uart3_init()` | 921600 | `printf` 阻塞发送；DMA 接收+空闲中断 | DMA1 Stream4 TX、Stream5 RX；`USART3_IRQn` | 终端输入和调试输出共用；支持 `help/status/set/enable/disable/stop/clear/zero` |
| **UART4 / J10** | 已使用 | `pcu_init()` -> `uart4_init()` | 921600 | DMA 收发，空闲中断接收 | DMA1 Stream6 TX、Stream7 RX；`UART4_IRQn` | PCU 7 字节状态反馈解析，以及电压开环、电流闭环、自动找零、清故障和失能命令发送 |
| **USART6 / J8** | 已使用 | `j8_uart_test_init()` -> `uart6_init()` | 115200 | DMA 收发，空闲中断接收 | DMA2 Stream2 TX、Stream3 RX；`USART6_IRQn` | `j8_uart_test_poll()` 每 2 秒发送一次 `NB\r\n`，用于接口/连线测试；当前没有业务接收解析 |
| **UART7 / J6** | 仅初始化 | `main()` -> `uart7_init()` | 921600 | DMA 收发能力已配置 | DMA2 Stream4 TX、Stream5 RX；`UART7_IRQn` | 当前没有模块调用 `uart7_transmit()` 或读取 `uart7_rx`，属于预留接口 |
| **UART5 / J2** | 未初始化 | 无 | - | 驱动存在 | DMA2 Stream0 TX、Stream1 RX；`UART5_IRQn` | `bsp_uart.c/.h` 提供驱动，但 `main()` 未调用 `uart5_init()`，当前不可视为运行中资源 |
| **UART8 / J3** | 未初始化 | 无 | - | 驱动存在 | DMA2 Stream6 TX、Stream7 RX；`UART8_IRQn` | `bsp_uart.c/.h` 提供驱动，但 `main()` 未调用 `uart8_init()`，当前未使用 |

## 启动顺序和主循环调用

`App/main.c` 当前初始化顺序为：

```text
uart2_init(460800)
gsa200_init()
uart3_init(921600)
pcu_terminal_init()
pcu_init(921600, 0x55)   // 内部初始化 UART4
uart7_init(921600)
j8_uart_test_init()      // 内部初始化 USART6
boot_init()               // 内部初始化 USART1，必须在中断优先级分组前
```

主循环对应的业务轮询为：

```text
gsa200_poll()       -> USART2/J5
pcu_poll()          -> UART4/J10
pcu_terminal_poll() -> USART3/J7
boot_poll()         -> USART1/J9
j8_uart_test_poll() -> USART6/J8
```

## DMA 缓冲区和缓存注意事项

- UART DMA 缓冲区统一放在 `0x3001xxxx` SRAM 区域，具体地址由 `Bsp/UART/bsp_uart.h` 中的 `ADDR_UARTx_TX/RX` 定义。
- USART2 使用 256 字节循环 DMA 接收缓冲区，适合 GSA200 连续高速数据流。
- 其他 UART 的接收缓冲区长度为 255 字节，采用一次 DMA + 空闲中断方式接收不定长数据。
- 当前工程开启了 D-Cache；MPU 将 DMA 使用的 SRAM 区域配置为不可缓存，避免 DMA 与 CPU 数据不一致。

## 资源边界

- 不要修改 USART1/J9 的烧录跳转逻辑。
- PCU 命令必须通过 `pcu_send_command()`，不要绕过 UART4 协议封装直接发送。
- USART3 同时承担终端输入和 `printf` 输出，新增大量日志可能影响终端交互时序。
- UART7、UART5、UART8 虽然有底层驱动或初始化入口，但在没有新增业务模块前，不应记录为“已使用”。
