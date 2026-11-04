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

/* 接口 -------------------------------------------------------------------- */
void FpgaLink_Init(void);

/* 更新下发命令：写入非活动双缓冲，下一次帧同步自动生效 */
void FpgaLink_SetCommand(const fpga_cmd_t* cmd);

/* 读取最新一帧响应（临界区拷贝）：
 * 返回 1 = 至少收到过一帧有效数据，0 = 尚无有效帧 */
uint8_t FpgaLink_GetRx(fpga_rx_t* out);

/* 链路是否存活：有新帧且未超时 */
uint8_t FpgaLink_Alive(void);

#ifdef __cplusplus
}
#endif

#endif /* __FPGA_LINK_H */
