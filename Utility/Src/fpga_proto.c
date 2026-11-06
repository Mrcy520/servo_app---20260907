/**
 ******************************************************************************
 * @file    fpga_proto.c
 * @brief   FPGA <-> MCU 内部接口 · 帧编解码实现（纯数据，可主机端测试）
 *
 * 全部为纯整数运算：16 kHz 交换节奏下不在控制路径上引入浮点。
 ******************************************************************************
 */

#include "fpga_proto.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* 24bit / 16bit 大端存取                                                      */
/* -------------------------------------------------------------------------- */

static void put_u24(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 16) & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)(v & 0xFFu);
}

static uint32_t get_u24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

/* 24bit 有符号：最高位为符号位，读入后做符号扩展 */
static int32_t get_s24(const uint8_t* p)
{
    uint32_t v = get_u24(p);

    if ((v & 0x00800000u) != 0u) {
        v |= 0xFF000000u;
    }
    return (int32_t)v;
}

static void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_u16(const uint8_t* p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* -------------------------------------------------------------------------- */
/* 组帧：MCU -> FPGA（文档表2）                                                */
/* -------------------------------------------------------------------------- */

void fpga_pack_cmd(uint8_t* out, const fpga_cmd_t* cmd)
{
    (void)memset(out, 0, FPGA_FRAME_LEN);

    out[0] = FPGA_HDR_TX0;
    out[1] = FPGA_HDR_TX1;
    out[2] = cmd->state;

    /* 控制模式只在"正常通讯"帧里有效；其余状态文档要求填"等待(关闭使能)" */
    out[3] = (cmd->state == FPGA_ST_COMM) ? cmd->mode : (uint8_t)FPGA_MODE_WAIT;

    switch (cmd->state) {
        case FPGA_ST_COMM:
            /* byte5..20：dq 设定（LSB 1/2^16）+ 电压增益/限幅 */
            put_u24(&out[4], (uint32_t)cmd->uq_raw & 0x00FFFFFFu);
            put_u24(&out[7], (uint32_t)cmd->ud_raw & 0x00FFFFFFu);
            put_u24(&out[10], (uint32_t)cmd->iq_raw & 0x00FFFFFFu);
            put_u24(&out[13], (uint32_t)cmd->id_raw & 0x00FFFFFFu);
            put_u16(&out[16], cmd->v_gain);
            out[18] = cmd->v_limit;
            break;

        case FPGA_ST_LOAD_PARAM:
            /* byte5..22：电机参数（LSB 与参数协议同量纲） */
            out[4] = cmd->pole_pairs;
            out[5] = cmd->motor_dir;
            out[6] = cmd->enc_dir;
            put_u24(&out[7], cmd->kp_q_raw & 0x00FFFFFFu);
            put_u24(&out[10], cmd->ki_q_raw & 0x00FFFFFFu);
            put_u24(&out[13], cmd->kp_d_raw & 0x00FFFFFFu);
            put_u24(&out[16], cmd->ki_d_raw & 0x00FFFFFFu);
            put_u24(&out[19], cmd->zero_pos_raw & 0x00FFFFFFu);
            break;

        case FPGA_ST_READ_PARAM:
        case FPGA_ST_READ_DATA:
        default:
            /* byte5..32 备用，保持 0：只发请求，数据由 FPGA 在回帧给出 */
            break;
    }
}

/* -------------------------------------------------------------------------- */
/* 解帧：FPGA -> MCU（文档表1）                                                */
/* -------------------------------------------------------------------------- */

uint8_t fpga_unpack_rx(const uint8_t* in, fpga_rx_t* rx)
{
    if ((in[0] != FPGA_HDR_RX0) || (in[1] != FPGA_HDR_RX1)) {
        return 0u;
    }
    if (FPGA_ST_IS_VALID(in[2]) == 0u) {
        return 0u; /* 非法状态字节：判为错帧，避免误锁 */
    }

    rx->state = in[2];

    switch (rx->state) {
        case FPGA_ST_COMM:
            rx->vbus_raw  = get_u16(&in[3]);
            rx->angle_raw = get_u24(&in[5]);
            rx->enc_fault = in[8];
            rx->ia_raw    = get_s24(&in[9]);
            rx->ib_raw    = get_s24(&in[12]);
            rx->ic_raw    = get_s24(&in[15]);
            rx->iq_raw    = get_s24(&in[18]);
            rx->id_raw    = get_s24(&in[21]);
            rx->uq_raw    = get_s24(&in[24]);
            rx->ud_raw    = get_s24(&in[27]);
            break;

        case FPGA_ST_LOAD_PARAM:
            /* 返回 0x02 仅表示 FPGA 已接收参数，数据域备用 */
            break;

        case FPGA_ST_READ_PARAM:
            rx->pole_pairs   = in[3];
            rx->motor_dir    = in[4];
            rx->enc_dir      = in[5];
            rx->kp_q_raw     = get_u24(&in[6]);
            rx->ki_q_raw     = get_u24(&in[9]);
            rx->kp_d_raw     = get_u24(&in[12]);
            rx->ki_d_raw     = get_u24(&in[15]);
            rx->zero_pos_raw = get_u24(&in[18]);
            rx->v_gain       = get_u16(&in[21]);
            rx->v_limit      = in[23];
            break;

        case FPGA_ST_READ_DATA:
            rx->elec_angle_raw  = get_u24(&in[3]);
            rx->zero_offset_raw = get_u24(&in[6]);
            rx->ialpha_raw      = get_s24(&in[9]);
            rx->ibeta_raw       = get_s24(&in[12]);
            rx->ualpha_raw      = get_s24(&in[15]);
            rx->ubeta_raw       = get_s24(&in[18]);
            rx->pwm_u           = get_u16(&in[21]);
            rx->pwm_v           = get_u16(&in[23]);
            rx->pwm_w           = get_u16(&in[25]);
            break;

        default:
            return 0u;
    }

    return 1u;
}

/* -------------------------------------------------------------------------- */
/* 量纲换算                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 百分比设定值 -> 电压码值（LSB 1/2^16 V）
 * @note  raw = ref / 32767 × FS_mV / 1000 × 2^16，四舍五入
 */
int32_t fpga_ref_to_v_raw(int16_t ref)
{
    int64_t v   = (int64_t)ref * (int64_t)FPGA_UQ_FS_MV * 65536LL;
    int64_t den = 32767LL * 1000LL;

    v += (ref >= 0) ? (den / 2) : -(den / 2);
    return (int32_t)(v / den);
}

/**
 * @brief 百分比设定值 -> 电流码值（LSB 1/2^16 A）
 */
int32_t fpga_ref_to_i_raw(int16_t ref)
{
    int64_t v   = (int64_t)ref * (int64_t)FPGA_IQ_FS_MA * 65536LL;
    int64_t den = 32767LL * 1000LL;

    v += (ref >= 0) ? (den / 2) : -(den / 2);
    return (int32_t)(v / den);
}

/**
 * @brief FPGA 角度码值（1/2^15 度）-> 对外控制帧 Uint24 角度（360/2^24）
 * @note  比例推导：raw × (2^24 / 360) / 2^15 = raw × 2^9 / 360 = raw × 64 / 45。
 *        64/45 与 2^9/360 精确相等，故除整数截断外无原理误差。
 *        满圈自洽性：360 × 2^15 = 11 796 480 恰为 45 的整数倍（= 2^18），
 *        故 raw = 满圈时结果精确等于 2^24，回绕后为 0。
 */
uint32_t fpga_angle_to_ctrl24(uint32_t raw)
{
    if (raw >= FPGA_ANGLE_FULL) {
        /* 恰好满圈是合法回绕点；超过则属超量程，饱和到 359.99998° */
        return (raw == FPGA_ANGLE_FULL) ? 0u : 0x00FFFFFFu;
    }

    return (uint32_t)(((uint64_t)raw * 128u + 45u) / 90u);
}

/**
 * @brief FPGA 母线电压码值（0.011 V）-> 对外控制帧 Uint8（0.5 V/LSB）
 */
uint8_t fpga_vbus_to_ctrl8(uint16_t raw)
{
    uint32_t mv = (uint32_t)raw * FPGA_VBUS_LSB_MV;
    uint32_t v8 = (mv + 250u) / 500u;

    return (uint8_t)((v8 > 255u) ? 255u : v8);
}

/**
 * @brief FPGA 电流码值（1/2^16 A）-> 对外控制帧 Int16（0.01 A/LSB）
 */
int16_t fpga_i_raw_to_ctrl16(int32_t raw)
{
    int64_t v = ((int64_t)raw * 100LL) + ((raw >= 0) ? 32768LL : -32768LL);
    int32_t c = (int32_t)(v / 65536LL);

    if (c > 32767) {
        c = 32767;
    } else if (c < -32768) {
        c = -32768;
    }
    return (int16_t)c;
}

/**
 * @brief 码值 -> 千分之一单位（shift=16 时输出 mV/mA，shift=4 时输出 0.001）
 */
int32_t fpga_raw_to_milli(int32_t raw, uint8_t shift)
{
    int64_t den = (int64_t)1 << shift;
    int64_t v   = (int64_t)raw * 1000LL;

    v += (raw >= 0) ? (den / 2) : -(den / 2);
    return (int32_t)(v / den);
}

/**
 * @brief 角度码值（1/2^15 度）-> 千分之一度
 */
uint32_t fpga_angle_to_mdeg(uint32_t raw)
{
    return (uint32_t)(((uint64_t)raw * 1000u + 16384u) / 32768u);
}

/**
 * @brief 角度码值 -> 千分之一度（实测校准口径）
 * @note  raw = FPGA_ANGLE_BASE + 角度 x 8192，一圈跨越 FPGA_ANGLE_SPAN。
 *        先扣偏置再按 span 取模，结果恒在 0~359999。
 */
uint32_t fpga_angle_to_mdeg_cal(uint32_t raw)
{
    uint32_t d = (raw >= FPGA_ANGLE_BASE) ? (raw - FPGA_ANGLE_BASE) : 0u;

    d %= FPGA_ANGLE_SPAN;

    return (uint32_t)(((uint64_t)d * 360000u + (FPGA_ANGLE_SPAN / 2u)) / FPGA_ANGLE_SPAN);
}

/**
 * @brief 母线电压码值 -> mV
 */
uint32_t fpga_vbus_to_mv(uint16_t raw)
{
    return (uint32_t)raw * FPGA_VBUS_LSB_MV;
}

/* -------------------------------------------------------------------------- */
/* 位相位纠正                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 把整帧按 k 位右移还原（对应"整帧左移了 k 位"的相位）
 * @note  dst[i] = (in[i] >> k) | ((in[i-1] 的低 k 位) << (8-k))。
 *        i=0 时的借位来源不在本窗口内（属于上一帧），置 0；
 *        实测该字节本就不可靠（窗边界），帧头为常量，不影响判定与解析。
 */
void fpga_unshift(const uint8_t* in, uint8_t k, uint8_t* out)
{
    uint8_t i;
    uint8_t mask;
    uint8_t prev_lo = 0u;

    if ((k == 0u) || (k > 7u)) {
        for (i = 0u; i < (uint8_t)FPGA_FRAME_LEN; i++) {
            out[i] = in[i];
        }
        return;
    }

    mask = (uint8_t)((1u << k) - 1u);

    for (i = 0u; i < (uint8_t)FPGA_FRAME_LEN; i++) {
        uint8_t b = in[i];

        out[i]  = (uint8_t)((b >> k) | (uint8_t)(prev_lo << (8u - k)));
        prev_lo = (uint8_t)(b & mask);
    }
}

void fpga_nibble_swap(const uint8_t* in, uint8_t* out)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)FPGA_FRAME_LEN; i++) {
        out[i] = (uint8_t)((in[i] >> 4) | (uint8_t)(in[i] << 4));
    }
}

