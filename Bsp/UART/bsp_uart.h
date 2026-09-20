#ifndef __BSP_UART_H__
#define __BSP_UART_H__

#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_gpio.h"
#include "stm32h7xx_ll_usart.h"

#include <stdio.h>
#include <string.h>

// usart使能
#define USART1_ENABLE
#define USART2_ENABLE
#define USART3_ENABLE
#define UART4_ENABLE
#define UART5_ENABLE
#define USART6_ENABLE
#define UART7_ENABLE
#define UART8_ENABLE

#define BAUDRATE_115200          (115200U)
#define BAUDRATE_230400          (230400U)
#define BAUDRATE_460800          (460800U)
#define BAUDRATE_921600          (921600U)
#define BAUDRATE_1500000         (1500000U)

#define UART_RX_PACKET_MAX_LEN (255U)

#define UART1_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART2_DMA_RX_LEN (256U)
#define UART3_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART4_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART5_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART6_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART7_DMA_RX_LEN UART_RX_PACKET_MAX_LEN
#define UART8_DMA_RX_LEN UART_RX_PACKET_MAX_LEN

/******** RAM地址分配 ----------------------------------------*/
#define ADDR_UART1_TX         (0x3001F000U)    //发送buffer
#define ADDR_UART1_RX         (0x3001F100U)    //接收buffer
#define ADDR_UART2_TX         (0x3001F200U)    //发送buffer
#define ADDR_UART2_RX         (0x3001F300U)    //接收buffer
#define ADDR_UART3_TX         (0x3001F400U)    //发送buffer
#define ADDR_UART3_RX         (0x3001F500U)    //接收buffer
#define ADDR_UART4_TX         (0x3001F600U)    //发送buffer
#define ADDR_UART4_RX         (0x3001F700U)    //接收buffer
#define ADDR_UART5_TX         (0x3001F800U)    //发送buffer
#define ADDR_UART5_RX         (0x3001F900U)    //接收buffer
#define ADDR_UART6_TX         (0x3001FA00U)    //发送buffer
#define ADDR_UART6_RX         (0x3001FB00U)    //接收buffer
#define ADDR_UART7_TX         (0x3001FC00U)    //发送buffer
#define ADDR_UART7_RX         (0x3001FD00U)    //接收buffer
#define ADDR_UART8_TX         (0x3001FE00U)    //发送buffer
#define ADDR_UART8_RX         (0x3001FF00U)    //接收buffer

#pragma pack(1)
typedef struct
{
	__IO uint8_t buffer[UART_RX_PACKET_MAX_LEN];
	__IO uint8_t buffer_length;
	__IO uint8_t interrupt_flag;
	__IO uint32_t interrupt_count;
}uart_receive_packet_t;
#pragma pack()

#if defined(USART1_ENABLE)

extern volatile uart_receive_packet_t uart1_rx;
extern void uart1_init(uint32_t baudrate);
extern uint8_t uart1_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(USART2_ENABLE)

extern volatile uart_receive_packet_t uart2_rx;
extern volatile uint32_t uart2_rx_error_count;
extern void uart2_init(uint32_t baudrate);
extern uint8_t uart2_read_byte(uint8_t *data);
extern uint8_t uart2_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(USART3_ENABLE)

extern volatile uart_receive_packet_t uart3_rx;
extern void uart3_init(uint32_t baudrate);
extern uint8_t uart3_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(UART4_ENABLE)

extern volatile uart_receive_packet_t uart4_rx;
extern void uart4_init(uint32_t baudrate);
extern uint8_t uart4_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(UART5_ENABLE)

extern volatile uart_receive_packet_t uart5_rx;
extern void uart5_init(uint32_t baudrate);
extern uint8_t uart5_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(USART6_ENABLE)

extern volatile uart_receive_packet_t uart6_rx;
extern void uart6_init(uint32_t baudrate);
extern uint8_t uart6_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(UART7_ENABLE)

extern volatile uart_receive_packet_t uart7_rx;
extern void uart7_init(uint32_t baudrate);
extern uint8_t uart7_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#if defined(UART8_ENABLE)

extern volatile uart_receive_packet_t uart8_rx;
extern void uart8_init(uint32_t baudrate);
extern uint8_t uart8_transmit(const uint8_t u8_data[], uint8_t u8_length);
#endif

#pragma pack()
#endif /* __UART_H */
