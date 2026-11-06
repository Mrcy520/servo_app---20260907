/**
 ******************************************************************************
 * @file    fpga_proto.h
 * @brief   FPGA <-> MCU 内部接口 · 帧编解码（纯数据，不依赖 HAL）
 *
 * 依据：Doc/驱动板内部接口.docx
 *   物理层  SPI3 全双工主模式，8 位，32 字节定长帧，刷新率 16 kHz，
 *           由 FPGA 的帧同步信号（PA11 下降沿）触发一次完整交换。
 *   字节序  大端：高字节在前，低字节在后。
 *   帧头    MCU -> FPGA : 0x55 0xAA
 *           FPGA -> MCU : 0xAA 0x55
 *   byte3   控制状态（双向同义），决定 byte4 之后的数据域含义：
 *           0x01 正常通讯 / 0x02 加载(设置)参数 / 0x03 读取参数 / 0x04 读取数据
 *
 * 分层：本文件只管"帧长什么样、怎么装、怎么拆、量纲怎么换"，
 *       不含收发调度（fpga_link）与业务映射（fpga_app）。
 ******************************************************************************
 */

#ifndef __FPGA_PROTO_H
#define __FPGA_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ===== 帧格式 ============================================================= */
#define FPGA_FRAME_LEN 32u

#define FPGA_HDR_TX0 0x55u /* MCU -> FPGA */
#define FPGA_HDR_TX1 0xAAu
#define FPGA_HDR_RX0 0xAAu /* FPGA -> MCU */
#define FPGA_HDR_RX1 0x55u

/* 控制状态 / 返回控制状态（双向同义） */
#define FPGA_ST_COMM       0x01u /* 正常通讯：实时量 / 运行指令 */
#define FPGA_ST_LOAD_PARAM 0x02u /* 加载参数（MCU->FPGA 带参数，FPGA->MCU 仅确认） */
#define FPGA_ST_READ_PARAM 0x03u /* 读取参数 */
#define FPGA_ST_READ_DATA  0x04u /* 读取内部数据（电气角、αβ、PWM） */

#define FPGA_ST_IS_VALID(s) ((uint8_t)(((s) >= FPGA_ST_COMM) && ((s) <= FPGA_ST_READ_DATA)))

/* 控制模式（仅 MCU -> FPGA，且仅控制状态为 0x01 时有效） */
#define FPGA_MODE_WAIT         0x01u /* 等待：关闭使能 */
#define FPGA_MODE_OPENLOOP_V   0x02u /* 开环电压 */
#define FPGA_MODE_CURRENT_LOOP 0x04u /* 电流环 */
#define FPGA_MODE_AUTO_ZERO    0x80u /* 自动找零 */
#define FPGA_MODE_SET_PARAM    0x88u /* 设置参数 */

/* ===== 量纲（文档表1/表2 的"最小有效位"） ================================ */
#define FPGA_VBUS_LSB_MV 11u /* 母线电压 0.011 V/LSB      */
#define FPGA_ANGLE_SHIFT 15u /* 角度 1/2^15 度/LSB        */

/* 角度满圈：360° = 360 × 2^15 = 11 796 480（= 0xB4 00 00）。
 * 依据文档"编码器角度值 Uint24 / 最小有效位 1/(2^15)"，FPGA 正常输出
 * 0 ~ FPGA_ANGLE_FULL-1，到满圈回绕为 0。
 * 注意：文档表1 总述另写"最小有效位 360/(2^24)"，与上式相差 512 倍；
 * 本工程按具体帧定义（1/2^15）实现，FPGA_ANGLE_FULL 同时充当口径校验门限——
 * 若现场出现 raw > 满圈，即说明实际口径为 2^24，须改本条而非改函数。 */
#define FPGA_ANGLE_FULL 11796480u
#define FPGA_I_SHIFT     16u /* 电流 1/2^16 A/LSB         */
#define FPGA_V_SHIFT     16u /* 电压 1/2^16 V/LSB         */
#define FPGA_GAIN_SHIFT  4u  /* Kp/Ki 1/2^4，等价于参数协议的 ×16 */

/* 设定值满量程：外部控制协议只给"百分比"，内部接口给绝对量，故需一个满量程。
 * 取内部接口的量程上限：电压 ±30 V、电流 ±10 A（见文档表1/表2）。 */
#define FPGA_UQ_FS_MV 30000 /* ±100% 对应 ±30.0 V */
#define FPGA_IQ_FS_MA 10000 /* ±100% 对应 ±10.0 A */

