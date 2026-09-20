#include <stdint.h>
#include "stm32h743xx.h"

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

extern void ExitRun0Mode(void);
extern void SystemInit(void);
extern void __libc_init_array(void);
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void);

void NMI_Handler(void) __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void) __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void) __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void) __attribute__((weak, alias("Default_Handler")));

extern void USART1_IRQHandler(void);
extern void USART2_IRQHandler(void);
extern void USART3_IRQHandler(void);
extern void UART4_IRQHandler(void);
extern void UART5_IRQHandler(void);
extern void USART6_IRQHandler(void);
extern void UART7_IRQHandler(void);
extern void UART8_IRQHandler(void);

#define VECTOR_COUNT (16U + 150U)

__attribute__((section(".isr_vector"), used, aligned(1024)))
const uintptr_t __vector_table[VECTOR_COUNT] = {
    [0] = (uintptr_t)&_estack,
    [1] = (uintptr_t)Reset_Handler,
    [2] = (uintptr_t)NMI_Handler,
    [3] = (uintptr_t)HardFault_Handler,
    [4] = (uintptr_t)MemManage_Handler,
    [5] = (uintptr_t)BusFault_Handler,
    [6] = (uintptr_t)UsageFault_Handler,
    [11] = (uintptr_t)SVC_Handler,
    [12] = (uintptr_t)DebugMon_Handler,
    [14] = (uintptr_t)PendSV_Handler,
    [15] = (uintptr_t)SysTick_Handler,
    [16 ... VECTOR_COUNT - 1U] = (uintptr_t)Default_Handler,
    [16U + USART1_IRQn] = (uintptr_t)USART1_IRQHandler,
    [16U + USART2_IRQn] = (uintptr_t)USART2_IRQHandler,
    [16U + USART3_IRQn] = (uintptr_t)USART3_IRQHandler,
    [16U + UART4_IRQn] = (uintptr_t)UART4_IRQHandler,
    [16U + UART5_IRQn] = (uintptr_t)UART5_IRQHandler,
    [16U + USART6_IRQn] = (uintptr_t)USART6_IRQHandler,
    [16U + UART7_IRQn] = (uintptr_t)UART7_IRQHandler,
    [16U + UART8_IRQn] = (uintptr_t)UART8_IRQHandler,
};

void Reset_Handler(void)
{
    uint32_t *source = &_sidata;
    uint32_t *destination = &_sdata;

    while (destination < &_edata)
    {
        *destination++ = *source++;
    }

    destination = &_sbss;
    while (destination < &_ebss)
    {
        *destination++ = 0U;
    }

    ExitRun0Mode();
    SystemInit();
    __libc_init_array();
    (void)main();

    for (;;)
    {
    }
}

void Default_Handler(void)
{
    for (;;)
    {
    }
}

__attribute__((naked, noreturn))
void Boot_StartRom(uint32_t stack_pointer, uint32_t reset_handler)
{
    (void)stack_pointer;
    (void)reset_handler;
    __asm volatile (
        "msr msp, r0\n"
        "bx r1\n"
    );
}
