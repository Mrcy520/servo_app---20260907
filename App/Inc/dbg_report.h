/**
 ******************************************************************************
 * @file    dbg_report.h
 * @brief   调试上报层 · 把上下行报文解码成人类可读的单行日志
 *
 * 用途：
 *   文档要求上行帧 1 kHz 主动上报，但 1 kHz 的原始十六进制流无法肉眼判读。
 *   本层只做"抽样打印"：协议仍然按 1 kHz 收发，打印按周期（默认 200 ms）
 *   或在事件（状态位变化 / 控制字变化 / 链路超时）时立即输出。
 *
 * 开关：
 *   COMM_DEBUG_REPORT = 1  调试期：打印 [DN]/[UP]/[FP]/[ST] 四类日志
 *   COMM_DEBUG_REPORT = 0  交付期：全部编译为空，零开销，恢复文档规定的纯主动上报
 *
 * 物理口：USART2（921600 8N1），与协议口 USART1 完全分离。
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
 * 一行约 100 字符，921600 下阻塞约 1.1 ms；事件一次打 3~4 行。
 * 取 50 ms 可保证最坏情况打印占用 < 7% CPU，同时仍能抓住秒级现象。 */
#ifndef DBG_MIN_GAP_MS
#define DBG_MIN_GAP_MS 50u
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

#ifdef __cplusplus
}
#endif

#endif /* __DBG_REPORT_H */
