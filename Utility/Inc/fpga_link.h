/**
 ******************************************************************************
 * @file    fpga_link.h
 * @brief   FPGA <-> MCU 内部接口 · 链路层（HAL / SPI3 / DMA / EXTI）
 *
 *  数据流（与参考工程 test_communcation 的 LL 实现等价，改用 HAL）：
 *
 *    PA11 下降沿（FPGA 16 kHz 帧同步，CubeMX 中的 nInt_Pin）
 *        -> HAL_GPIO_EXTI_Callback
 *        -> FpgaLink_OnSync()        快照活动发送帧，启动一次 32B 全双工 DMA 交换
 *        -> HAL_SPI_TxRxCpltCallback 收满 32B：滑窗重同步 -> 解帧 -> 更新最新响应
 *
 *  时序余量：SPI3 = APB1(36 MHz) / 4 = 9 MHz，32 字节 = 256 bit ≈ 28.4 µs，
 *            16 kHz 周期 62.5 µs，余量约 55%。
 *            文档写"时钟 30M"，超出 STM32F302 的 SPI 上限（APB1/2 = 18 MHz），
 *            故沿用当前 CubeMX 配置（DIV4），以 FPGA 侧能正常出帧为准。
 *
 *  丢帧判据（现场读 [ST] 行的 spi=/sync= 两项）：
 *    busy_max < sync_min  → 正常，ovr 不增长。
 *    busy_max ≈ sync_min  → 临界：ovr 按"偶尔错过"的比例增长，必须拉开余量。
 *    busy_max > sync_min  → 周期性丢弃，等效于按 ceil(busy/sync) 分频采样。
 *  两个 µs 值都按 SystemCoreClock 换算，而 **sync 实测值本身就是时钟校准基准**：
 *  16 kHz 对应 62.5 µs，读到 62~63 µs 说明主频与配置一致；
 *  若读到 ~21 µs，说明实际主频只有配置值的 1/3（HSE 与 HSE_VALUE 不符时常见），
 *  这时 busy 的真实值要按 ×3 反算——先修时钟，再谈余量。
 ******************************************************************************
 */

#ifndef __FPGA_LINK_H
#define __FPGA_LINK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "fpga_proto.h"

/* 链路存活判据：连续多少个 16 kHz 周期没有有效响应即判为断链
 * 80 周期 = 5 ms，远大于单帧耗时，又能快速发现 FPGA 侧停摆 */
#define FPGA_LINK_TIMEOUT_CYCLES 80u

/* 统计量 ------------------------------------------------------------------ */
extern volatile uint32_t g_fpga_cycle;   /* 已发起的交换周期数        */
extern volatile uint32_t g_fpga_rx_cnt;  /* 有效响应帧数              */
extern volatile uint32_t g_fpga_err;     /* 帧头/状态非法、DMA 错误数 */
extern volatile uint32_t g_fpga_slip;    /* 字节滑移重同步次数        */
extern volatile uint32_t g_fpga_overrun; /* 上周期未完成导致跳过的周期 */

/* 非实时请求的应答追踪：应用层用 ack_cnt 的变化 + ack_state 判定"我方请求已回应"。
 * 之所以不直接读最新帧状态，是因为 16 kHz 下应答帧瞬间就会被后续 0x01 帧覆盖。 */
extern volatile uint32_t g_fpga_ack_cnt;  /* 非 0x01 响应帧累计数     */
extern volatile uint8_t g_fpga_ack_state; /* 最近一次非 0x01 响应状态 */

/* SPI 状态机被强制复位的次数（自愈）。
 * "忙"持续超过 500 µs 或 HAL 发起失败时触发；正常应为 0。
 * 该值持续增长说明存在系统性异常（DMA 中断丢失 / SPI 状态机卡死），
 * 而不是偶发干扰。 */
extern volatile uint32_t g_fpga_recover;

/* 最近一次"发起失败"的现场快照（按字节打包）：
 *   [7:0]   HAL_SPI_TransmitReceive_DMA 返回码  (0=OK 1=ERROR 2=BUSY 3=TIMEOUT)
 *   [15:8]  hspi3.State      (0=RESET 1=READY 2=BUSY_TX 3=BUSY_RX 4=BUSY_TX_RX …)
 *   [23:16] hspi3.Lock       (0=UNLOCKED 1=LOCKED)
 *   [31:24] hspi3.hdmarx->State
 * [ST] 行里的 e=0x........ 就是这个值，用来定位 HAL 为什么拒绝发起。 */
extern volatile uint32_t g_fpga_err_detail;

/* 时序诊断（单位 µs，基于 DWT 周期计数器，72 MHz = 1 µs/72 cycles）：
 *   sync_*  = 相邻两次同步沿的间隔（应为 62.5 µs，16 kHz）
 *   busy_*  = 从发起 DMA 到 SPI 交还 READY 的占用时长（决定会不会丢帧）
 * 判据：busy_max 必须明显小于 sync_min，否则周期必然被跳过（g_fpga_overrun 增长）。
 * 复位后取到的 min 是 0xFFFFFFFF，表示尚未采到样本。 */
