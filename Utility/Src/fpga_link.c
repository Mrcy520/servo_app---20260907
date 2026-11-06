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

/* 最近一帧被接受的原始字节（供现场核对量纲/字节序，见 FpgaLink_GetRawRx） */
static uint8_t s_raw_frame[FPGA_FRAME_LEN];

/* 最近一个 DMA 窗口的原始字节，**不论是否解出帧头**。
 * 链路完全收不到有效帧时（rx=0、slip 疯涨），这才是唯一能说明问题的数据：
 * 全 0x00/0xFF 表示 FPGA 根本没驱动 MISO；有数据但无 0xAA55 表示错位或时序不符。 */
static uint8_t s_last_win[FPGA_FRAME_LEN];
static volatile uint8_t s_last_win_valid;

/* 角度跳变检测：上一帧实时帧的角度（只在 ISR 里读写，无需 volatile）。
 * 放在解析路径而不是调试层，是为了让计数不受 2 ms 采样率影响。 */
static uint32_t s_prev_angle;
static uint8_t  s_prev_angle_valid;

/* 位相位缓存：实测该 FPGA 的 MISO 起始位相位在不同上电周期里会变（见过 0/1/4 位），
 * 但在一个上电周期内稳定。因此"检测一次 + 缓存"，而不是每帧穷举。
 * 检测只在窗口里找不到帧头时做一次，且必须通过帧头/状态校验才采用，
 * 所以正常相位（0）下这条路径永远不会被激活。 */
static uint8_t  s_phase;      /* 0 = 原样；其余见 fpga_detect_phase 的返回值 */
static uint8_t  s_phase_lock;
static uint16_t s_phase_fail; /* 锁定后连续失败计数，超限则重新检测 */

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
volatile uint32_t g_fpga_recover; /* SPI 状态机被强制复位的次数（自愈计数） */

/* 最近一次发起失败的现场快照，按字节打包：
 *   bit7:0   HAL_SPI_TransmitReceive_DMA 的返回码（HAL_OK=0/BUSY=2/ERROR=1/TIMEOUT=3）
 *   bit15:8  hspi3.State
 *   bit23:16 hspi3.Lock
 *   bit31:24 hspi3.hdmarx->State
 * 有了它就不必再猜"为什么 HAL 返回失败"。 */
volatile uint32_t g_fpga_err_detail;

/* 时序诊断（DWT 周期计数器） */
volatile uint32_t g_fpga_sync_us_min = 0xFFFFFFFFu;
volatile uint32_t g_fpga_sync_us_max;
volatile uint32_t g_fpga_busy_us_min = 0xFFFFFFFFu;
volatile uint32_t g_fpga_busy_us_max;
volatile uint32_t g_fpga_sck_khz;

/* SPI 占用窗口的分段测量（DWT）：
 *   busy = s_ts_start → 完成回调      整个"下个同步沿会被拒绝"的窗口
 *   hal  = 发起 → HAL 调用返回         纯软件开销（-O0 下 HAL 入口 + DMA 启动）
 * 于是  传输 + 中断延迟 = busy - hal，可直接判断慢在硬件还是慢在软件。
 * 若 hal 很小而 busy 仍 ~51 µs，说明 SCK 实际没跑到 9 MHz；
 * 若 hal 就占 20 µs 以上，则是 -O0 下 HAL 调用自身的开销，需绕开 HAL。 */
volatile uint32_t g_fpga_hal_us_min = 0xFFFFFFFFu;
volatile uint32_t g_fpga_hal_us_max;

/* 被拒绝那一刻"距上次发起过了多久"（µs）。这是区分两种丢帧原因的唯一判据：
 *   ≈ 60~62  → 上次交换跑满了一整个周期，说明窗口太长（要压软件开销）；
 *   ≈ 20~30  → 沿在交换还没做完时就来了，说明沿间隔本身短于 62.5 µs
 *              （FPGA 侧抖动，或本次沿自身被中断延迟推后），压窗口无用。
 * 只看 ovr 计数永远分不开这两类，故单独记 min/max。 */