/* ===== 数据结构 =========================================================== */
/* MCU -> FPGA 命令（应用层填，链路层快照到双缓冲） */
typedef struct {
    uint8_t state; /* FPGA_ST_xxx                       */
    uint8_t mode;  /* FPGA_MODE_xxx，仅 state=0x01 有效 */

    /* state = 0x01：运行指令，LSB 1/2^16 V / A */
    int32_t uq_raw;
    int32_t ud_raw;
    int32_t iq_raw;
    int32_t id_raw;
    uint16_t v_gain; /* 电压增益 0 ~ 500 */
    uint8_t v_limit; /* 电压限幅 0 ~ 20  */

    /* state = 0x02：电机参数 */
    uint8_t pole_pairs;    /* 1 ~ 100          */
    uint8_t motor_dir;     /* 0 反向 / 1 正向   */
    uint8_t enc_dir;       /* 0 反向 / 1 正向   */
    uint32_t kp_q_raw;     /* LSB 1/2^4，即参数 0x04 */
    uint32_t ki_q_raw;     /* LSB 1/2^4，即参数 0x05 */
    uint32_t kp_d_raw;     /* LSB 1/2^4，即参数 0x06 */
    uint32_t ki_d_raw;     /* LSB 1/2^4，即参数 0x07 */
    uint32_t zero_pos_raw; /* LSB 1/2^15 度，即参数 0x08 */
} fpga_cmd_t;

/* FPGA -> MCU 响应（各状态域的字段共存，按 state 取用） */
typedef struct {
    uint8_t state; /* 返回控制状态 FPGA_ST_xxx */

    /* state = 0x01：实时量 */
    uint16_t vbus_raw;  /* 0.011 V/LSB  */
    uint32_t angle_raw; /* 1/2^15 度/LSB */
    uint8_t enc_fault;  /* 编码器故障字  */
    int32_t ia_raw;
    int32_t ib_raw;
    int32_t ic_raw;
    int32_t iq_raw;
    int32_t id_raw;
    int32_t uq_raw;
    int32_t ud_raw;

    /* state = 0x03：参数回读 */
    uint8_t pole_pairs;
    uint8_t motor_dir;
    uint8_t enc_dir;
    uint32_t kp_q_raw;
    uint32_t ki_q_raw;
    uint32_t kp_d_raw;
    uint32_t ki_d_raw;
    uint32_t zero_pos_raw;
    uint16_t v_gain;
    uint8_t v_limit;

    /* state = 0x04：内部数据 */
    uint32_t elec_angle_raw; /* 1/2^15 度/LSB */
    uint32_t zero_offset_raw;
    int32_t ialpha_raw;
    int32_t ibeta_raw;
    int32_t ualpha_raw;
    int32_t ubeta_raw;
    uint16_t pwm_u;
    uint16_t pwm_v;
    uint16_t pwm_w;
} fpga_rx_t;

/* ===== 接口 =============================================================== */

/* 组帧：按 cmd->state 生成 32 字节发送帧（0x03/0x04 的数据域全 0） */
void fpga_pack_cmd(uint8_t* out, const fpga_cmd_t* cmd);

/* 解帧：帧头 + 状态合法性检查后按状态解析；返回 1 有效，0 无效 */
uint8_t fpga_unpack_rx(const uint8_t* in, fpga_rx_t* rx);

/* ---- 量纲换算（纯整数，避免在 16 kHz 路径上引入浮点） ---- */

/* 百分比设定值 -> 电压/电流码值（LSB 1/2^16），±32767 对应 ±满量程 */
int32_t fpga_ref_to_v_raw(int16_t ref);
int32_t fpga_ref_to_i_raw(int16_t ref);

/* FPGA 原始角度（1/2^15 度）-> 对外控制帧的 Uint24 角度（360/2^24）
 * 超出满圈返回饱和值 0xFFFFFF（不静默回绕），恰好满圈按 0° 处理。
 * 配套用 fpga_angle_in_range 判定输入是否在量纲范围内。 */
uint32_t fpga_angle_to_ctrl24(uint32_t raw);

/* 角度码值是否在量纲范围内（0 ~ 满圈，含满圈的回绕点） */
static inline uint8_t fpga_angle_in_range(uint32_t raw)
{
    return (raw <= FPGA_ANGLE_FULL) ? 1u : 0u;
}

/* FPGA 母线电压（0.011 V）-> 对外控制帧的 Uint8（0.5 V/LSB） */
uint8_t fpga_vbus_to_ctrl8(uint16_t raw);

/* FPGA 电流（1/2^16 A）-> 对外控制帧的 Int16（0.01 A/LSB） */
int16_t fpga_i_raw_to_ctrl16(int32_t raw);

/* 码值 -> 千分之一单位（mV / mA / 0.001 度），供诊断输出 */
int32_t fpga_raw_to_milli(int32_t raw, uint8_t shift);
uint32_t fpga_angle_to_mdeg(uint32_t raw);
uint32_t fpga_vbus_to_mv(uint16_t raw);

#ifdef __cplusplus
}
#endif

#endif /* __FPGA_PROTO_H */