/**
 * @brief 判定一个 32 字节窗口处于哪种相位
 * @retval 0 = 原样即可；1..7 = 需要按该位数右移；FPGA_PHASE_ROT|k = 需先右移再半字节互换；
 *         0xFF = 都试过不合法
 */
uint8_t fpga_detect_phase(const uint8_t* win)
{
    uint8_t buf[FPGA_FRAME_LEN];
    uint8_t swp[FPGA_FRAME_LEN];
    uint8_t k;

    for (k = 0u; k <= 7u; k++) {
        fpga_unshift(win, k, buf);

        if (k == 0u) {
            /* 无位移：帧头必须逐字节精确匹配 */
            if ((buf[0] == FPGA_HDR_RX0) && (buf[1] == FPGA_HDR_RX1) &&
                (FPGA_ST_IS_VALID(buf[2]) != 0u)) {
                return 0u;
            }
            fpga_nibble_swap(buf, swp);
            if ((swp[0] == FPGA_HDR_RX0) && (swp[1] == FPGA_HDR_RX1) &&
                (FPGA_ST_IS_VALID(swp[2]) != 0u)) {
                return (uint8_t)(FPGA_PHASE_ROT | 0u);
            }
            continue;
        }

        /* k>=1：第 0 字节的借位来自窗口外，第 1 字节最高位又依赖它，故这两位不可靠；
         * 第 2 字节起完全正确。用"第 1 字节低 7 位 == 0x55"+"状态合法"判定。 */
        if (((buf[1] & 0x7Fu) == 0x55u) && (FPGA_ST_IS_VALID(buf[2]) != 0u)) {
            return k;
        }
        fpga_nibble_swap(buf, swp);
        if (((swp[1] & 0x7Fu) == 0x55u) && (FPGA_ST_IS_VALID(swp[2]) != 0u)) {
            return (uint8_t)(FPGA_PHASE_ROT | k);
        }
    }

    return 0xFFu;
}

/**
 * @brief 按判定的相位把一个窗口还原成规范帧
 * @note  位移相位下第 0/1 字节不可恢复（借位在窗口外），但它们是固定的帧头常量
 *        AA 55，且相位已由状态字节验证，因此这里直接补上——否则 unpack 的帧头检查会拒收。
 */
void fpga_apply_phase(const uint8_t* win, uint8_t phase, uint8_t* out)
{
    uint8_t k = (uint8_t)(phase & 0x7Fu);

    fpga_unshift(win, k, out);

    if (k != 0u) {
        out[0] = FPGA_HDR_RX0;
        out[1] = FPGA_HDR_RX1;
    }

    if ((phase & FPGA_PHASE_ROT) != 0u) {
        uint8_t tmp[FPGA_FRAME_LEN];
        uint8_t i;

        for (i = 0u; i < (uint8_t)FPGA_FRAME_LEN; i++) {
            tmp[i] = out[i];
        }
        fpga_nibble_swap(tmp, out);
        if (k != 0u) {
            out[0] = FPGA_HDR_RX0;
            out[1] = FPGA_HDR_RX1;
        }
    }
}
