/**
 ******************************************************************************
 * @file    nvm.h
 * @brief   内部 Flash 掉电保存 · 裸记录存取（不含业务语义）
 *
 * 存储策略：两页轮换（A/B），每次保存只擦写"非活动页"，校验通过后才切换。
 *   - 擦/写过程中掉电：目标页 magic 或 CRC 不成立，加载时被跳过，旧页仍然有效；
 *   - 记录带递增序号，加载时取"有效且序号最大"的一页；
 *   - 页内布局（小端）：
 *       [0..3]   magic  0x4E564D31 ('NVM1')
 *       [4..7]   seq    uint32，每次保存 +1
 *       [8..9]   len    有效载荷字节数（≤ NVM_PAYLOAD_MAX）
 *       [10..11] 保留   0xFFFF
 *       [12..59] 载荷   由上层决定（本项目放参数表）
 *       [60..61] CRC16-CCITT-FALSE，覆盖 [0..60)
 *       [62..63] 保留   0xFFFF
 *
 * 占用：0x0800F000 起 2 页 × 2 KB（STM32F3 页大小 0x800）。
 *       链接脚本已把 FLASH 长度从 64K 收到 60K，代码不会越过该区域。
 ******************************************************************************
 */

#ifndef __NVM_H
#define __NVM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 页与记录参数（与链接脚本保留区一致） */
#define NVM_PAGE_A        0x0800F000u
#define NVM_PAGE_B        0x0800F800u
#define NVM_FLASH_PAGE_SZ 0x800u
#define NVM_SLOT_SIZE     64u
#define NVM_HDR_SIZE      12u
#define NVM_CRC_OFF       60u
#define NVM_PAYLOAD_MAX   (NVM_CRC_OFF - NVM_HDR_SIZE) /* 48 字节 */

/* 保存失败次数（擦除/编程/回读校验失败） */
extern volatile uint32_t g_stat_nvm_err;

/* 识别两页中最新有效记录；上电调用一次 */
void Nvm_Init(void);

/* 读取最新有效载荷：返回 1 成功（buf 至少 max_len 字节），0 表示无有效记录 */
uint8_t Nvm_Load(uint8_t* buf, uint16_t max_len, uint16_t* out_len);

/* 写入载荷到非活动页并回读校验：返回 1 成功，0 失败（旧页不受影响） */
uint8_t Nvm_Save(const uint8_t* buf, uint16_t len);

/* 最近一次成功保存的载荷长度（无记录时为 0） */
uint16_t Nvm_LastLen(void);

#ifdef __cplusplus
}
#endif

#endif /* __NVM_H */
