/**
 ******************************************************************************
 * @file    fpga_app.h
 * @brief   应用层 · FPGA 执行器映射接口
 *
 * 本层回答"外部协议的语义如何落到 FPGA 的寄存器语义上"：
 *   g_param    → 0x02 参数帧（量纲与参数协议 1:1，无需换算）
 *   g_control  → 0x01 运行帧（使能/模式/找零/给定百分比 → 控制模式 + dq 码值）
 *   FPGA 响应  → g_status.angle / iq / vbus + 编码器故障位
 ******************************************************************************
 */

#ifndef __FPGA_APP_H
#define __FPGA_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 上电是否自动下装一次参数（0x02）。
 * 默认 0：避免用 MCU 侧默认值覆盖 FPGA 里已经校准好的参数；
 * 参数只有经参数链路写入并由控制字 bit3 锁存后才会自动下装。 */
#ifndef FPGA_APP_LOAD_ON_START
#define FPGA_APP_LOAD_ON_START 0
#endif

/* 非实时请求（读参数/读数据/加载参数）无应答的次数 */
extern volatile uint32_t g_fpga_req_err;

void FpgaApp_Init(void); /* 必须在 MX_SPI3_Init / MX_GPIO_Init 之后调用 */
void FpgaApp_Poll(void); /* 主循环任务：1 ms 节拍消费响应、调度请求、刷新命令帧 */

/* FPGA 链路是否存活（16 kHz 帧同步 + 响应超时判定） */
uint8_t FpgaApp_LinkAlive(void);

#ifdef __cplusplus
}
#endif

#endif /* __FPGA_APP_H */
