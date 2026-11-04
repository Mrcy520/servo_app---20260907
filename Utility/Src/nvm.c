/**
 ******************************************************************************
 * @file    nvm.c
 * @brief   内部 Flash 掉电保存 · 双页轮换实现（HAL）
 *
 * 为什么用两页轮换：
 *   单页方案在"擦除后、写入完成前"掉电会同时失去新旧参数；两页轮换保证任何时刻
 *   至少有一页是完整有效的记录。写入顺序为：擦目标页 -> 写记录 -> 回读校验 ->
 *   切换活动页，校验不通过就丢弃目标页，旧页继续可用。
 *
 * 说明：擦除一页约 20~40 ms，期间 CPU 停住、主循环不再执行（接收 DMA 仍在搬运，
 *       但环形缓冲只有 64 字节，约 6 ms 就会绕回）。因此保存动作必须低频、且只在
 *       输出关闭时发生，由应用层控制节奏（见 comm_app.c 的 NvmService）。
 ******************************************************************************
 */

#include "nvm.h"

#include "main.h"
#include "crc.h"

#include <string.h>

#define NVM_MAGIC 0x314D564Eu /* 'NVM1'（小端存储，读回比对即可，不参与上电判断） */

volatile uint32_t g_stat_nvm_err;

/* 当前活动页、序号、载荷长度 */
static uint32_t s_active = NVM_PAGE_B; /* 无记录时首次保存写到 A 页 */
static uint32_t s_seq;
static uint16_t s_len;

/* -------------------------------------------------------------------------- */
/* 内部工具                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 校验指定页是否为一条完整记录
 * @param addr  页起始地址
 * @param len   输出：载荷长度
 * @param seq   输出：序号
 * @retval 1 有效
 */
static uint8_t Nvm_IsValid(uint32_t addr, uint16_t* len, uint32_t* seq)
{
    const uint8_t* p = (const uint8_t*)addr;
    uint32_t magic;
    uint16_t payload_len;
    uint16_t crc_stored;

    (void)memcpy(&magic, p, sizeof(magic));
    if (magic != NVM_MAGIC) {
        return 0u;
    }

    (void)memcpy(&payload_len, p + 8, sizeof(payload_len));
    if ((payload_len == 0u) || (payload_len > NVM_PAYLOAD_MAX)) {
        return 0u;
    }

    (void)memcpy(&crc_stored, p + NVM_CRC_OFF, sizeof(crc_stored));
    if (crc16_checksum(p, NVM_CRC_OFF) != crc_stored) {
        return 0u;
    }

    (void)memcpy(seq, p + 4, sizeof(*seq));
    *len = payload_len;
    return 1u;
}

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

void Nvm_Init(void)
{
    uint16_t len_a = 0u;
    uint16_t len_b = 0u;
    uint32_t seq_a = 0u;
    uint32_t seq_b = 0u;
    uint8_t ok_a;
    uint8_t ok_b;

    g_stat_nvm_err = 0u;

    ok_a = Nvm_IsValid(NVM_PAGE_A, &len_a, &seq_a);
    ok_b = Nvm_IsValid(NVM_PAGE_B, &len_b, &seq_b);

    if ((ok_a != 0u) && (ok_b != 0u)) {
        /* 两页都有效：取序号大的；序号相同时以 A 页为准（理论上不会发生） */
        if (seq_a >= seq_b) {
            s_active = NVM_PAGE_A;
            s_seq    = seq_a;
            s_len    = len_a;
        } else {
            s_active = NVM_PAGE_B;
            s_seq    = seq_b;
            s_len    = len_b;
        }
    } else if (ok_a != 0u) {
        s_active = NVM_PAGE_A;
        s_seq    = seq_a;
        s_len    = len_a;
    } else if (ok_b != 0u) {
        s_active = NVM_PAGE_B;
        s_seq    = seq_b;
        s_len    = len_b;
    } else {
        /* 首次上电：无有效记录，下次保存写 A 页 */
        s_active = NVM_PAGE_B;
        s_seq    = 0u;
        s_len    = 0u;
    }
}

uint8_t Nvm_Load(uint8_t* buf, uint16_t max_len, uint16_t* out_len)
{
    if ((s_len == 0u) || (s_len > max_len) || (buf == NULL)) {
        return 0u;
    }

    (void)memcpy(buf, (const void*)(s_active + NVM_HDR_SIZE), s_len);
    if (out_len != NULL) {
        *out_len = s_len;
    }
    return 1u;
}

uint16_t Nvm_LastLen(void)
{
    return s_len;
}

uint8_t Nvm_Save(const uint8_t* buf, uint16_t len)
{
    uint32_t target;
    uint32_t words[NVM_SLOT_SIZE / 4u];
    uint8_t* p                   = (uint8_t*)words;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error          = 0u;
    uint16_t chk_len             = 0u;
    uint32_t chk_seq             = 0u;
    uint16_t i;

    if ((buf == NULL) || (len == 0u) || (len > NVM_PAYLOAD_MAX)) {
        g_stat_nvm_err++;
        return 0u;
    }

    /* 目标页 = 非活动页：写坏了也不影响当前有效记录 */
    target = (s_active == NVM_PAGE_A) ? NVM_PAGE_B : NVM_PAGE_A;

    /* 组记录：先全部填 0xFF（擦除态），再覆盖有效字段 */
    (void)memset(words, 0xFF, sizeof(words));

    {
        uint32_t magic = NVM_MAGIC;
        uint32_t seq   = s_seq + 1u;
        uint16_t plen  = len;
        uint16_t rsv   = 0xFFFFu;
        uint16_t crc;

        (void)memcpy(p, &magic, sizeof(magic));
        (void)memcpy(p + 4, &seq, sizeof(seq));
        (void)memcpy(p + 8, &plen, sizeof(plen));
        (void)memcpy(p + 10, &rsv, sizeof(rsv));
        (void)memcpy(p + NVM_HDR_SIZE, buf, len);
        crc = crc16_checksum(p, NVM_CRC_OFF);
        (void)memcpy(p + NVM_CRC_OFF, &crc, sizeof(crc));
    }

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);

    erase.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = target;
    erase.NbPages     = 1u;

    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
        HAL_FLASH_Lock();
        g_stat_nvm_err++;
        return 0u;
    }

    for (i = 0u; i < (NVM_SLOT_SIZE / 4u); i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, target + ((uint32_t)i * 4u), words[i]) !=
            HAL_OK) {
            HAL_FLASH_Lock();
            g_stat_nvm_err++;
            return 0u;
        }
    }

    HAL_FLASH_Lock();

    /* 回读校验：不通过就当这次没写成功，旧页仍然有效 */
    if (Nvm_IsValid(target, &chk_len, &chk_seq) == 0u) {
        g_stat_nvm_err++;
        return 0u;
    }
    if (chk_len != len) {
        g_stat_nvm_err++;
        return 0u;
    }

    s_active = target;
    s_seq    = chk_seq;
    s_len    = chk_len;
    return 1u;
}
