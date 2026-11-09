/**
 ******************************************************************************
 * @file    proto.h
 * @brief   协议层 · 帧编解码（纯数据，不依赖 HAL，可主机端单元测试）
 *
 * 协议分两套，风格与用途均不同：
 *
 *   A. 控制链路 —— 10 字节短帧，单帧头，无命令码，位域密集，
 *      周期双向流转，累加和校验。
 *        上行 驱动器 -> 主控：AA | ID | 状态 | 角度(3) | 电流(2) | 母线 | SUM
 *        下行 主控 -> 驱动器：AA | ID | 控制字 | 模式 | 值1(2) | 值2(2) | RSV | SUM
 *
 *   B. 参数链路 —— 16 字节定长，双字节帧头，命令码 + 参数索引，
 *      一问一答，CRC-16 校验，由上位机使用。
 *        5A A5 | 地址 | 命令 | 索引 | 状态 | 数值(4) | 保留(4) | CRC(2)
 *
 * 分层约定：本文件只描述"帧长什么样、怎么装、怎么拆"，
 * 不含收发调度（comm_proto）与业务语义（comm_app）。
 ******************************************************************************
 */

#ifndef __PROTO_H
#define __PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>

/* ===== 驱动器编号 ========================================================= */
/* 参数链路的设备地址（1 ~ 247），控制链路 ID 码已按协议固定为 0x55 */
#ifndef COMM_DEV_ADDR
#define COMM_DEV_ADDR 1u
#endif

#if (COMM_DEV_ADDR < 1u) || (COMM_DEV_ADDR > 247u)
#error "COMM_DEV_ADDR 必须在 1 ~ 247 之间（参数协议表 2-1）"
#endif

/* ===== A. 控制链路 ======================================================== */
#define CTRL_UP_LEN   10u
#define CTRL_DOWN_LEN 10u
#define CTRL_HEAD     0xAAu
#define CTRL_ID       0x55u /* 固定控制链路 ID 码为 0x55（与驱动器编号解耦，见控制协议表 2-1/3-1） */
#define CTRL_ID_BASE  0x55u /* 兼容旧宏定义 */

/* 上行状态位 */
#define ST_ANGLE_ERR   0x01u /* 角度传感器报警 */
#define ST_OVER_CUR    0x02u /* 过流 */
#define ST_OVER_VOLT   0x04u /* 过压 */
#define ST_UNDER_VOLT  0x08u /* 欠压 */
#define ST_OVER_TEMP   0x10u /* 过温 */
#define ST_ENABLED     0x20u /* 使能回读 */
#define ST_COMM_LOST   0x40u /* 通讯超时 */
#define ST_FAULT_LATCH 0x80u /* 故障锁存 */

/* 下行控制字 */
#define CW_ENABLE    0x01u /* 输出使能，电平有效 */
#define CW_CLR_FAULT 0x02u /* 故障清除，上升沿有效 */
#define CW_ZERO      0x04u /* 自动找零，上升沿有效 */
#define CW_LATCH     0x08u /* 参数锁存，上升沿有效 */
#define CW_MASK      0x0Fu /* 有效位掩码，bit4~7 为保留位（协议要求填 0） */

/* 控制模式 */
#define MODE_VOLTAGE          0x01u /* 电压开环：值1 = Uq，值2 = Ud */
#define MODE_CURRENT          0x02u /* 电流闭环：值1 = Iq，值2 = Id */
#define CTRL_MODE_IS_VALID(m) (((m) == MODE_VOLTAGE) || ((m) == MODE_CURRENT))

#define CTRL_TIMEOUT_MS 200u /* 下行超时后自动关输出（参数 0x0B 可改） */

/* 自动找零持续时间：文档规定约 5 s 后自动结束 */
#define CTRL_ZERO_SEARCH_MS 5000u

/* 设定值量纲：-32767 ~ 32767 对应 -100.00% ~ 100.00%
 * 换算关系：设定值 = 百分比 × 327.67，结果四舍五入取整（与文档示例帧一致）
 */
#define CTRL_REF_MAX     32767
#define CTRL_REF_PER_PCT 327.67f

