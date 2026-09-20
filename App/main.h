/**
  ******************************************************************************
  * @file    main.h
  * @brief   Header for main.c module
  ******************************************************************************
  * @note    本文件声明 main.c 中定义的全局变量，
  *          供中断处理(Bsp/It_Handler)等其他模块访问。
  *          变量名必须与 main.c 中的定义严格一致。
  ******************************************************************************
  */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

/* Includes ------------------------------------------------------------------*/
/* LL drivers common to all LL examples */
#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_gpio.h"
#include "stm32h7xx_ll_pwr.h"
#include "stm32h7xx_ll_rcc.h"
#include "stm32h7xx_ll_spi.h"
#include "stm32h7xx_ll_utils.h"

#include <stdint.h>

/* Exported variables --------------------------------------------------------*/
/* 系统时基与标志 */
extern __IO uint32_t u32_timer_1ms;
extern __IO uint16_t u16_led_count;
extern __IO uint8_t  flag_timer_1ms;

extern __IO uint32_t u32_elapsed_time;

/* 收发缓冲区 */
extern __IO uint8_t  send_buffer[32];
extern __IO uint8_t  rx_buffer[32];

extern __IO uint16_t send_data[15];

/* 总线时钟频率 */
extern __IO uint32_t HCLK_Freq;
extern __IO uint32_t PCLK1_Freq;
extern __IO uint32_t PCLK2_Freq;
extern __IO uint32_t PCLK3_Freq;
extern __IO uint32_t PCLK4_Freq;

extern __IO uint32_t USART16_Freq, USART234578_Freq;
extern __IO uint32_t SPI123_Freq, SPI45_Freq;

#endif /* __MAIN_H */
