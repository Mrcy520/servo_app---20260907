# UART 使用资源

本文档按当前工程源码整理，记录各串口的引脚、DMA、初始化入口和业务占用情况。

## 接口与引脚

| 外设 | 接口 | MCU 引脚 | 当前业务 |
|---|---|---|---|
| USART2 | J5 | PA2 / PA3 | GSA200 陀螺仪 |
| UART7 | J6 | PE8 / PE7 | 九通道波形数据输出 |
| USART3 | J7 | PB10 / PB11 | PCU 终端和 printf |
| USART6 | J8 | PC6 / PC7 | 串口测试 |
| USART1 | J9 | PA9 / PA10 | 串口烧录引导 |
| UART4 | J10 | PC10 / PC11 | PCU 伺服驱动器 |
| UART5 | J2 | PC12 / PD2 | 第二路 GSA200 陀螺仪 |
| UART8 | J3 | PE1 / PE0 | 当前未初始化，预留 |

## 当前使用状态

| 外设 | 初始化入口 | 波特率 | 收发方式 | DMA / 中断资源 | 业务说明 |
|---|---|---:|---|---|---|
| USART1 / J9 | boot_init() -> uart1_init() | 115200 | DMA 发送；空闲中断接收 | DMA1 Stream0 TX、Stream1 RX；USART1_IRQn | 串口烧录 ROM Bootloader 跳转和 ACK 通信 |
| USART2 / J5 | main() -> uart2_init() | 921600 | 64 字节循环 DMA；HT/TC 中断；软件环形队列读取 | DMA1 Stream2 TX、Stream3 RX；DMA1_Stream3_IRQn；USART2_IRQn 仅处理错误 | GSA200 32 字节帧接收、校验、姿态/加速度/温度/采样计数解析 |
| USART3 / J7 | main() -> uart3_init() | 921600 | DMA 收发；空闲中断接收 | DMA1 Stream4 TX、Stream5 RX；USART3_IRQn | PCU 终端输入和 printf 调试输出，共用同一串口 |
| UART4 / J10 | pcu_init() -> uart4_init() | 921600 | DMA 收发；空闲中断接收 | DMA1 Stream6 TX、Stream7 RX；UART4_IRQn | PCU 7 字节状态反馈、控制命令和故障处理 |
| USART6 / J8 | j8_uart_test_init() -> uart6_init() | 115200 | DMA 收发；空闲中断接收 | DMA2 Stream2 TX、Stream3 RX；USART6_IRQn | 每 2 秒发送一次 NB\r\n，用于接口连线测试 |
| UART7 / J6 | main() -> uart7_init() | 921600 | 九通道 20 字节帧 DMA 发送；发送前检查 DMA 和 UART TC 忙状态；RX DMA/空闲中断仅由底层初始化 | DMA2 Stream4 TX、Stream5 RX；UART7_IRQn | `waveform_uart7_send9()` 输出九通道波形；实现位于 App/waveform_uart7.c |
| UART5 / J2 | main() -> gsa200_uart5_init() | 921600 | 自定义循环 DMA 接收，主循环轮询读取；不启用 UART5 TX 和 UART5 中断 | DMA2 Stream1 RX（Stream0 TX 未被该模块启用） | 第二路 GSA200 数据接收和解析；实现位于 App/gsa200_uart5.c |
| UART8 / J3 | 未调用 uart8_init() | - | 未启用 | DMA2 Stream6 TX、Stream7 RX；UART8_IRQn | 驱动接口保留，但当前未初始化、未使用 |

## USART2 / J5：GSA200 接收链路

USART2 是当前主要的 GSA200 接收接口，初始化顺序为：

~~~
uart2_init(BAUDRATE_921600)
gsa200_init()
~~~

主循环中由以下函数消费数据：

~~~
gsa200_poll()
    -> uart2_read_byte()
    -> process_byte()
~~~

### DMA 配置

USART2 RX 使用 DMA1 Stream3：

~~~
方向：USART2 RDR -> 内存
模式：循环模式
数据宽度：字节
DMA缓冲区：64字节
缓冲区地址：0x30020000
~~~

64 字节缓冲区分为两个半区：

~~~
buffer[0..31]   前半区
buffer[32..63]  后半区
~~~

DMA1 Stream3 开启：

~~~
HT：半传输中断，提交前32字节
TC：传输完成中断，提交后32字节
~~~

中断函数为：

~~~
DMA1_Stream3_IRQHandler()
~~~

该函数位于 Bsp/UART/bsp_uart.c，并已在 Project/GCC/startup_stm32h743xx.c 的中断向量表中注册。

### 软件环形队列

DMA 中断只处理已经完成的半区，并把数据复制到软件队列：

~~~
DMA1 Stream3
    -> 64字节 DMA 缓冲区
    -> HT/TC 中断
    -> 2048字节软件环形队列
    -> uart2_read_byte()
    -> GSA200 协议解析
~~~

软件队列定义为：

~~~
#define UART2_RX_QUEUE_LEN (2048U)
~~~

环形队列保留一个空槽，因此实际可用容量为 2047 字节，可以缓存 63 个完整的 32 字节 GSA200 帧。队列满时，uart2_rx_overflow_count 增加。

uart2_read_byte() 不再直接读取 DMA NDTR，也不再直接读取 DMA 活跃缓冲区，只读取软件队列，因此不会把 DMA 尚未可靠完成的字节交给协议解析器。

## USART2 DMA 接收问题说明

