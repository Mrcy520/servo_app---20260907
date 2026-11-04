/**
 * @file crc.c
 * @author yex
 * @brief CRC calculation functions
 * @version 0.2
 * @date 2026-11-04
 */
#include "crc.h"

/**
 * @brief 累加和校验
 * @param data 数据指针
 * @param length 数据长度
 * @return uint8_t 校验结果（前 length 字节累加，取低 8 位）
 */
uint8_t crc_sum_checksum(const uint8_t* data, uint16_t length)
{
    uint16_t crc = 0x00u;
    for (uint16_t i = 0; i < length; i++) {
        crc += data[i];
    }
    return (uint8_t)(crc & 0x00FFu);
}

/**
 * @brief CRC-16/CCITT-FALSE 校验
 * @param data 数据指针
 * @param length 数据长度
 * @return uint16_t 校验结果
 * @note  参数协议文档指定：多项式 0x1021，初值 0xFFFF，输入不反射，
 *        输出不异或（即 MSB-first 移位）。此前实现使用反射式 0xA001
 *        （CRC-16/MODBUS），与本协议不兼容，已更正。
 */
uint16_t crc16_checksum(const uint8_t* data, uint16_t length)
{
    uint16_t crc = 0xFFFFu;

    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t j = 0; j < 8u; j++) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/**
 * @brief 算法自检
 * @return uint8_t 1 通过（结果等于 0x29B1），0 不通过
 */
uint8_t crc16_selftest(void)
{
    static const uint8_t probe[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    return (uint8_t)(crc16_checksum(probe, 9u) == CRC16_CCITT_FALSE_CHECK);
}