volatile uint32_t g_fpga_rej_us_min = 0xFFFFFFFFu;
volatile uint32_t g_fpga_rej_us_max;

/* nibble 交换纠正次数。正常应恒为 0；若持续增长，说明该 FPGA 这一次上电
 * 就工作在"半字节互换"相位——数据仍是可用的（已纠正），但值得反馈给 FPGA 侧。 */
volatile uint32_t g_fpga_nib_swap;

/* 实时帧角度跳变计数（见头文件说明）。只在解析路径里累加，与调试采样率无关。 */
volatile uint32_t g_fpga_angle_glitch;

/* 位相位判定/变更次数（每次确定一个相位 +1）。正常应在上电后很快稳定为 1 次。 */
volatile uint32_t g_fpga_phase_cnt;

/* 同步沿间隔直方图（每 5 s 由诊断层取走并清零）。
 * 稳态 16 kHz = 62.5 µs，样本应几乎全部落在 56~72 这一档。
 * 若出现大量 <40 µs 的样本，说明"沿与沿之间"本身就不均匀（FPGA 抖动，
 * 或中断延迟抖动），此时单靠压缩 SPI 窗口无法做到零丢帧——
 * 这一条把"能不能追上"和"追得慢"两件事分开，避免继续凭 spi 数值猜。 */
static volatile uint16_t s_sync_hist[FPGA_SYNC_HIST_N];

static void FpgaLink_HistAdd(uint32_t us)
{
    uint8_t i;

    if (us < 20u) {
        i = 0u;
    } else if (us < 40u) {
        i = 1u;
    } else if (us < 56u) {
        i = 2u;
    } else if (us < 72u) {
        i = 3u;
    } else if (us < 88u) {
        i = 4u;
    } else {
        i = 5u;
    }

    if (s_sync_hist[i] != 0xFFFFu) {
        s_sync_hist[i]++;
    }
}

static volatile uint8_t s_dwt_ok;
static volatile uint32_t s_ts_prev_edge; /* 上一次同步沿的时间戳 */
static volatile uint32_t s_ts_start;     /* 本次发起 DMA 前的时刻 */

#define FPGA_CYC_TO_US(cyc) ((uint32_t)((cyc) / (SystemCoreClock / 1000000u)))

/* 最近一次有效帧所处的周期号 */
static volatile uint32_t s_last_rx_cycle;

/* 半字节交换（nibble swap）。用于纠正 FPGA 在某些上电相位下的输出错位：
 * 实测该现象下每个字节的高低半字节互换，整帧交换后与正常帧逐字节一致。 */
#define FPGA_NIB_SWAP(b) ((uint8_t)(((uint8_t)(b) >> 4) | (uint8_t)((uint8_t)(b) << 4)))

/* 私有函数声明 ------------------------------------------------------------ */
static void FpgaLink_StartExchange(void);
static void FpgaLink_RxWindow(const uint8_t* win);
static void FpgaLink_Parse(const uint8_t* frame);
static void FpgaLink_RecoverSpi(void);

/* 一次传输被判定"卡死"的门限：8 个同步周期。超过就强制复位 SPI/DMA。
 * 没有这条自愈路径时，任何一次失败都会让链路永久死亡（cycle 不再增长、
 * ovr 每秒 +16000），现场只能靠复位恢复。 */
#define FPGA_LINK_STUCK_US 500u

/* 实时帧角度跳变门限：SPAN(0x2D0000) 对应 360°，20° ≈ 163840 counts。
 * 取 20° 的理由：10 kHz 级采样下相邻两帧的机械位移远小于此（本台架 30 s 才走 0.01°），
 * 20° 只可能来自数据异常，不会误伤真实运动。 */
#define FPGA_LINK_ANGLE_GLITCH 163840u