原先的实现每次读取字节时通过 NDTR 计算 DMA 写索引，然后立即读取 DMA 缓冲区。实测发现，CPU 观察到写索引变化时，对应目标内存可能仍处于 DMA 写入完成的时序窗口，导致 CPU 读到旧数据；稍后暂停查看内存时，内存内容又已经正确。

在读取索引后加入 NOP 延时可以暂时缓解问题，但延时不是可靠同步机制。当前正式方案使用 DMA HT/TC 事件建立完成边界，再通过软件环形队列向主循环提供数据。

## 其他串口初始化和主循环顺序

App/main.c 当前初始化顺序：

~~~
uart2_init(BAUDRATE_921600)
gsa200_init()
gsa200_uart5_init(BAUDRATE_921600)
uart3_init(BAUDRATE_921600)
pcu_terminal_init()
pcu_init(BAUDRATE_921600, PCU_DEFAULT_DEVICE_ID)  // 内部初始化 UART4
pcu_position_init()
pcu_speed_init()
pcu_current_test_init()
uart7_init(BAUDRATE_921600)
j8_uart_test_init()                                // 内部初始化 USART6
boot_init()                                        // 内部初始化 USART1
~~~

主循环轮询顺序：

~~~
gsa200_poll()       -> USART2 / J5
gsa200_uart5_poll() -> UART5 / J2
pcu_poll()          -> UART4 / J10
pcu_current_test_poll() -> 纯电流测试模式（启用时独占 PCU 电流指令）
pcu_position_poll() -> PCU 位置控制
pcu_speed_poll()    -> PCU 速度控制
waveform_uart7_send9() -> UART7 / J6 九通道波形输出
pcu_terminal_poll() -> USART3 / J7
boot_poll()         -> USART1 / J9
j8_uart_test_poll() -> USART6 / J8
~~~

## DMA 缓冲区和缓存注意事项

- USART2 DMA 缓冲区位于 0x30020000，对应 SRAM2；MPU 将该区域配置为不可缓存，避免 DMA 和 CPU 之间的 D-Cache 不一致。
- USART2 的 DMA 缓冲区现在只有 64 字节，不能再按照旧配置当作 2048 字节数组使用。
- 其他串口仍按各自的 UARTx_DMA_RX_LEN 和空闲中断逻辑工作，不应直接套用 USART2 的 HT/TC 处理方式。
- DMA 中断处理函数中只做数据搬运和队列发布，不应加入长时间阻塞或复杂协议解析。

## 资源边界

- 不要在未确认 DMA 和中断资源的情况下重新分配 DMA Stream。
- USART2 的 DMA1 Stream3 中断向量必须保持注册，否则首次 HT/TC 中断会进入 Default_Handler。
- USART3 同时承担终端输入和 printf 输出，大量日志会影响终端交互和实时性。
- PCU 命令必须通过 pcu_send_command() 发送，不要绕过协议封装直接操作 UART4。
- UART7 已由九通道波形模块占用，不能再与其他模块共用 TX DMA2 Stream4 或 UART7 发送缓冲区。
- UART8 虽然有底层驱动和 DMA 资源，但当前未初始化、未被业务使用；新增业务前需要先更新本表。

## UART7 / J6：九通道波形输出

UART7 当前由 `App/waveform_uart7.c` 负责发送二进制波形帧，主循环在速度环或纯电流测试模式产生新控制量后调用：

~~~
waveform_uart7_send9(waveform_values)
~~~

帧格式如下：

~~~
AA | Ch1_H Ch1_L | Ch2_H Ch2_L | ... | Ch9_H Ch9_L | checksum
~~~

- 总长度：20 字节。
- 帧头：`0xAA`。
- 每个通道：有符号 16 位整数，大端发送。
- 编码：`raw = round(value * 100)`，可表示约 `-327.68~327.67`；超出范围饱和。
- 校验：前 19 字节累加和的低 8 位。
- 未使用通道由主函数显式填充为 `0.0f`，每帧清零。
- 发送函数在 DMA2 Stream4 或 UART7 最后一个停止位尚未完成时返回 `WAVEFORM_UART7_BUSY`，不会覆盖上一帧。

当前主函数通道映射：

| 通道 | 当前数据 |
|---:|---|
| Ch1 | 纯电流测试指令 `pcu_current_test_command_a`；未启用测试时为速度环电流指令 `pcu_speed_current_a` |
| Ch2 | GSA200 原始 X 轴角速度 `gsa200_gyro_x_dps` |
| Ch3 | 零偏校正后的 X 轴角速度 `gsa200_gyro_x_corrected_dps` |
| Ch4 | 陀螺仪换算速度 `pcu_speed_gyro_rpm` |
| Ch5 | 速度环实际选用的反馈 `pcu_speed_feedback_rpm` |
| Ch6~Ch9 | `0.0f` |

UART7 当前只使用 TX 发送业务；虽然底层同时初始化了 RX DMA 和空闲中断，但工程没有主循环接收解析模块，因此不要把 UART7 RX 当作已实现的命令通道。

## 调试建议

调试 DMA 和高速 UART 时建议暂时关闭 Cortex-Debug Live Watch，避免调试器周期性读取大量变量改变实时运行时序。重点观察：

~~~
uart2_rx_overflow_count
uart2_rx_error_count
gsa200_valid_frames
gsa200_checksum_errors
gsa200_format_errors
gsa200_dropped_frames
~~~

正常接收时，gsa200_valid_frames 应持续增加，uart2_rx_overflow_count 和 uart2_rx_error_count 不应持续增加。