extern volatile uint32_t g_fpga_sync_us_min;
extern volatile uint32_t g_fpga_sync_us_max;
extern volatile uint32_t g_fpga_busy_us_min;
extern volatile uint32_t g_fpga_busy_us_max;

/* 发起阶段纯软件开销：s_ts_start → HAL_SPI_TransmitReceive_DMA 返回。
 * 判读（配合 busy）：busy - hal ≈ 传输 + 中断延迟。
 *   hal ≈ 1~3 µs 且 busy ≈ 51 µs  → 传输本身慢，SCK 实际不到 9 MHz；
 *   hal ≥ 15 µs                   → 慢在 -O0 下 HAL 调用自身，需绕开 HAL 或局部 -O2。 */
extern volatile uint32_t g_fpga_hal_us_min;
extern volatile uint32_t g_fpga_hal_us_max;

/* 被拒绝时"距上次发起过了多久"。
 *   ≈60~62 → 上一次交换占满周期，窗口太长（继续压软件开销有效）；
 *   ≈20~30 → 沿在交换未完成时就到，沿间隔本身短于 62.5 µs（压窗口无效）。 */
extern volatile uint32_t g_fpga_rej_us_min;
extern volatile uint32_t g_fpga_rej_us_max;

/* nibble 交换纠正次数。恒为 0 = FPGA 输出相位正常；
 * 持续增长 = 该 FPGA 本次上电工作在"每字节高低半字节互换"的相位，
 * 数据已被纠正、可正常使用，但这个相位本身应由 FPGA 侧修掉。 */
extern volatile uint32_t g_fpga_nib_swap;

/* 位相位判定次数。上电后应很快稳定为 1 次；
 * 若持续增长，说明 FPGA 的 MISO 位相位在反复变化（该查 FPGA 侧）。 */
extern volatile uint32_t g_fpga_phase_cnt;

/* 同步沿间隔直方图：6 档，单位 µs
 *   [0] <20  [1] 20~40  [2] 40~56  [3] 56~72  [4] 72~88  [5] >=88
 * 取走后清零，因此每次得到的都是"上一个统计窗口"的分布。
 * 判读：稳态 16 kHz 应几乎全部落在 [3]。若 [0]/[1] 占比可观，
 * 说明同步沿本身不均匀（FPGA 抖动或中断延迟抖动），
 * 那时压缩 SPI 窗口也无法做到零丢帧，需要 FPGA 侧一起看。 */
#define FPGA_SYNC_HIST_N 6
uint8_t FpgaLink_TakeSyncHist(uint16_t* out);

/* SPI3 实际 SCK 频率（kHz），由 PCLK1 与 CR1.BR 现场算出：
 * 32 字节 = 256 bit，理论占用 = 256 / SCK。9 MHz → 28.4 µs。
 * 若这里显示的不是 9000，说明分频或时钟源与预期不符，先改这里。 */
extern volatile uint32_t g_fpga_sck_khz;

/* 接口 -------------------------------------------------------------------- */
void FpgaLink_Init(void);

/* 更新下发命令：写入非活动双缓冲，下一次帧同步自动生效 */
void FpgaLink_SetCommand(const fpga_cmd_t* cmd);

/* 读取最新一帧响应（临界区拷贝）：
 * 返回 1 = 至少收到过一帧有效数据，0 = 尚无有效帧 */
uint8_t FpgaLink_GetRx(fpga_rx_t* out);

/* 读取最近一帧被接受的**原始接收字节**（FPGA_FRAME_LEN 字节，已做过滑移重同步）：
 * 返回 1 = 有数据，0 = 尚无有效帧。
 * 用途：现场核对量纲/字节序时直接看线上字节，而不是看解码后的结构体
 *       （解码结构体重新编码出来的字节只能证明代码自洽，不能证明口径正确）。 */
uint8_t FpgaLink_GetRawRx(uint8_t* out);

/* 读取最近一个 DMA 窗口的原始字节（FPGA_FRAME_LEN 字节），**不论是否解出帧头**：
 * 返回 1 = 已有窗口，0 = 还没发生过一次交换。
 * 用途：rx=0、slip 疯涨时，判断"FPGA 根本没应答（全 0x00/0xFF）"
 *       还是"有数据但帧头错位（含 0xAA55 但不在 0 位）"。 */
uint8_t FpgaLink_GetLastWindow(uint8_t* out);

/* 读取最近一次真正发到 MOSI 上的 32 字节（与 [WIN] 对照即可判断：
 *   两者逐字节相同 → 回环（MISO 没有由 FPGA 驱动，被 MOSI 串扰/短接）；
 *   不同          → FPGA 确实在驱动 MISO，但帧格式/字节序与我们预期不符。 */
uint8_t FpgaLink_GetLastTx(uint8_t* out);

/* 链路是否存活：有新帧且未超时 */
uint8_t FpgaLink_Alive(void);

#ifdef __cplusplus
}
#endif

#endif /* __FPGA_LINK_H */