/* 锁定相位后，连续多少帧找不到帧头才重新判定相位（FPGA 重新上电可能换相位）。
 * 取 64 ≈ 4 ms，足够区分"偶发错帧"与"相位真的变了"。 */
#define FPGA_LINK_PHASE_RETRY 64u

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

    /* 时序诊断：打开 DWT 周期计数器（Cortex-M4 内核自带，零额外硬件） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    s_dwt_ok = 1u;

    s_ts_prev_edge     = 0u;
    s_ts_start         = DWT->CYCCNT; /* 不能留 0，否则首个沿会误判"卡死" */
    g_fpga_sync_us_min = 0xFFFFFFFFu;
    g_fpga_sync_us_max = 0u;
    g_fpga_busy_us_min = 0xFFFFFFFFu;
    g_fpga_busy_us_max = 0u;
    g_fpga_hal_us_min  = 0xFFFFFFFFu;
    g_fpga_hal_us_max  = 0u;
    g_fpga_rej_us_min  = 0xFFFFFFFFu;
    g_fpga_rej_us_max  = 0u;
    g_fpga_nib_swap    = 0u;
    s_phase            = 0u;
    s_prev_angle       = 0u;
    s_prev_angle_valid = 0u;
    g_fpga_angle_glitch = 0u;
    s_phase_lock       = 0u;
    s_phase_fail       = 0u;
    g_fpga_phase_cnt   = 0u;
    (void)memset((void*)s_sync_hist, 0, sizeof(s_sync_hist));
    g_fpga_recover    = 0u;
    g_fpga_err_detail = 0u;

    /* ===== 时序参数就地改写 =====
     * 这里**只改寄存器**，绝不调用 HAL_SPI_Init()：实测完整重初始化会把 HAL 的
     * 状态机打乱——首次传输再也回不来（cycle 停在 1、rx=0、spi 无样本），
     * 之后每个同步沿都被"忙"拒绝，链路永久死亡。此刻 SPI 尚未使能，直接改位域是安全的。 */

#ifndef FPGA_LINK_DISABLE_NSSP
#define FPGA_LINK_DISABLE_NSSP 0
#endif
#ifndef FPGA_LINK_SCK_DIV2
#define FPGA_LINK_SCK_DIV2 0
#endif

#if FPGA_LINK_DISABLE_NSSP
    /* 【已实测否证，默认关闭，勿再打开】
     * 曾假设 spi_min=58 µs（理论 28.4 的 2 倍）来自 NSSP 每字节插 NSS 脉冲。
     * 实测关闭 NSSP 后：spi_min 只从 58 降到 51（不是 29），且 FPGA 立刻不再
     * 驱动 MISO——[WIN] 变成 32 个 0xAA、rx=0、err=slip=cycle。
     * 结论：① 翻倍不是 NSSP 造成的；② 该 FPGA 依赖 NSS 边沿分帧，NSSP 不能关。 */
    CLEAR_BIT(SPI3->CR2, SPI_CR2_NSSP);
    hspi3.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
#endif

#if FPGA_LINK_SCK_DIV2
    /* 把 SCK 从 PCLK1/4 提到 PCLK1/2（9 MHz → 18 MHz）。
     *
     * 【实测结论：18 MHz 该 FPGA 用不了，默认关闭，勿再打开】
     * 13:32 实测（锁泄漏已修、hal=7/8us、spi=33/53us、ovr=0、rec=0）：
     *   时序问题确实消失（窗口装得下），但 [WIN] 收到的 32 字节几乎全是 0x00，
     *   只有前 2 字节在变（75 6A / DD 3A / D5 2E …），rx=0、err=slip=cycle。
     *   即 MISO 基本没数据 —— 不是帧头错位，是从机输出无效。
     * 9 MHz 下同一套代码收数完全正常，说明**不是我们的启动/解析问题**，
     * 而是该 FPGA 的 SPI 从机（或其连线信号完整性）做不到 18 MHz
     * （接口文档写"按 30 MHz 设计"，与实测不符，可拿这条去核对）。
     *
     * 这条实测同时证明了一件事：18 MHz 时 ovr=0，说明我们软件侧的开销
     * 已经不是瓶颈，窗口 24 µs 装得进 25 µs 的短间隙。
     * 也就是说——只要 FPGA 能吃住更高 SCK，丢帧问题立刻消失。 */
    MODIFY_REG(SPI3->CR1, SPI_CR1_BR, SPI_BAUDRATEPRESCALER_2);
    hspi3.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
