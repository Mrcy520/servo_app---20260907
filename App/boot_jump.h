#ifndef __BOOT_JUMP_H__
#define __BOOT_JUMP_H__

/* =====================================================================
 * 串口烧录引导模块
 *
 * 目的：让固件收到串口暗号后，跳进 STM32 芯片自带的 ROM Bootloader，
 *       这样就可以用 USB 转串口直接烧固件，不再需要烧录器。
 *
 * 用哪个串口：USART1 -> PA9/PA10 -> J9（通道5）
 *   选它的三个理由：
 *     1) 代码里完全没被占用 —— uart1_init 全工程零调用
 *     2) H743 的 ROM Bootloader 只认 USART1/2/3，
 *        而 USART2(J5) 被业务占用、USART3(J7) 被 printf 占用
 *     3) J9 是空通道，烧录时不用拔任何外设
 *
 * 硬件前提：
 *   J9 出来的是 RS-422 差分信号（经 U9 隔离器 + U15 MAX3490），不是 TTL，
 *   所以要配 USB 转 RS422「四线全双工」模块。
 *   J9 没有 GND 针脚，且隔离侧靠 J11 的 12V 供电 —— 板子不上电 J9 就是死的。
 * ===================================================================== */

#include <stdint.h>

#define BOOT_STAGE_IDLE             (0U)
#define BOOT_STAGE_TRIGGERED        (1U)
#define BOOT_STAGE_JUMPING          (2U)
#define BOOT_STAGE_VALIDATION_ERROR (3U)
#define BOOT_STAGE_TX_TIMEOUT       (4U)

extern volatile uint32_t boot_selftest;
extern volatile uint8_t boot_stage;
extern volatile uint8_t boot_debug_jump;
extern volatile uint8_t boot_7f_count;

/* 初始化：打开 UART1。调用位置见 boot_jump.c 的说明 */
extern void boot_init(void);

/* 轮询：主循环调用。未触发时几乎零开销 */
extern void boot_poll(void);

#endif /* __BOOT_JUMP_H__ */
