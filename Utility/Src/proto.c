/**
 ******************************************************************************
 * @file    proto.c
 * @brief   协议层 · 帧编解码实现
 *
 *   控制链路：累加和校验，10 字节定长
 *   参数链路：CRC-16 校验，16 字节定长
 *
 * 全部函数为纯数据操作，不访问外设、不持有状态，
 * 因此可脱离目标板用 gcc 直接编译做主机端自检。
 ******************************************************************************
 */

#include "proto.h"
#include "crc.h"

/* -------------------------------------------------------------------------- */
/* A. 控制链路                                                                 */
/* -------------------------------------------------------------------------- */

void ctrl_pack_up(uint8_t* out, const ctrl_up_t* f)
{
    out[0] = CTRL_HEAD;
    out[1] = CTRL_ID;
    out[2] = f->status;
    out[3] = (uint8_t)((f->angle >> 16) & 0xFFu);
    out[4] = (uint8_t)((f->angle >> 8) & 0xFFu);
    out[5] = (uint8_t)(f->angle & 0xFFu);
    out[6] = (uint8_t)((uint16_t)f->iq >> 8);
    out[7] = (uint8_t)((uint16_t)f->iq & 0x00FFu);
    out[8] = f->vbus;
    out[9] = crc_sum_checksum(out, CTRL_UP_LEN - 1u);
}

void ctrl_pack_down(uint8_t* out, const ctrl_down_t* f)
{
    out[0] = CTRL_HEAD;
    out[1] = CTRL_ID;
    out[2] = f->ctrl;
    out[3] = f->mode;
    out[4] = (uint8_t)((uint16_t)f->ref1 >> 8);
    out[5] = (uint8_t)((uint16_t)f->ref1 & 0x00FFu);
    out[6] = (uint8_t)((uint16_t)f->ref2 >> 8);
    out[7] = (uint8_t)((uint16_t)f->ref2 & 0x00FFu);
    out[8] = f->rsv;
    out[9] = crc_sum_checksum(out, CTRL_DOWN_LEN - 1u);
}

int ctrl_unpack_down(const uint8_t* in, ctrl_down_t* f)
{
    if (in[0] != CTRL_HEAD) {
        return 1;
    }
    if (in[CTRL_DOWN_LEN - 1u] != crc_sum_checksum(in, CTRL_DOWN_LEN - 1u)) {
        return 2;
    }
    if (in[1] != CTRL_ID) {
        return 3;
    }

    f->ctrl = in[2];
    f->mode = in[3];
    f->ref1 = (int16_t)(((uint16_t)in[4] << 8) | in[5]);
    f->ref2 = (int16_t)(((uint16_t)in[6] << 8) | in[7]);
    f->rsv  = in[8];
    return 0;
}

/* -------------------------------------------------------------------------- */
/* B. 参数链路                                                                 */
/* -------------------------------------------------------------------------- */

void param_pack(uint8_t* out, const param_frame_t* f)
{
    uint16_t crc;
    uint32_t v = (uint32_t)f->value;

    out[0]  = PARAM_HEAD0;
    out[1]  = PARAM_HEAD1;
    out[2]  = f->addr;
    out[3]  = f->cmd;
    out[4]  = f->idx;
    out[5]  = f->status;
    out[6]  = (uint8_t)((v >> 24) & 0xFFu);
    out[7]  = (uint8_t)((v >> 16) & 0xFFu);
    out[8]  = (uint8_t)((v >> 8) & 0xFFu);
    out[9]  = (uint8_t)(v & 0xFFu);
    out[10] = 0u;
    out[11] = 0u;
    out[12] = 0u;
    out[13] = 0u;

    crc                      = crc16_checksum(out, PARAM_CRC_SPAN);
    out[PARAM_CRC_SPAN]      = (uint8_t)(crc >> 8);
    out[PARAM_CRC_SPAN + 1u] = (uint8_t)(crc & 0x00FFu);
}

int param_unpack(const uint8_t* in, param_frame_t* f)
{
    uint16_t crc_rx;

    if ((in[0] != PARAM_HEAD0) || (in[1] != PARAM_HEAD1)) {
        return 1;
    }

    crc_rx = (uint16_t)(((uint16_t)in[PARAM_CRC_SPAN] << 8) | in[PARAM_CRC_SPAN + 1u]);
    if (crc16_checksum(in, PARAM_CRC_SPAN) != crc_rx) {
        return 2;
    }

    f->addr   = in[2];
    f->cmd    = in[3];
    f->idx    = in[4];
    f->status = in[5];
    f->value  = (int32_t)(((uint32_t)in[6] << 24) | ((uint32_t)in[7] << 16) |
                         ((uint32_t)in[8] << 8) | (uint32_t)in[9]);
    return 0;
}