#endif

    /* 中断优先级在这里设置，而不是改 CubeMX 生成的 gpio.c / spi.c（重新生成会被覆盖）。
     *
     * EXTI（同步沿）必须与 DMA 同级或更高：原先是 DMA=0 / EXTI=2，
     * 一旦 DMA 完成中断正在执行，同步沿会被推迟，交换窗口整体后移，
     * 进而挤到下个周期造成 overrun。两条 ISR 都很短，同级不会互相饿死。
     *
     * SPI3 的错误中断必须打开：HAL_SPI_TransmitReceive_DMA 使能了 SPI_IT_ERR，
     * 但原工程从未在 NVIC 里使能 SPI3_IRQn，导致 OVR/MODF 发生时
     * HAL_SPI_ErrorCallback 永远不执行、标志也清不掉。 */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 0, 0);
    HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 0, 0);
    HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 0, 0);

    /* SPI3 错误中断：恢复为 10:51 那个可用版本的配置（使能）。
     * 它会执行 HAL_SPI_ErrorCallback → 清 OVR 标志。若 NVIC 不使能，
     * 溢出标志一旦置位就没人清，后续接收数据会被持续破坏。 */
#ifndef FPGA_LINK_SPI_ERR_IRQ
#define FPGA_LINK_SPI_ERR_IRQ 1
#endif

#if FPGA_LINK_SPI_ERR_IRQ
    HAL_NVIC_SetPriority(SPI3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(SPI3_IRQn);
#endif

    /* 现场读出 SCK 实际频率：CR1.BR 的编码是 0=/2、1=/4、2=/8 …
     * 只信寄存器，不按 CubeMX 里的分频值推算（时钟源可能与预期不同）。 */
    {
        uint32_t br = (SPI3->CR1 & SPI_CR1_BR) >> SPI_CR1_BR_Pos;

        g_fpga_sck_khz = (HAL_RCC_GetPCLK1Freq() >> (br + 1u)) / 1000u;
    }

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

    /* 写非活动缓冲，再原子切换下标：中断读到的始终是完整帧。
     * 本文件编成 -O2（见 CMakeLists），编译器可能把 pack_cmd 的写回
     * 重排到 s_tx_active 之后，那样中断会读到"一半旧一半新"的帧。
     * 这里插一条编译屏障强制顺序（只需挡住编译器，不需要关中断）。 */
    next = (uint8_t)(s_tx_active ^ 1u);
    fpga_pack_cmd(s_tx_buf[next], cmd);

    __asm__ volatile("" ::: "memory");

    s_tx_active = next;
}

/**
 * @brief 强制复位 SPI3 / DMA 状态机（自愈）
 * @note  运行于 EXTI 中断（优先级 0），**只能用纯寄存器操作**。
 *
 * 这里绝对不能用 HAL_DMA_Abort()：HAL 的 Abort 用 HAL_GetTick() 做超时轮询，
 * 而本函数在最高优先级中断里执行，SysTick（优先级最低）无法抢占，
 * tick 永远不前进 —— 只要 EN 位没有立刻清零就会死循环，整个系统原地卡死。
 * 现场已复现：烧录后打印到 "-- event --" 就再无输出。
 */
