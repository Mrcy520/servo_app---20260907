/**
 ******************************************************************************
 * @file    fpga_link.c
 * @brief   FPGA <-> MCU 内部接口 · 链路层实现（HAL / SPI3 / DMA / EXTI）
 *
 * 关键约束：
 *   1. 一次交换只能由 FPGA 的 16 kHz 同步沿发起。脱离同步沿自行发起会让
 *      双方字节计数错位，因此 FpgaLink_Init() 不主动启动任何传输。
 *   2. 收发都用 DMA1（RX = Ch2，TX = Ch3，见 Core/Src/spi.c 的 MspInit）。
 *   3. FPGA 的字节流可能与 DMA 窗口错位（实测存在 3~12 字节漂移），
 *      故不按固定偏移解帧，而是把每个窗口追加进 64 字节流缓冲后搜索帧头，
 *      帧头不在 0 位即丢弃错位前缀，从而在任意滑移后自动恢复。
 ******************************************************************************
 */

#include "fpga_link.h"

#include "spi.h"
#include "main.h"

#include <string.h>

/* -------------------------------------------------------------------------- */
/* 内部状态                                                                    */
/* -------------------------------------------------------------------------- */

/* 发送双缓冲：主循环写非活动区，中断读活动区，无需关中断 */
static uint8_t s_tx_buf[2][FPGA_FRAME_LEN];
static volatile uint8_t s_tx_active;
static uint8_t s_tx_local[FPGA_FRAME_LEN]; /* 中断内快照，避免主循环改写 */

/* DMA 接收窗口 */
static uint8_t s_rx_win[FPGA_FRAME_LEN];

/* 最新响应（中断写，主循环临界区读） */
static fpga_rx_t s_rx;
static volatile uint8_t s_rx_valid;

/* 滑窗重同步缓冲：s_rd..s_wr 为尚未消费的字节 */
static uint8_t s_stream[FPGA_FRAME_LEN * 2u];
static uint8_t s_rd;
static uint8_t s_wr;

/* 统计量 */
volatile uint32_t g_fpga_cycle;
volatile uint32_t g_fpga_rx_cnt;
volatile uint32_t g_fpga_err;
volatile uint32_t g_fpga_slip;
volatile uint32_t g_fpga_overrun;
volatile uint32_t g_fpga_ack_cnt;
volatile uint8_t g_fpga_ack_state;

/* 最近一次有效帧所处的周期号 */
static volatile uint32_t s_last_rx_cycle;

/* 私有函数声明 ------------------------------------------------------------ */
static void FpgaLink_StartExchange(void);
static void FpgaLink_RxWindow(const uint8_t* win);
static void FpgaLink_Parse(const uint8_t* frame);

/* -------------------------------------------------------------------------- */
/* 初始化                                                                      */
/* -------------------------------------------------------------------------- */

void FpgaLink_Init(void)
{
    fpga_cmd_t idle;

    s_tx_active = 0u;
    s_rd        = 0u;
    s_wr        = 0u;

    (void)memset(&s_rx, 0, sizeof(s_rx));
    s_rx_valid = 0u;

    g_fpga_cycle     = 0u;
    g_fpga_rx_cnt    = 0u;
    g_fpga_err       = 0u;
    g_fpga_slip      = 0u;
    g_fpga_overrun   = 0u;
    g_fpga_ack_cnt   = 0u;
    g_fpga_ack_state = 0u;

    s_last_rx_cycle = 0u;

    /* 安全空闲帧：正常通讯 + 等待（关闭使能） */
    (void)memset(&idle, 0, sizeof(idle));
    idle.state = FPGA_ST_COMM;
    idle.mode  = FPGA_MODE_WAIT;

    /* 预生成两路，保证首个同步沿就能取到完整帧 */
    fpga_pack_cmd(s_tx_buf[0], &idle);
    fpga_pack_cmd(s_tx_buf[1], &idle);

    /* 注意：EXTERNAL 中断（PA11 = nInt_Pin，下降沿）由 MX_GPIO_Init 配置，
     * 此处不重复配置，避免与 CubeMX 生成的 GPIO 初始化冲突。 */
}

/* -------------------------------------------------------------------------- */
/* 发送                                                                        */
/* -------------------------------------------------------------------------- */

void FpgaLink_SetCommand(const fpga_cmd_t* cmd)
{
    uint8_t next;

    if (cmd == NULL) {
        return;
    }

    /* 写非活动缓冲，再原子切换下标：中断读到的始终是完整帧 */
    next = (uint8_t)(s_tx_active ^ 1u);
    fpga_pack_cmd(s_tx_buf[next], cmd);
    s_tx_active = next;
}

/**
 * @brief 帧同步沿到：发起一次 32 字节全双工交换
 * @note  运行于 EXTI 中断
 */
static void FpgaLink_StartExchange(void)
{
    /* 上一周期尚未收完（正常不会发生：28.4 µs 传输 << 62.5 µs 周期），
     * 说明链路异常，跳过本周期而不打断进行中的 DMA，避免 SPI 状态错乱 */
    if (hspi3.State != HAL_SPI_STATE_READY) {
        g_fpga_overrun++;
        return;
    }

    (void)memcpy(s_tx_local, s_tx_buf[s_tx_active], FPGA_FRAME_LEN);

    g_fpga_cycle++;

    if (HAL_SPI_TransmitReceive_DMA(&hspi3, s_tx_local, s_rx_win, (uint16_t)FPGA_FRAME_LEN) !=
        HAL_OK) {
        g_fpga_err++;
    }
}

