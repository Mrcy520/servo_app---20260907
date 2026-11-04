/**
 * @file crc.h
 * @author yex
 * @brief CRC calculation functions
 * @version 0.2
 * @date 2026-11-04
 */
#ifndef _CRC_H_
#define _CRC_H_

#include <stdint.h>

/* 参数链路校验：CRC-16/CCITT-FALSE
 *   多项式 0x1021，初值 0xFFFF，输入不反射，输出不异或
 *   标准检查值：对 ASCII "123456789" 计算应得 0x29B1
 */
#define CRC16_CCITT_FALSE_CHECK 0x29B1u

uint8_t crc_sum_checksum(const uint8_t* data, uint16_t length);
uint16_t crc16_checksum(const uint8_t* data, uint16_t length);

/* 算法自检：返回 1 表示实现与协议文档一致，0 表示已偏离（主机端测试用） */
uint8_t crc16_selftest(void);

#endif  // _CRC_H_
