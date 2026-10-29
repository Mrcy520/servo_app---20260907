# 陀螺仪 DMA 接收问题总结

## 1. 问题背景

工程通过 USART2 接收 GSA200 陀螺仪数据，使用 DMA 将 USART2 的接收数据搬运到内存，再由主循环中的 `gsa200_poll()` 逐字节解析协议。

当前相关资源如下：

| 项目 | 配置 |
| --- | --- |
| MCU | STM32H743 |
| 外设 | USART2 |
| DMA | DMA1 Stream3 |
| DMA方向 | 外设到内存 |
| DMA模式 | 循环模式 |
| DMA数据宽度 | 字节 |
| GSA200帧长度 | 32字节 |
| DMA缓冲区 | 64字节，分为两个32字节半区 |
| 软件队列 | 2048字节 |
| DMA接收地址 | `0x30020000`，SRAM2不可缓存区域 |

## 2. 故障现象

在原来的实现中，CPU 每次调用 `uart2_read_byte()` 时读取 DMA 的 `NDTR`，根据剩余传输数量计算 DMA 写索引：

```c
write_index = UART2_DMA_RX_LEN - LL_DMA_GetDataLength(...);
```

随后判断是否存在新数据，如果存在就直接读取 DMA 缓冲区：

```c
*data = dma_buffer[read_index];
```

实际运行时出现以下现象：

1. 陀螺仪发送的数据帧偶尔校验失败或解析异常；
2. 暂停程序查看 DMA 缓冲区时，内存中的数据又是正确的；
3. 在读取 `write_index` 后加入若干 `NOP` 延时，数据错误明显减少或消失；
4. 删除延时后问题重新出现。

## 3. 原因定位

### 3.1 原有读取流程存在时序窗口

原有流程是：

```text
CPU读取NDTR
    ↓
计算write_index
    ↓
判断有新数据
    ↓
CPU立即读取DMA缓冲区
```

根据实际测试，当前 STM32H743、DMA 和总线配置下，CPU 观察到 `NDTR`/写索引发生变化时，对应的 DMA 目标内存字节还没有形成可靠的 CPU 可读结果。于是 CPU 可能读取到该地址原来残留的旧数据。

稍后 DMA 完成内存写入后，再暂停查看内存，看到的就是正确数据。这解释了“运行时读错、暂停后查看内存正确”的现象。

### 3.2 `NDTR` 不是单个内存字节的安全发布标志

`NDTR` 主要表示 DMA 剩余传输数量。直接通过 `NDTR` 推导当前写索引，并不等同于向 CPU 发布“这个索引之前的每一个目标内存字节都已经可以安全读取”的完成事件。

连续读取两次 `NDTR`、增加 `volatile`、加入 `__DMB()` 或 `__NOP()` 只能缩小问题窗口，不能建立严格的 DMA 完成同步关系。

### 3.3 NOP 延时只是验证手段

在 `write_index` 判断和内存读取之间加入 NOP 后问题改善，说明延时给了 DMA 完成内存写入的时间，因此支持上述时序竞争判断。

但 NOP 延时不是可靠修复方案，原因包括：

- 延时时间随主频、编译优化和总线负载变化；
- 中断、缓存和其他 DMA 活动会改变实际时序；
- 延时放在逐字节读取路径中，会降低 CPU 消费数据的速度；
- 当 CPU 消费速度低于串口接收速度时，仍可能发生环形缓冲区覆盖。

## 4. 正式解决方案

采用“小块 DMA + HT/TC + 软件环形队列”架构。

### 4.1 DMA 使用两个明确完成的半区

DMA 缓冲区改为 64 字节：

```text
buffer[0..31]   前半区
buffer[32..63]  后半区
```

DMA 使用循环模式持续接收：

```text
DMA写前半区
    ↓
半传输中断 HT
    ↓
前32字节已完成，CPU提交前半区

DMA写后半区
    ↓
传输完成中断 TC
    ↓
后32字节已完成，CPU提交后半区
```

CPU 不再通过 `NDTR` 追踪最新字节，而只读取 HT/TC 已确认完成的半区。

### 4.2 使用软件环形队列解耦 DMA 和协议解析

DMA 中断将已完成的 32 字节复制到 2048 字节软件环形队列：

```text
USART2
  ↓
DMA1 Stream3，64字节循环缓冲区
  ↓ HT/TC中断
2048字节软件环形队列
  ↓
uart2_read_byte()
  ↓
gsa200_poll()
  ↓
GSA200协议解析
```

