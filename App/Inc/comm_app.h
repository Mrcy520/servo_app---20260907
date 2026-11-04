/**
 ******************************************************************************
 * @file    comm_app.h
 * @brief   应用层 · 电机控制业务语义接口
 *
 * 本层回答"帧里的字段是什么意思"：
 *   控制字  → 使能门控、故障清除 / 找零 / 参数锁存的上升沿判定
 *   参数索引 → 参数表、量程、只读属性、暂存与锁存
 *   状态位  → 由谁置位、何时撤销
 *   超时    → 落到什么安全状态
 *
 * 协议原文对应关系：
 *   Doc/驱动板控制协议.docx  表 2-1 ~ 2-3、表 3-1 ~ 3-3
 *   Doc/驱动板参数协议.docx  表 2-1 ~ 2-3、表 3-1
 ******************************************************************************
 */

#ifndef __COMM_APP_H
#define __COMM_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "proto.h"
#include "comm_proto.h"

/* 固件版本，参数 0x0C 的高 16 位为主版本、低 16 位为次版本 --------------- */
#define FW_VER_MAJOR 1
#define FW_VER_MINOR 0

/* 全局数据区 --------------------------------------------------------------
 * 字段归属（避免多处写同一字段）：
 *   g_status.status       本层独占：使能回读 / 通讯超时 / 保护位 / 故障锁存
 *   g_status.angle/iq/vbus 由采样层（SPI 接收）填充，本层只读
 *   g_control             本层解析下行帧后写入的生效控制量，功率级只读
 *   g_param               已锁存的生效参数，功率级只读
 * ------------------------------------------------------------------------ */
extern volatile ctrl_up_t g_status;
extern volatile ctrl_down_t g_control;
extern int32_t g_param[PARAM_INDEX_MAX + 1];

/* 参数代次：每次控制字 bit3 锁存生效后自增。
 * 执行层（如 fpga_app）比对该值即可判断"参数已更新，需要重新下装"，
 * 避免应用层与执行层之间再引入回调耦合。 */
extern volatile uint32_t g_param_gen;

/* 接口 -------------------------------------------------------------------- */
void CommApp_Init(void); /* 参数表初始化 + 启动协议层接收 */
void CommApp_Poll(void); /* 周期任务：协议层定时 + 自动找零计时 */
uint8_t CommApp_ControlAlive(void);

/* 保护层接口：检出过流/过压/欠压/过温/角度异常时调用。
 * 置位对应状态位并锁存 bit7，同时强制关闭输出、给定清零；
 * 需主控下发控制字 bit1 上升沿清除。 */
void CommApp_SetFault(uint8_t fault_bits);

/* 实时保护位电平更新：只改指定位，不锁存 bit7、不关输出。
 * 用于"故障条件仍在但主控已清锁存"的场合，保证状态位反映真实条件
 * （例如 FPGA 每帧上报的编码器故障字）。
 *   active != 0 → 置位；active == 0 → 清除。 */
void CommApp_SetFaultLevel(uint8_t fault_bit, uint8_t active);

/* 手动清除故障锁存（控制字 bit1 上升沿内部已调用同一逻辑） */
void CommApp_ClearFault(void);

/* 把参数暂存区的内容整体搬入生效区（控制字 bit3 上升沿内部已调用） */
void CommApp_LatchParams(void);

/* 输出是否处于使能状态（与上行帧 bit5 一致） */
uint8_t CommApp_IsEnabled(void);

/* 自动找零是否进行中 */
uint8_t CommApp_ZeroSearchActive(void);

/* 本次上电是否从内部 Flash 恢复了参数（保存于参数锁存后自动触发） */
uint8_t CommApp_ParamsFromNvm(void);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_APP_H */
