#include "boot_jump.h"
#include "bsp_uart.h"
#include "stm32h7xx_ll_cortex.h"
#include "stm32h7xx_ll_rcc.h"

/* =====================================================================
 * 串口烧录引导模块
 *
 * UART1 以 115200/8N1 等待连续三个 0x7F。触发后跳到
 * STM32H743 芯片内置 ROM Bootloader，之后由上位机以 8E1 连接。
 * ===================================================================== */

#define BOOT_ROM_VECTOR_ADDRESS    (0x1FF09800UL)
#define BOOT_ROM_START_ADDRESS     (0x1FF00000UL)
#define BOOT_ROM_END_ADDRESS       (0x1FF1E800UL)
#define BOOT_TRIGGER_BYTE          (0x7FU)
#define BOOT_TRIGGER_COUNT         (3U)
#define BOOT_TX_WAIT_LIMIT         (1000000UL)
#define BOOT_ACK                    (0x79U)

extern void Boot_StartRom(uint32_t stack_pointer, uint32_t reset_handler);


/* ---------------------------------------------------------------------
 * 自检变量 —— 验证 Flash 版的 .data 初始化
 *
 * 非 const 且带初值  =>  落在 .data 段。
 * Flash 配置正确时：初值存在 Flash 里，启动代码把它拷进 RAM。
 *
 * 怎么验：检查 IAR map 文件，确认 .data 运行地址在 RAM，
 * 而装载地址在 0x08xxxxxx。调试器可能直接初始化 RAM，
 * 因此不能只靠 Live Watch 的数值判断。
 *
 * 为什么必须专门验这一条：
 *     LED 会闪只能证明「代码在跑」，证明不了 .data 初始化对不对 ——
 *     因为闪灯那段代码不依赖任何带初值的全局变量。
 *     这是 RAM 版从来不会暴露、换成 Flash 版才会暴露的问题。
 *
 * 用 __IO(volatile) 防止被编译器优化掉。
 * ------------------------------------------------------------------ */
__IO uint32_t boot_selftest = 0x5A5AA5A5;
__IO uint8_t boot_stage = BOOT_STAGE_IDLE;
__IO uint8_t boot_debug_jump = 0U;
__IO uint8_t boot_7f_count = 0U;


/* ---------------------------------------------------------------------
 * 初始化
 *
 * 【调用位置有要求】
 *   必须和其他 uartN_init 放在一起，且在 main.c 的
 *   NVIC_SetPriorityGrouping(4) 之前调用。
 *   这样 UART1 的中断优先级编码方式才和 UART2/3/4/7 完全一致，
 *   既不改动别人，也不让自己特殊化。
 *
 * 【为什么用 115200 而不是业务口的 921600】
 *   USART1 挂在 PCLK2 = 15MHz 上（main.c 没设过 USART 时钟源，走复位默认值）：
 *       115200  ->  实际 115385，误差 0.16%   ✓
 *       921600  ->  实际 937500，误差 1.72%   ✗ 握手临界
 *
 * 【资源占用】
 *   DMA1 Stream0(TX) / Stream1(RX) 和 USART1_IRQn 都是 BSP 里专为
 *   USART1 留好的，与其他 UART 不重叠，所以启用它不会影响任何现有模块。
 * ------------------------------------------------------------------ */
void boot_init(void)
{
    uart1_init(115200);
}


static uint8_t boot_stack_pointer_is_valid(uint32_t stack_pointer)
{
    const uint8_t in_dtcm = ((stack_pointer >= 0x20000000UL) &&
                             (stack_pointer <= 0x20020000UL));
    const uint8_t in_axi_sram = ((stack_pointer >= 0x24000000UL) &&
                                 (stack_pointer <= 0x24080000UL));

    return (uint8_t)(((stack_pointer & 0x3UL) == 0UL) &&
                     (in_dtcm || in_axi_sram));
}


static uint8_t boot_reset_handler_is_valid(uint32_t reset_handler)
{
    const uint32_t address = reset_handler & ~1UL;

    return (uint8_t)(((reset_handler & 1UL) != 0UL) &&
                     (address >= BOOT_ROM_START_ADDRESS) &&
                     (address < BOOT_ROM_END_ADDRESS));
}


static uint8_t boot_send_ack(void)
{
    uint32_t timeout = BOOT_TX_WAIT_LIMIT;
    const uint8_t ack = BOOT_ACK;

    uart1_transmit(&ack, 1U);
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_0))
    {
        if (timeout-- == 0U)
        {
            return 0U;
        }
    }

    timeout = BOOT_TX_WAIT_LIMIT;
    while (LL_USART_IsActiveFlag_TC(USART1) == 0U)
    {
        if (timeout-- == 0U)
        {
            return 0U;
        }
    }

    return 1U;
}


