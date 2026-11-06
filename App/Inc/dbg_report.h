/**
 ******************************************************************************
 * @file    dbg_report.h
 * @brief   调试上报层 · 把上下行报文解码成人类可读的单行日志（按来源分通道）
 *
 * 用途：
 *   文档要求上行帧 1 kHz 主动上报，但 1 kHz 的原始十六进制流无法肉眼判读。
 *   本层只做"抽样打印"：协议仍然按 1 kHz 收发，打印按周期（默认 200 ms）
 *   或在事件（状态位变化 / 控制字变化 / 链路超时）时立即输出。
 *
 * 输出通道（RTT，见 Utility/Inc/rtt_log.h）：
 *   LOG_CH_COMM  [DN] [UP]          外部协议报文，含线上 hex
 *   LOG_CH_FPGA  [FP] [ST]          FPGA 内部接口实时量与 SPI3 链路统计
 *   LOG_CH_MCU   [PR] [EV] [ST]     MCU 自身：参数读写、事件、协议统计
 *   LOG_CH_ANGLE angle:359.997      角度单值流，等间隔采样，专供作图
 *
 * 开关：
 *   COMM_DEBUG_REPORT = 1  调试期：上述日志全部输出
 *   COMM_DEBUG_REPORT = 0  交付期：全部编译为空，零开销，恢复文档规定的纯主动上报
 *
 * 物理口：调试探针 SWD/JTAG（RTT），不占用 UART；协议口 USART1 因此可以
 *         同时满速跑 1 Hz 上行帧与参数应答，无需 COMM_DEBUG_SILENT。
 ******************************************************************************
 */

#ifndef __DBG_REPORT_H
#define __DBG_REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "proto.h"
#include "fpga_proto.h"
#include "comm_cfg.h" /* 开关统一由 comm_cfg.h 管理：COMM_DEBUG_REPORT / COMM_DEBUG_SILENT */

#if (COMM_DEBUG_REPORT != 0)

/* 周期摘要间隔（ms） */
#ifndef DBG_PERIOD_MS
#define DBG_PERIOD_MS 200u
#endif

/* 事件即时打印的最小间隔（ms）：
 * 旧方案走 USART1 阻塞发送，一行约 100 字符 / 921600 ≈ 1.1 ms，故取 50 ms 限制占用；
 * 现走 RTT（非阻塞内存拷贝），单行成本降到几十微秒，最小间隔可收紧，
 * 以便更贴近真实的事件时序。改大只是降低刷屏速度，不影响功能。 */
#ifndef DBG_MIN_GAP_MS
#define DBG_MIN_GAP_MS 20u
#endif

/* 角度单值流采样周期（ms）：默认 2 ms = 500 Hz。
 * 该行每个周期固定输出一条，与其它日志互不影响，时间轴才是均匀的（作图前提）。
 * 上限受主循环周期约束：若 Dbg_Poll 间隔大于此值，实际就退化为循环频率。
 * 为什么不用 1 ms：实测 1 kHz（15.5 B/行 = 15.5 KB/s）时主机端读不过来，
 * [ST] 的 rtt_drop 第 4 位以约 1.5 KB/s 增长，约 23% 的样本被丢弃；
 * 500 Hz 降到 7.8 KB/s 后可全部送出。要更高采样率请同时加大 RTT_BUFFER_SIZE_UP。
 * 注意 [FP] 的 200 ms 抽样在电机高速旋转时会混叠，看瞬时变化请以本通道为准。 */
#ifndef DBG_ANGLE_PERIOD_MS
#define DBG_ANGLE_PERIOD_MS 2u
#endif

void Dbg_Init(void);
void Dbg_Poll(void);                                  /* 主循环调用 */
void Dbg_OnUpFrame(const ctrl_up_t* up);              /* 上行帧组好后 */
void Dbg_OnDownFrame(const ctrl_down_t* dn);          /* 下行帧接受后 */
void Dbg_OnFpgaFrame(const fpga_rx_t* rx);            /* FPGA 响应帧解析后 */
void Dbg_OnParam(const param_frame_t* req, const param_frame_t* rsp); /* 参数读写 */
void Dbg_OnEvent(const char* tag);                    /* 自定义事件（超时等） */

#else /* 关闭：全部为空实现，编译器可以直接内联掉 */

static inline void Dbg_Init(void) {}
static inline void Dbg_Poll(void) {}
static inline void Dbg_OnUpFrame(const ctrl_up_t* up)
{
    (void)up;
}
static inline void Dbg_OnDownFrame(const ctrl_down_t* dn)
{
    (void)dn;
}
static inline void Dbg_OnFpgaFrame(const fpga_rx_t* rx)
{
    (void)rx;
}
static inline void Dbg_OnParam(const param_frame_t* req, const param_frame_t* rsp)
{
    (void)req;
    (void)rsp;
}
static inline void Dbg_OnEvent(const char* tag)
{
    (void)tag;
}

#endif /* COMM_DEBUG_REPORT */

/* 硬故障上报：不受调试开关控制，永远可用。
 * 由 Core/Src/stm32f3xx_it.c 的 HardFault_Handler 调用。 */
void Dbg_OnHardFault(void);

#ifdef __cplusplus
}
#endif

#endif /* __DBG_REPORT_H */