static void FpgaLink_RecoverSpi(void)
{
    if (hspi3.hdmarx != NULL) {
        __HAL_DMA_DISABLE(hspi3.hdmarx);
        hspi3.hdmarx->State = HAL_DMA_STATE_READY;
        /* 【关键】必须同时解锁。
         * HAL_DMA_Start_IT 在**成功路径上不释放锁**（只有 else 分支 unlock），
         * 锁是由 DMA 完成中断里的 HAL_DMA_IRQHandler 释放的。
         * 一旦完成中断丢失、我们又在这里强制复位了通道，锁就永久停在 HAL_LOCKED，
         * 此后每次 HAL_DMA_Start_IT 都直接 return HAL_BUSY（连 State 都不改），
         * 于是 HAL_SPI_TransmitReceive_DMA 永远返回 HAL_ERROR —— 自愈反而变成永久死锁。
         * 实测现象：18 MHz 下 err=cycle、rx=0、spi=0/0、e=0x01000501
         * （低字节 01=HAL_ERROR，次字节 05=State 仍 BUSY_TX_RX，最高字节 01=hdmarx
         *   State 却是 READY，正是"被锁挡住、状态没动"的指纹）。 */
        hspi3.hdmarx->Lock = HAL_UNLOCKED;
    }
    if (hspi3.hdmatx != NULL) {
        __HAL_DMA_DISABLE(hspi3.hdmatx);
        hspi3.hdmatx->State = HAL_DMA_STATE_READY;
        hspi3.hdmatx->Lock  = HAL_UNLOCKED;
    }

    __HAL_SPI_DISABLE(&hspi3);
    /* 完整静默：只关 SPE 不够——上次中断的传输会把 TXDMAEN/RXDMAEN 和 ERR 中断
     * 留在使能状态，下一次发起时会立刻带上陈旧的 DMA 请求与错误标志。 */
    CLEAR_BIT(SPI3->CR2, SPI_CR2_TXDMAEN | SPI_CR2_RXDMAEN | SPI_CR2_ERRIE);
    __HAL_SPI_CLEAR_OVRFLAG(&hspi3);

    hspi3.State     = HAL_SPI_STATE_READY;
    hspi3.ErrorCode = HAL_SPI_ERROR_NONE;

    /* 流缓冲可能停在半帧，交给下一窗口的帧头搜索自恢复 */
    s_rd = 0u;
    s_wr = 0u;

    g_fpga_recover++;
}

/**
 * @brief 帧同步沿到：发起一次 32 字节全双工交换
 * @note  运行于 EXTI 中断
 */