/* 百分比 -> 设定值：对称四舍五入，并饱和到 ±CTRL_REF_MAX */
static inline int16_t ctrl_pct_to_ref(float pct)
{
    float v = pct * CTRL_REF_PER_PCT;

    v = (v >= 0.0f) ? (v + 0.5f) : (v - 0.5f);
    if (v > (float)CTRL_REF_MAX) {
        v = (float)CTRL_REF_MAX;
    } else if (v < -(float)CTRL_REF_MAX) {
        v = -(float)CTRL_REF_MAX;
    }
    return (int16_t)v;
}

/* 设定值 -> 百分比 */
static inline float ctrl_ref_to_pct(int16_t ref)
{
    return (float)ref / CTRL_REF_PER_PCT;
}

/* 控制链路量纲 */
#define CTRL_IQ_LSB   0.01f                  /* Q 轴电流，0.01 A/LSB */
#define CTRL_VBUS_LSB 0.5f                   /* 母线电压，0.5 V/LSB */
#define CTRL_ANG_LSB  (360.0f / 16777216.0f) /* 角度，360/2^24 度/LSB */

/* ===== B. 参数链路 ======================================================== */
#define PARAM_LEN      16u
#define PARAM_HEAD0    0x5Au
#define PARAM_HEAD1    0xA5u
#define PARAM_CRC_SPAN 14u /* 对 1..14 字节计算 */

#define PARAM_CMD_READ  0x01u
#define PARAM_CMD_WRITE 0x02u

#define PARAM_ST_OK       0x00u
#define PARAM_ST_UNKNOWN  0x01u
#define PARAM_ST_RANGE    0x02u
#define PARAM_ST_INDEX    0x03u
#define PARAM_ST_CRC      0x04u
#define PARAM_ST_READONLY 0x05u

/* 参数索引 */
#define PI_POLE_PAIRS   0x01u /* 电机极对数      1 ~ 100 */
#define PI_MOTOR_DIR    0x02u /* 电机方向        0 / 1 */
#define PI_ENC_DIR      0x03u /* 编码器方向      0 / 1 */
#define PI_IQ_KP        0x04u /* Q 轴电流比例    ×16 */
#define PI_IQ_KI        0x05u /* Q 轴电流积分    ×16 */
#define PI_ID_KP        0x06u /* D 轴电流比例    ×16 */
#define PI_ID_KI        0x07u /* D 轴电流积分    ×16 */
#define PI_ZERO_POS     0x08u /* 电机零位        0 ~ 11796480，1/2^15 度 */
#define PI_V_GAIN       0x09u /* 电压增益        0 ~ 500 */
#define PI_V_LIMIT      0x0Au /* 电压限幅        0 ~ 20 */
#define PI_COMM_TIMEOUT 0x0Bu /* 通讯超时        ms, 10 ~ 1000 */
#define PI_FW_VERSION   0x0Cu /* 固件版本        只读 */
#define PI_PROTO_VER    0x0Du /* 协议版本        只读 */

#define PARAM_INDEX_MAX 0x0Du

/* 协议版本，参数 0x0D 固定返回该值 */
#define PARAM_PROTO_VERSION 1u

/* 控制链路帧结构 */
typedef struct {
    uint8_t status;
    uint32_t angle;
    int16_t iq;
    uint8_t vbus;
} ctrl_up_t;

typedef struct {
    uint8_t ctrl;
    uint8_t mode;
    int16_t ref1;
    int16_t ref2;
    uint8_t rsv;
} ctrl_down_t;

void ctrl_pack_up(uint8_t* out, const ctrl_up_t* f);
void ctrl_pack_down(uint8_t* out, const ctrl_down_t* f);
int ctrl_unpack_down(const uint8_t* in, ctrl_down_t* f); /* 0 成功 */

/* 参数链路帧结构 */
typedef struct {
    uint8_t addr;
    uint8_t cmd;
    uint8_t idx;
    uint8_t status;
    int32_t value;
} param_frame_t;

void param_pack(uint8_t* out, const param_frame_t* f);
int param_unpack(const uint8_t* in, param_frame_t* f); /* 0 成功 */

#ifdef __cplusplus
}
#endif

#endif /* __PROTO_H */