/* -------------------------------------------------------------------------- */
/* 接收与解帧                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 把一个 32 字节接收窗口并入字节流并尝试解出完整帧
 * @note  运行于 DMA 完成中断
 */
static void FpgaLink_RxWindow(const uint8_t* win)
{
    uint16_t i;

    /* 缓冲回绕时先搬移未消费字节 */
    if (((uint16_t)s_wr + (uint16_t)FPGA_FRAME_LEN) > (uint16_t)sizeof(s_stream)) {
        uint8_t used = (uint8_t)(s_wr - s_rd);

        (void)memmove(s_stream, &s_stream[s_rd], used);
        s_rd = 0u;
        s_wr = used;
    }

    (void)memcpy(&s_stream[s_wr], win, FPGA_FRAME_LEN);
    s_wr = (uint8_t)(s_wr + FPGA_FRAME_LEN);

    for (;;) {
        uint8_t avail = (uint8_t)(s_wr - s_rd);
        uint8_t found = 0xFFu;

        if (avail < FPGA_FRAME_LEN) {
            break; /* 不足一帧，等下一窗口 */
        }

        /* 搜索帧头，并要求状态字节合法以降低误锁概率 */
        for (i = 0u; (uint16_t)(i + 1u) < avail; i++) {
            if ((s_stream[s_rd + i] == FPGA_HDR_RX0) && (s_stream[s_rd + i + 1u] == FPGA_HDR_RX1)) {
                if (((uint16_t)(i + 2u) < avail) &&
                    (FPGA_ST_IS_VALID(s_stream[s_rd + i + 2u]) == 0u)) {
                    continue;
                }
                found = (uint8_t)i;
                break;
            }
        }

        if (found == 0xFFu) {
            /* 整窗无帧头：只保留最后 1 字节，防止帧头跨窗 */
            s_rd = (uint8_t)(s_wr - 1u);
            g_fpga_err++;
            g_fpga_slip++;
            break;
        }

        if ((uint16_t)(found + FPGA_FRAME_LEN) > (uint16_t)avail) {
            /* 帧头已找到但帧不完整：丢弃错位前缀，等下一窗口补齐 */
            if (found != 0u) {
                s_rd = (uint8_t)(s_rd + found);
                g_fpga_slip++;
            }
            break;
        }

        if (found != 0u) {
            s_rd = (uint8_t)(s_rd + found);
            g_fpga_slip++;
        }

        FpgaLink_Parse(&s_stream[s_rd]);
        s_rd = (uint8_t)(s_rd + FPGA_FRAME_LEN);

        if (s_rd == s_wr) {
            s_rd = 0u;
            s_wr = 0u;
        }
    }
}

/**
 * @brief 解析一帧响应并更新最新快照
 */
static void FpgaLink_Parse(const uint8_t* frame)
{
    if (fpga_unpack_rx(frame, &s_rx) == 0u) {
        g_fpga_err++;
        return;
    }

    s_rx_valid      = 1u;
    s_last_rx_cycle = g_fpga_cycle;
    g_fpga_rx_cnt++;

    /* 非实时响应（0x02/0x03/0x04）单独计数：应用层据此判定请求已回应 */
    if (s_rx.state != FPGA_ST_COMM) {
        g_fpga_ack_state = s_rx.state;
        g_fpga_ack_cnt++;
    }
}

/* -------------------------------------------------------------------------- */
/* 读取                                                                        */
/* -------------------------------------------------------------------------- */

uint8_t FpgaLink_GetRx(fpga_rx_t* out)
{
    uint8_t valid;
    uint32_t primask;

    if (out == NULL) {
        return 0u;
    }

    /* 仅在原中断状态为开时恢复，避免在临界区内误开中断 */
    primask = __get_PRIMASK();
    __disable_irq();

    *out  = s_rx;
    valid = s_rx_valid;

    if (primask == 0u) {
        __enable_irq();
    }

    return valid;
}

uint8_t FpgaLink_Alive(void)
{
    if (s_rx_valid == 0u) {
        return 0u;
    }
    return (uint8_t)((uint32_t)(g_fpga_cycle - s_last_rx_cycle) <= FPGA_LINK_TIMEOUT_CYCLES);
}

/* -------------------------------------------------------------------------- */
/* HAL 回调                                                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief GPIO 外部中断回调：FPGA 16 kHz 帧同步（PA11 下降沿）
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != nInt_Pin) {
        return;
    }

    FpgaLink_StartExchange();
}

/**
 * @brief SPI 全双工 DMA 完成：结算接收窗口
 */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi->Instance != SPI3) {
        return;
    }

    FpgaLink_RxWindow(s_rx_win);
}

/**
 * @brief SPI 错误：清溢出标志并计数，下一同步沿自动重试
 */
void HAL_SPI_ErrorCallback(SPI_HandleTypeDef* hspi)
{
    if (hspi->Instance != SPI3) {
        return;
    }

    g_fpga_err++;
    __HAL_SPI_CLEAR_OVRFLAG(hspi);
}