`uart2_read_byte()` 现在只读取软件队列，不再读取 DMA `NDTR`，也不再直接读取 DMA 正在使用的内存区域。

### 4.3 软件队列的发布顺序

生产者必须先写入队列数据，再更新写索引：

```c
queue[write_index] = value;
__DMB();
queue_write_index = next_index;
```

这样主循环观察到写索引变化时，对应的数据已经被写入软件队列。

## 5. 工程中的具体修改

### 5.1 DMA和队列容量

在 [bsp_uart.h](Bsp/UART/bsp_uart.h) 中：

```c
#define UART2_DMA_RX_LEN   (64U)
#define UART2_RX_QUEUE_LEN (2048U)
```

### 5.2 DMA中断

在 [bsp_uart.c](Bsp/UART/bsp_uart.c) 中开启：

```c
LL_DMA_EnableIT_HT(DMA1, LL_DMA_STREAM_3);
LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_3);
```

并实现：

```c
void DMA1_Stream3_IRQHandler(void)
```

HT 中断提交前 32 字节，TC 中断提交后 32 字节。

### 5.3 GCC中断向量

工程使用自定义 GCC 启动文件 [startup_stm32h743xx.c](Project/GCC/startup_stm32h743xx.c)。该文件原来将未显式注册的外部中断全部指向 `Default_Handler`，因此即使 `bsp_uart.c` 中实现了 `DMA1_Stream3_IRQHandler()`，DMA 中断仍会进入死循环。

已补充：

```c
extern void DMA1_Stream3_IRQHandler(void);
```

以及向量表项：

```c
[16U + DMA1_Stream3_IRQn] =
    (uintptr_t)DMA1_Stream3_IRQHandler,
```

## 6. 队列容量和溢出

软件队列长度为 2048 字节。环形队列保留一个空槽区分“空”和“满”，因此实际可用容量为：

```text
2048 - 1 = 2047字节
```

GSA200 每帧 32 字节，因此可以缓存：

```text
2047 / 32 = 63个完整帧
```

队列满时，`uart2_rx_overflow_count` 会增加。上板调试时应确认该计数保持为 0；如果增加，说明主循环处理速度不足，或者中断被长时间屏蔽。

## 7. 为什么没有直接使用 DMA TC 等待整块 2048 字节

如果继续使用 2048 字节 DMA 缓冲区，半传输/传输完成通知分别只会在 1024 字节完成时产生，数据处理延迟过大。

64 字节 DMA 缓冲区的半区为 32 字节，和 GSA200 帧长度一致，在 921600 波特率下每 32 字节约 347 微秒产生一次提交，延迟较小，同时保留了明确的 DMA 完成边界。

DMA 半区边界不要求和协议帧边界一致。即使半区中包含半帧，软件队列仍按字节传递，GSA200 解析器会继续根据 `AA 55` 帧头和长度字段完成协议同步。

## 8. 验证结果

修改后已使用当前 ARM GCC 对以下文件做语法检查：

```text
Bsp/UART/bsp_uart.c
App/gsa200.c
App/main.c
```

并完成完整 GCC 固件构建，生成：

```text
build/gcc/servo_app.elf
build/gcc/servo_app.hex
```

最终 ELF 中已确认存在：

```text
DMA1_Stream3_IRQHandler
```

因此 DMA1 Stream3 不会再因为启动文件未注册而进入 `Default_Handler`。

## 9. 上板测试检查项

1. 烧录最新的 `build/gcc/servo_app.hex`；
2. 确认 USART2 波特率和陀螺仪输出波特率一致；
3. 确认 `uart2_rx_overflow_count == 0`；
4. 观察 `gsa200_checksum_errors` 是否持续增加；
5. 观察 `gsa200_valid_frames` 是否持续增加；
6. 确认程序不再进入 `Default_Handler`；
7. 如果仍进入 `Default_Handler`，检查是否还有其他未注册中断；
8. 如果队列溢出，减少主循环阻塞时间或增大 `UART2_RX_QUEUE_LEN`，并保持其为 2 的幂。

## 10. 结论

本问题的核心不是 GSA200 校验算法，而是 CPU 通过 `NDTR` 观察 DMA 写指针后，立即读取 DMA 目标内存，导致 CPU 可能在目标字节可靠完成前读取旧数据。

加入 NOP 延时可以暂时掩盖该问题，但不能提供稳定保证。最终通过 DMA 半传输/传输完成事件建立数据完成边界，再由软件环形队列向协议解析器提供数据，解决了 CPU 直接读取 DMA 活跃缓冲区的时序风险。
