/**
 ******************************************************************************
 * @file    comm_cfg.h
 * @brief   通信与调试的全局编译期开关（协议层与应用层共用，故放在 Utility）
 *
 * 日志物理通道 = RTT（调试探针 SWD/JTAG），**不占用任何 UART**。
 * 各来源分流到不同上行通道，在调试器里是各自独立的终端窗口：
 *   ch0 MCU   ch1 FPGA   ch2 COMM   ch3 ANGLE     （见 Utility/Inc/rtt_log.h）
 * 其中 ch3 是角度单值流（每行 "angle:359.997"），1 kHz 等间隔采样，供主机作图。
 *
 * 两档工作模式：
 *
 *   ① 调试档（默认）：COMM_DEBUG_REPORT = 1
 *      · [DN]/[UP] 走 COMM 通道，[FP] 走 FPGA 通道，[PR]/[EV]/[ST] 走 MCU 通道
 *      · COMM_DEBUG_SILENT 默认 0：协议照常 1 kHz 上行 + 参数应答——
 *        RTT 与 USART1 完全无关，不再需要为"让串口只有文本"而牺牲协议输出
 *      · 若探针没接、没人读 RTT，缓冲区写满后整行丢弃，仅浪费极短的内存拷贝时间
 *
 *   ② 交付档：COMM_DEBUG_REPORT = 0
 *      · 打印代码全部编译为空，零开销
 *      · 恢复文档规定：上行帧 1 kHz 主动上报 + 参数链路一问一答
 *
 * RTT 缓冲区可通过编译宏调整（默认已够用，16 KB RAM 下 3 x 512 B）：
 *   -DRTT_BUFFER_SIZE_UP=1024u   每通道上行缓冲
 *   -DRTT_NUM_UP_BUFFERS=4u      通道数（新增通道须同步 rtt_log.h 的 log_chan_t）
 ******************************************************************************
 */

#ifndef __COMM_CFG_H
#define __COMM_CFG_H

/* ===== 调试上报总开关 ===== */
#ifndef COMM_DEBUG_REPORT
#define COMM_DEBUG_REPORT 1
#endif

/* ===== 调试期协议静默 =====
 * 1 = 不发上行主动帧、不发参数应答
 * 0 = 协议照常输出（推荐：日志已走 RTT，与 USART1 不冲突）
 *
 * 历史原因：旧方案 printf 重定向到 USART1，与协议共用一条线，必须静默其一方；
 * 现 printf 与 dbg_report 都走 RTT，此开关只在"确实要用 USART1 看文本
 * （retarget.c 的 PRINTF_SINK_RTT=2）"时才需要置 1。 */
#ifndef COMM_DEBUG_SILENT
#define COMM_DEBUG_SILENT 0
#endif

#endif /* __COMM_CFG_H */
