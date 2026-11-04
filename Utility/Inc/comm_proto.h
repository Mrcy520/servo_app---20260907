/**
 ******************************************************************************
 * @file    comm_proto.h
 * @brief   协议层 · 链路/会话层接口
 *
 * 层间位置：
 *
 *   USART/DMA  ──►  comm_proto（本层）  ──►  comm_app
 *   物理收发        帧边界 · 校验 · 寻址      参数表 · 量程
 *                   收发队列 · 超时监控        控制字语义 · 状态位
 *
 *   下行：DMA 空闲中断取回一批字节 → 状态机逐字节拼帧 → 校验 → 回调应用层
 *   上行：应用层组织数据 → 帧层组帧 → 本层发送队列 → DMA
 *
 * 本层刻意不含任何业务语义：不知道参数含义、不知道控制字如何生效，
 * 只知道"收到一帧该交给谁""要发一帧该问谁要数据"。
 ******************************************************************************
 */

#ifndef __COMM_PROTO_H
#define __COMM_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "proto.h"

/* 本机站号：与控制链路 ID 码共用同一编号（见 proto.h 的 COMM_DEV_ADDR）---- */
#ifndef COMM_PROTO_ADDR
#define COMM_PROTO_ADDR COMM_DEV_ADDR
#endif

/* 链路自检：置 1 时收到任意字节翻转 LED 并原样回送，量产必须置 0 ---------- */
#ifndef COMM_DIAG_ECHO
#define COMM_DIAG_ECHO 0
#endif

/* 上行发送周期（ms），1 对应 1 kHz ---------------------------------------- */
#ifndef COMM_UP_PERIOD_MS
#define COMM_UP_PERIOD_MS 1u
#endif

/* 协议层统计量 ------------------------------------------------------------ */
extern volatile uint32_t g_stat_ctrl_err;  /* 控制帧帧头/校验/ID/业务非法数 */
extern volatile uint32_t g_stat_param_err; /* 参数帧帧头/CRC 错误数 */
extern volatile uint32_t g_stat_timeout;   /* 下行超时次数 */
extern volatile uint32_t g_stat_rx_err;    /* 接收错误（溢出/帧错）次数 */
extern volatile uint32_t g_stat_tx_drop;   /* 发送队列满导致的丢弃次数 */

/* 接口 -------------------------------------------------------------------- */
void CommProto_Init(void);
void CommProto_Poll(void);
uint8_t CommProto_LinkAlive(void);
uint8_t CommProto_Send(const uint8_t* buf, uint8_t len); /* 1 入队成功，0 队列满 */
uint32_t CommProto_GetTick(void);                        /* 毫秒时基，供应用层计时 */

/* 丢弃接收缓冲中的残留并让状态机重新对齐。
 * 用于"主循环被长时间占用"之后（典型场景：擦写参数 Flash 约 20~40 ms，
 * 期间环形缓冲绕了若干圈，旧数据已被覆盖）。 */
void CommProto_FlushRx(void);

/* 应用层回调 --------------------------------------------------------------
 * 由 comm_app.c 实现。
 * 应用层与协议层的唯一耦合点，方向固定为 应用层 → 协议层。
 * ------------------------------------------------------------------------ */

/* 收到一帧已通过帧头/校验/ID 检查的控制帧。
 * 返回 1 表示业务上接受该帧，本层据此刷新链路计时；
 * 返回 0 表示业务上判定为非法帧（如未定义的控制模式），
 * 本层不计入"合法下行帧"，超时保护照旧生效。 */
uint8_t CommApp_OnControlFrame(const ctrl_down_t* c);

/* 收到一帧地址匹配的参数请求：req 已解出，rsp 的 addr/cmd/idx 已回填，
 * 应用层只需给出 status 与 value，本层负责组帧回发 */
void CommApp_OnParamRequest(const param_frame_t* req, param_frame_t* rsp);

/* 上行周期到：请应用层填充上行数据源 */
void CommApp_BuildUpFrame(ctrl_up_t* up);

/* 下行超时：请应用层执行安全回落 */
void CommApp_OnLinkTimeout(void);

/* 下行超时门限（ms），由应用层从参数表给出 */
uint32_t CommApp_GetLinkTimeoutMs(void);

#ifdef __cplusplus
}
#endif

#endif /* __COMM_PROTO_H */
