/**
 *@file retarget.c
 *@author westin
 *@brief printf 重定向（默认走 RTT 通道 0，可切回 USART1）
 *@version 0.2
 *@date 2026-11-06
 *
 * 落点选择（PRINTF_SINK_RTT）：
 *   1 = RTT 通道 LOG_CH_MCU（默认）
 *       · 不占用任何 UART，与协议物理口彻底无关；
 *       · 非阻塞（只做内存拷贝），可在任意上下文调用；
 *       · 调试器未连接时写入被静默丢弃，不会拖慢 CPU
 *         （旧方案 HAL_UART_Transmit 是阻塞发送，DEBUG_REPORT 打开时占用明显）。
 *   2 = USART1 阻塞发送（历史方案）
 *       · USART1 同时是协议口，此时必须把 COMM_DEBUG_SILENT 置 1，
 *         否则文本与 1 kHz 二进制帧混流，且协议 DMA 处于 BUSY 时日志会被丢掉。
 *
 * 说明：本工程的正式日志由 App/Src/dbg_report.c 直接走 rtt_log 的分通道接口，
 *       这里的 _write 只是"兜底"——任何遗漏的 printf / puts 都会落到 MCU 通道，
 *       而不是打到协议口上破坏通信。
 */
#include "main.h"
#if defined(__has_include)
#if __has_include("usart.h")
#include "usart.h"
#endif
#endif
#include <sys/unistd.h>

#include "rtt_log.h"

/* 输出落点：1 = RTT，2 = USART */
#ifndef PRINTF_SINK_RTT
#define PRINTF_SINK_RTT 1
#endif

#if (PRINTF_SINK_RTT == 1)

int _write(int file, char* ptr, int len)
{
    (void)file;
    if ((ptr == 0) || (len <= 0)) {
        return 0;
    }
    /* 整段一次写入；缓冲区不足时整条丢弃，丢弃量见 rtt_get_dropped_bytes(LOG_CH_MCU) */
    (void)rtt_write_channel((unsigned)LOG_CH_MCU, ptr, (unsigned)len);
    return len;
}

#else /* ===== 历史方案：USART 阻塞发送 ===== */

/* 就绪开关：1 = 真正输出到串口；0 = 编译为空实现 */
#define PRINTF_UART_READY 1
/* 打印口 = USART1（现场只引出该口，USART2 未接线） */
#define PRINTF_UART       huart1

#if PRINTF_UART_READY
#ifndef HAL_UART_MODULE_ENABLED
#error "需要在 HAL 配置中启用 HAL_UART 模块，或把 PRINTF_UART_READY 改回 0"
#endif
/**
 * 注意：HAL_UART_Transmit 是阻塞调用，单帧耗时为 len x 10 / 波特率。
 * 与协议共用 USART1 时，协议 TX 走 DMA，printf 会因 gState=BUSY 直接丢日志。
 */
int _write(int file, char* ptr, int len)
{
    (void)file;
    if ((ptr == 0) || (len <= 0)) {
        return 0;
    }
    HAL_UART_Transmit(&PRINTF_UART, (uint8_t*)ptr, (uint16_t)len, HAL_MAX_DELAY);
    return len;
}
#else
/** 占位实现：保持 newlib 链接完整，输出被丢弃 */
int _write(int file, char* ptr, int len)
{
    (void)file;
    (void)ptr;
    return len;
}
#endif

#endif /* PRINTF_SINK_RTT */