static void FpgaLink_StartExchange(void)
{
    uint32_t now = (s_dwt_ok != 0u) ? DWT->CYCCNT : 0u;

    if (s_dwt_ok != 0u) {
        uint32_t d = now - s_ts_prev_edge; /* 无符号减法，跨 32 位回绕也正确 */

        if (s_ts_prev_edge != 0u) {
            uint32_t us = FPGA_CYC_TO_US(d);

            if (us < g_fpga_sync_us_min) {
                g_fpga_sync_us_min = us;
            }
            if (us > g_fpga_sync_us_max) {
                g_fpga_sync_us_max = us;
            }

            FpgaLink_HistAdd(us);
        }
        s_ts_prev_edge = now;
    }

    if (hspi3.State != HAL_SPI_STATE_READY) {
        /* 正常的"上一帧还没收完"：跳过本周期，等下一个同步沿。
         * 但若"忙"已经持续远超一个周期（DMA 中断丢失、SPI 状态机卡住），
         * 就强制复位——否则链路会静默死掉：cycle 不再增长、ovr 每秒 +16000。 */
        if (s_dwt_ok != 0u) {
            uint32_t el = FPGA_CYC_TO_US(now - s_ts_start);

            if (el < g_fpga_rej_us_min) {
                g_fpga_rej_us_min = el;
            }
            if (el > g_fpga_rej_us_max) {
                g_fpga_rej_us_max = el;
            }
        }

        if ((s_dwt_ok != 0u) && (FPGA_CYC_TO_US(now - s_ts_start) > FPGA_LINK_STUCK_US)) {
            FpgaLink_RecoverSpi();
        } else {
            g_fpga_overrun++;
        }
        return;
    }

    (void)memcpy(s_tx_local, s_tx_buf[s_tx_active], FPGA_FRAME_LEN);

    g_fpga_cycle++;

    /* 记录发起时刻，用于统计"SPI 占用窗口"——这才是决定会不会丢帧的量 */
    s_ts_start = now;

    {
        HAL_StatusTypeDef st =
                HAL_SPI_TransmitReceive_DMA(&hspi3, s_tx_local, s_rx_win, (uint16_t)FPGA_FRAME_LEN);

        if (s_dwt_ok != 0u) {
            uint32_t us = FPGA_CYC_TO_US(DWT->CYCCNT - s_ts_start);

            if (us < g_fpga_hal_us_min) {
                g_fpga_hal_us_min = us;
            }
            if (us > g_fpga_hal_us_max) {
                g_fpga_hal_us_max = us;
            }
        }

        if (st != HAL_OK) {
            g_fpga_err++;
            /* 现场快照按字节：低→高
             *   [7:0]   HAL 返回码（0=OK 1=ERROR 2=BUSY 3=TIMEOUT）
             *   [15:8]  hspi3.State（1=READY 3=BUSY_TX 5=BUSY_TX_RX）
             *   [23:16] hdmarx->State（1=READY 2=BUSY）
             *   [31:24] hdmarx->Lock（0=UNLOCKED 1=LOCKED）
             * 最后两项是判定"锁泄漏"的指纹：返回 HAL_ERROR 而 DMA State 仍是 READY、
             * Lock=LOCKED，就说明被锁挡住，而不是 DMA 真的忙。 */
            g_fpga_err_detail =
                    ((uint32_t)st & 0xFFu) | (((uint32_t)hspi3.State & 0xFFu) << 8) |
                    (((uint32_t)((hspi3.hdmarx != NULL) ? hspi3.hdmarx->State : 0xFFu) & 0xFFu)
                     << 16) |
                    (((uint32_t)((hspi3.hdmarx != NULL) ? hspi3.hdmarx->Lock : 0xFFu) & 0xFFu)
                     << 24);
            /* HAL 失败路径会把 State 留在 BUSY_xxx，而完成回调永远不会来。
             * 不在这里复位，第一次失败就等于链路永久死亡。 */
            FpgaLink_RecoverSpi();
        }
    }
}

/* -------------------------------------------------------------------------- */
/* 接收与解帧                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 把一个 32 字节接收窗口并入字节流并尝试解出完整帧
 * @note  运行于 DMA 完成中断
 */