static void boot_reset_active_peripherals(void)
{
    const uint32_t apb1_uarts = LL_APB1_GRP1_PERIPH_USART2 |
                                LL_APB1_GRP1_PERIPH_USART3 |
                                LL_APB1_GRP1_PERIPH_UART4 |
                                LL_APB1_GRP1_PERIPH_UART7;
    const uint32_t dma_controllers = LL_AHB1_GRP1_PERIPH_DMA1 |
                                     LL_AHB1_GRP1_PERIPH_DMA2;

    LL_AHB1_GRP1_ForceReset(dma_controllers);
    LL_APB1_GRP1_ForceReset(apb1_uarts);
    LL_APB2_GRP1_ForceReset(LL_APB2_GRP1_PERIPH_USART1);
    __DSB();
    LL_AHB1_GRP1_ReleaseReset(dma_controllers);
    LL_APB1_GRP1_ReleaseReset(apb1_uarts);
    LL_APB2_GRP1_ReleaseReset(LL_APB2_GRP1_PERIPH_USART1);
}


static void boot_jump_to_rom(uint8_t send_ack)
{
    const uint32_t stack_pointer = *(const uint32_t *)BOOT_ROM_VECTOR_ADDRESS;
    const uint32_t reset_handler = *(const uint32_t *)(BOOT_ROM_VECTOR_ADDRESS + 4UL);
    uint32_t index;

    if ((!boot_stack_pointer_is_valid(stack_pointer)) ||
        (!boot_reset_handler_is_valid(reset_handler)))
    {
        boot_stage = BOOT_STAGE_VALIDATION_ERROR;
        return;
    }

    if ((send_ack != 0U) && (boot_send_ack() == 0U))
    {
        boot_stage = BOOT_STAGE_TX_TIMEOUT;
        return;
    }

    boot_stage = BOOT_STAGE_JUMPING;
    __disable_irq();

    SysTick->CTRL = 0UL;
    SysTick->LOAD = 0UL;
    SysTick->VAL = 0UL;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    for (index = 0U; index < 8U; ++index)
    {
        NVIC->ICER[index] = 0xFFFFFFFFUL;
        NVIC->ICPR[index] = 0xFFFFFFFFUL;
    }

    boot_reset_active_peripherals();

    /* LL_RCC_DeInit first switches SYSCLK to HSI, then turns the PLLs off. */
    LL_RCC_DeInit();

    SCB_DisableDCache();
    SCB_DisableICache();
    LL_MPU_Disable();

    SCB->VTOR = BOOT_ROM_VECTOR_ADDRESS;
    __set_CONTROL(0UL);
    __set_BASEPRI(0UL);
    __set_FAULTMASK(0UL);
    __DSB();
    __ISB();

    /* No interrupt source is enabled now; restore the reset-state PRIMASK. */
    __enable_irq();
    Boot_StartRom(stack_pointer, reset_handler);

    for (;;)
    {
    }
}


/* ---------------------------------------------------------------------
 * 轮询
 *
 * 跨多次 DMA 空闲中断累计连续的 0x7F，其他字节会重置计数。
 * ------------------------------------------------------------------ */
void boot_poll(void)
{
    uint8_t index;
    uint8_t length;

    if (boot_debug_jump != 0U)
    {
        boot_debug_jump = 0U;
        boot_stage = BOOT_STAGE_TRIGGERED;
        boot_jump_to_rom(0U);
        return;
    }

    if (uart1_rx.interrupt_flag == 0U)
    {
        return;
    }

    NVIC_DisableIRQ(USART1_IRQn);
    length = uart1_rx.buffer_length;
    uart1_rx.interrupt_flag = 0U;

    for (index = 0U; index < length; ++index)
    {
        if (uart1_rx.buffer[index] == BOOT_TRIGGER_BYTE)
        {
            ++boot_7f_count;
            if (boot_7f_count >= BOOT_TRIGGER_COUNT)
            {
                NVIC_EnableIRQ(USART1_IRQn);
                boot_stage = BOOT_STAGE_TRIGGERED;
                boot_jump_to_rom(1U);
                boot_7f_count = 0U;
                return;
            }
        }
        else
        {
            boot_7f_count = 0U;
        }
    }

    NVIC_EnableIRQ(USART1_IRQn);
}
