#ifndef _BSP_LED_H
#define _BSP_LED_H

#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_gpio.h"

/* Definition LED1-GREEN*/
#define LED_PIN LL_GPIO_PIN_0
#define LED_GPIO_PORT GPIOA
#define LED_GPIO_CLK_ENABLE() LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOA)
/* Turn On */
#define LedOn() LL_GPIO_SetOutputPin(LED_GPIO_PORT, LED_PIN);
/* Turn Off */
#define LedOff() LL_GPIO_ResetOutputPin(LED_GPIO_PORT, LED_PIN)

extern void LED_Init(void);

extern void LED_Toggle(void);

#endif