static void FpgaLink_RxWindow(const uint8_t* win_in)
{
    uint8_t  fixed[FPGA_FRAME_LEN];
    const uint8_t* win = win_in;
    uint16_t i;

    /* 先留**原始**窗口：解不出帧时它是唯一的现场证据（必须是线上字节，不能是还原后的） */
    (void)memcpy(s_last_win, win_in, FPGA_FRAME_LEN);
    s_last_win_valid = 1u;

    /* 已锁定非零相位：先还原成规范帧再走原有流程 */
    if (s_phase != 0u) {
        fpga_apply_phase(win_in, s_phase, fixed);
        win = fixed;
    }

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

        /* 搜索帧头，并要求状态字节合法以降低误锁概率。
         * 注意：状态字节也要接受"nibble 交换后合法"的情况——该 FPGA 存在
         * 把每个字节高低半字节互换后输出的相位（0x30=0x03、0x40=0x04）。
         * 因为 AA/55 是半字节回文，帧头不受影响；若这里只认原样合法，
         * 这种相位下会一个帧头都锁不到（表现为 err=slip=cycle、rx=0）。 */
        for (i = 0u; (uint16_t)(i + 1u) < avail; i++) {
            if ((s_stream[s_rd + i] == FPGA_HDR_RX0) && (s_stream[s_rd + i + 1u] == FPGA_HDR_RX1)) {
                if (((uint16_t)(i + 2u) < avail) &&
                    (FPGA_ST_IS_VALID(s_stream[s_rd + i + 2u]) == 0u) &&
                    (FPGA_ST_IS_VALID(FPGA_NIB_SWAP(s_stream[s_rd + i + 2u])) == 0u)) {
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

            /* 位相位自愈：找不到帧头时判一次相位。判定必须通过"帧头/状态"校验，
             * 校验不过就保持原样（0xFF），因此不会把好的相位改坏。
             * 相位一旦确定就锁定，只在连续失败过多（例如 FPGA 重新上电换了相位）时重判。 */
            if (++s_phase_fail >= FPGA_LINK_PHASE_RETRY) {
                uint8_t ph = fpga_detect_phase(s_last_win);

                s_phase_fail = 0u;
                if (ph != 0xFFu) {
                    if ((ph != s_phase) || (s_phase_lock == 0u)) {
                        g_fpga_phase_cnt++;
                    }
                    s_phase      = ph;
                    s_phase_lock = 1u;
                }
            }
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
    uint8_t swapped[FPGA_FRAME_LEN];
    const uint8_t* f = frame;
    uint8_t i;

    /* 半字节相位兜底（实测该 FPGA 存在这种输出相位）：
     * 某些上电周期里，它把**每个字节的 nibble 交换**后发出——实测 0x30 = 0x03 交换、
     * 0x40 = 0x04 交换，且整帧 24 字节与往次正常帧逐字节完全一致（见下）。
     * 惩罚很小的原因：AA=0xAA、55=0x55 都是半字节回文，交换后帧头不变，
     * 所以滑窗照样锁得到帧头，只有第 3 字节会变成非法的 0x30/0x40。
     * 判据就用这一条：正常解不出 state 时，交换 nibble 再解一次。
     * 这样 FPGA 无论进入哪个相位都能工作，不必要求现场断电重来。 */
    if (FPGA_ST_IS_VALID(frame[2]) == 0u) {
        for (i = 0u; i < (uint8_t)FPGA_FRAME_LEN; i++) {
            swapped[i] = FPGA_NIB_SWAP(frame[i]);
        }
        if (FPGA_ST_IS_VALID(swapped[2]) != 0u) {
            f = swapped;
            g_fpga_nib_swap++;
        }
    }

    if (fpga_unpack_rx(f, &s_rx) == 0u) {
        g_fpga_err++;
        return;
    }

    s_rx_valid      = 1u;
    s_last_rx_cycle = g_fpga_cycle;
    g_fpga_rx_cnt++;

    /* 保留原始字节：调试层可按线上字节核对量纲，而不是看解码后的结构体。
     * 这里存的是**纠正后**的帧，[WIN] 才是线上的原始 32 字节。 */
    (void)memcpy(s_raw_frame, f, FPGA_FRAME_LEN);

    /* 实时帧角度跳变统计：**在解析路径里按帧统计**，不依赖调试层的 2 ms 采样，
     * 因此这个计数反映"线上真实发生了多少次"，可以用来判断调试层看到的次数是否被采样率
     * 掩盖。按圆周差计算，避免 0xB40000->0x870000 的正常回绕被误记成跳变。 */
    if (s_rx.state == FPGA_ST_COMM) {
        if (s_prev_angle_valid != 0u) {
            int32_t d = (int32_t)s_rx.angle_raw - (int32_t)s_prev_angle;

            d %= (int32_t)FPGA_ANGLE_SPAN;
            if (d > (int32_t)(FPGA_ANGLE_SPAN / 2u)) {
                d -= (int32_t)FPGA_ANGLE_SPAN;
            } else if (d < -(int32_t)(FPGA_ANGLE_SPAN / 2u)) {
                d += (int32_t)FPGA_ANGLE_SPAN;
            }

            if ((d > (int32_t)FPGA_LINK_ANGLE_GLITCH) || (d < -(int32_t)FPGA_LINK_ANGLE_GLITCH)) {
                g_fpga_angle_glitch++;
            }
        }
        s_prev_angle       = s_rx.angle_raw;
        s_prev_angle_valid = 1u;
    }

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

uint8_t FpgaLink_GetRxEx(fpga_rx_t* out, uint8_t* raw)
{
    uint8_t  valid;
    uint32_t primask;

    if ((out == NULL) && (raw == NULL)) {
        return 0u;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    if (out != NULL) {
        *out = s_rx;
    }
    if (raw != NULL) {
        (void)memcpy(raw, s_raw_frame, FPGA_FRAME_LEN);
    }
    valid = s_rx_valid;

    if (primask == 0u) {
        __enable_irq();
    }

    return valid;
}

uint8_t FpgaLink_GetRawRx(uint8_t* out)
{
    uint8_t valid;
    uint32_t primask;

    if (out == NULL) {
        return 0u;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    (void)memcpy(out, s_raw_frame, FPGA_FRAME_LEN);
    valid = s_rx_valid;

    if (primask == 0u) {
        __enable_irq();
    }

    return valid;
}

uint8_t FpgaLink_GetLastWindow(uint8_t* out)
{
    uint8_t valid;
    uint32_t primask;

    if (out == NULL) {
        return 0u;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    (void)memcpy(out, s_last_win, FPGA_FRAME_LEN);
    valid = s_last_win_valid;

    if (primask == 0u) {
        __enable_irq();
    }

    return valid;
}

uint8_t FpgaLink_GetLastTx(uint8_t* out)
{
    if (out == NULL) {
        return 0u;
    }

    /* 中断里会改写 s_tx_local，拷贝期间关一下中断（32 字节，代价可忽略） */
    {
        uint32_t primask = __get_PRIMASK();

        __disable_irq();
        (void)memcpy(out, s_tx_local, FPGA_FRAME_LEN);
        if (primask == 0u) {
            __enable_irq();
        }
    }

    return 1u;
}

uint8_t FpgaLink_TakeSyncHist(uint16_t* out)
{
    uint32_t primask;
    uint8_t i;
    uint8_t any = 0u;

    if (out == NULL) {
        return 0u;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    for (i = 0u; i < (uint8_t)FPGA_SYNC_HIST_N; i++) {
        out[i] = s_sync_hist[i];
        if (s_sync_hist[i] != 0u) {
            any = 1u;
        }
        s_sync_hist[i] = 0u; /* 取走即清零，得到的是"最近一个窗口"的分布 */
    }

    if (primask == 0u) {
        __enable_irq();
    }

    return any;
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

    /* 先结算时序：此刻 HAL 已把 State 置回 READY，
     * 所以这里量到的就是"SPI 被占用、下个同步沿会被拒绝"的真实窗口 */
    if (s_dwt_ok != 0u) {
        uint32_t us = FPGA_CYC_TO_US(DWT->CYCCNT - s_ts_start);

        if (us < g_fpga_busy_us_min) {
            g_fpga_busy_us_min = us;
        }
        if (us > g_fpga_busy_us_max) {
            g_fpga_busy_us_max = us;
        }
    }

    FpgaLink_RxWindow(s_rx_win);
}

/**
 * @brief SPI3 中断入口
 * @note  只为错误路径服务：HAL_SPI_TransmitReceive_DMA 使能了 SPI_IT_ERR，
 *        没有这个入口时 OVR/MODF 只会挂起而永远处理不到。
 *        若日后在 CubeMX 里勾选了 SPI3 全局中断，会与 stm32f3xx_it.c 的生成版本
 *        重名，届时删掉本函数即可（本函数只是转调 HAL_SPI_IRQHandler）。
 */
void SPI3_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&hspi3);
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
