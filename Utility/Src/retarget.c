/**
 *@file retarget.c
 *@author westin
 *@brief printf 重定向(UART:USART2)
 *@version 0.1
 *@date 2026-11-02
 */
#include "main.h"
#if defined(__has_include)
#if __has_include("usart.h")
#include "usart.h"
#endif
#endif
#include <sys/unistd.h>

/**
 * 就绪开关：
 * - 1 = 真正输出到串口
 * - 0 = 编译为空实现,printf 不输出但不会破坏构建
 *
 * 将所需重定向串口号改为对应串口即可
 */
#define PRINTF_UART_READY 1
/* 打印口 = USART1（现场只引出该口，USART2 未接线）。
 * 因为 USART1 同时是协议物理口，printf 与 1 kHz 上行帧/参数应答共用一条线，
 * 必须靠 comm_cfg.h 的 COMM_DEBUG_SILENT 让协议侧静默，否则：
 *   1) 文本与二进制帧混流，上位机与终端都无法判读；
 *   2) 协议 TX 走 DMA，printf 走阻塞发送，HAL 会因 gState=BUSY 直接丢掉日志。
 * 调试结束把 COMM_DEBUG_REPORT 置 0 → 打印全部编译掉，USART1 恢复纯协议。 */
#define PRINTF_UART       huart1
#if PRINTF_UART_READY
#ifndef HAL_UART_MODULE_ENABLED
#error "需要在 stm32h7xx_hal_conf.h 中启用 HAL_UART 模块，或把 PRINTF_UART_READY 改回 0"
#endif
/**
 * 注意:HAL_UART_Transmit 是阻塞调用，单帧耗时为 len x 10 / 波特率。
 * 在中断上下文或高频循环中调用会拖慢甚至卡死系统。
 * 需要中断安全时改用「SWO/ITM」模式,或自行改为环形缓冲 + DMA。
 */
int _write(int file, char* ptr, int len)
{
    (void)file;
    if (ptr == 0 || len <= 0) {
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
