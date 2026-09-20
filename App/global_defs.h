
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __GLOBAL_DEFS_H
#define __GLOBAL_DEFS_H

#include <stdint.h>
#include <string.h>

#pragma pack(1)
typedef union
{
	volatile uint16_t all;
	struct 
	{
		volatile uint8_t low : 8;				// 双字节的低字节
		volatile uint8_t high : 8;				// 双字节的高字节
	}byte;
}byte_word_t;

#pragma pack(1)
typedef union
{
	volatile uint32_t all;
	struct 
	{
		volatile uint8_t low : 8;				// 四字节的低字节
		volatile uint8_t middle : 8;			// 四字节的中字节
		volatile uint8_t middle_high : 8;		// 四字节的中高字节
		volatile uint8_t high : 8;				// 四字节的高字节
	}byte;
}byte_double_word_t;

#pragma pack(1)
typedef union
{
  uint32_t data_uint32;
  float data_float;
}uint32_to_float_t;

#pragma pack()

extern volatile float filter_out;
/**
 * @brief 从有效数据中按小端模式读取16位整数
 * @param data 输入数据起始地址
 * @return 16位整数值
 */
static inline uint16_t read_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/**
 * @brief 从有效数据中按小端(little-endian)模式读取32位整数
 * @param data 输入数据起始地址
 * @return 32位整数值
 */
static inline uint32_t read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] |
         ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

/**
 * @brief 按小端(little-endian)模式将16位整数写入缓冲区
 * @param buf 输出目标缓冲区
 * @param value 输入16位整数值
 */
static inline void write_le16(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)(value & 0xFF);
  buf[1] = (uint8_t)((value >> 8) & 0xFF);
}

/**
 * @brief 按小端(little-endian)模式将32位整数写入缓冲区
 * @param buf 输出目标缓冲区
 * @param value 输入32位整数值
 */
static inline void write_le32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)(value & 0xFF);
  buf[1] = (uint8_t)((value >> 8) & 0xFF);
  buf[2] = (uint8_t)((value >> 16) & 0xFF);
  buf[3] = (uint8_t)((value >> 24) & 0xFF);
}

/**
 * @brief 从有效数据中按大端(big-endian)模式读取16位整数
 * @param data 输入数据起始地址
 * @return 16位整数值
 */
static inline uint16_t read_be16(const uint8_t *data)
{
  return ((uint16_t)data[0] << 8) | (uint16_t)data[1];
}

/**
 * @brief 从有效数据中按大端模式读取32位整数
 * @param data 输入数据起始地址
 * @return 32位整数值
 */
static inline uint32_t read_be32(const uint8_t *data)
{
  return ((uint32_t)data[0] << 24) |
         ((uint32_t)data[1] << 16) |
         ((uint32_t)data[2] << 8)  |
         (uint32_t)data[3];
}

/**
 * @brief 按大端(big-endian)模式将16位整数写入缓冲区
 * @param buf 输出目标缓冲区
 * @param value 输入16位整数值
 */
static inline void write_be16(uint8_t *buf, uint16_t value)
{
  buf[0] = (uint8_t)((value >> 8) & 0xFF);
  buf[1] = (uint8_t)(value & 0xFF);
}

/**
 * @brief 按大端(big-endian)模式将16位整数写入缓冲区
 * @param buf 输出目标缓冲区
 * @param value 输入16位整数值
 */
static inline void write_be32(uint8_t *buf, uint32_t value)
{
  buf[0] = (uint8_t)((value >> 24) & 0xFF);
  buf[1] = (uint8_t)((value >> 16) & 0xFF);
  buf[2] = (uint8_t)((value >> 8) & 0xFF);
  buf[3] = (uint8_t)(value & 0xFF);
}

/**
 * @brief 按大端(big-endian)模式将32位整数读取成float
 * @param data 输入数据起始地址
 * @return float数据
 */
static inline float read_float_be(const uint8_t *data)
{
  uint32_to_float_t temp;
  temp.data_uint32 = ((uint32_t)data[0] << 24) |
                     ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] << 8) |
                     (uint32_t)data[3];
  return temp.data_float;
}

#endif /* __GLOBAL_DEFS_H */

