
/* Includes ------------------------------------------------------------------*/
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_system.h"
#include "stm32h7xx_ll_cortex.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_gpio.h"
#include "stm32h7xx_ll_pwr.h"
#include "stm32h7xx_ll_rcc.h"
#include "stm32h7xx_ll_spi.h"
#include "stm32h7xx_ll_utils.h"

#include "bsp_led.h"
#include "bsp_uart.h"
#include "boot_jump.h"
#include "gsa200.h"
#include "j8_uart_test.h"
#include "main.h"

__IO uint32_t u32_timer_1ms;
__IO uint16_t u16_led_count;
__IO uint8_t flag_timer_1ms;

__IO uint32_t u32_elapsed_time;

__IO uint8_t send_buffer[32];
__IO uint8_t rx_buffer[32];

__IO uint16_t send_data[15];

__IO uint32_t HCLK_Freq = 120000000;           // HCLK
__IO uint32_t PCLK1_Freq = 120000000;          // PCLK1
__IO uint32_t PCLK2_Freq = 120000000;          // PCLK2
__IO uint32_t PCLK3_Freq = 120000000;          // PCLK3
__IO uint32_t PCLK4_Freq = 120000000;          // PCLK4

__IO uint32_t USART16_Freq, USART234578_Freq;
__IO uint32_t SPI123_Freq, SPI45_Freq;

/* Private function prototypes -----------------------------------------------*/
static void VectorBase_Config(void);
static void SystemClock_Config(void);
static void MPU_Config(void);
static void CPU_CACHE_Enable(void);

// redefine fputc for printf
int fputc(int ch, FILE *f)
{
	// wait transmit ok
	while(LL_USART_IsActiveFlag_TC(USART3) == 0);
	// transmit current 
	LL_USART_TransmitData8(USART3, (uint8_t)ch);
	return ch;
}

int main(void)
{
  VectorBase_Config();  // Configure the vector table base address.
  MPU_Config();         // 配合 D-Cache 使用
	CPU_CACHE_Enable();		// Enable the i-Cache, d-Cache

	SystemClock_Config(); // Configure the system clock to 480 MHz

	LED_Init();

  uart2_init(BAUDRATE_460800);
  gsa200_init();
  uart3_init(BAUDRATE_921600);
  uart4_init(BAUDRATE_921600);
  uart7_init(BAUDRATE_921600);
  j8_uart_test_init();

  boot_init();               // 串口烧录：打开 UART1（必须放在 NVIC_SetPriorityGrouping 之前）

	NVIC_SetPriorityGrouping(4);
	/* Set systick to 1ms */
	SysTick_Config(SystemCoreClock / 1000); //1000-1ms,500-2ms
  NVIC_SetPriority(SysTick_IRQn, 0); // SysTick_IRQn中断优先级

	/* Infinite loop */
	while (1)
	{
		gsa200_poll(u32_timer_1ms);
		boot_poll();
		j8_uart_test_poll(u32_timer_1ms);

		// led 显示
		if (u16_led_count < 1000)
		{
			LedOn();
		}
		else if (u16_led_count < 2000)
		{
			LedOff();
		}
		else if (u16_led_count == 2000)
		{
		}
		else
			u16_led_count = 0;

		/* 定时1ms标志 */
		if (flag_timer_1ms)
		{
			flag_timer_1ms = 0;
			u32_elapsed_time = (SysTick->LOAD - SysTick->VAL) / 480;
		}
	}
}

static void VectorBase_Config(void)
{
  /* The constant array with vectors of the vector table is declared externally in the
   * c-startup code.
   */
  extern const unsigned long __vector_table[];

  /* Remap the vector table to where the vector table is located for this program. */
  SCB->VTOR = (unsigned long)&__vector_table[0];
} /*** end of VectorBase_Config ***/

/**
* @brief  系统时钟配置函数
*         系统时钟配置参数如下 :
*            系统时钟源                = PLL1 (外部高速晶振HSE 非旁路模式)
*            系统时钟SYSCLK(Hz)        = 480000000 (CPU核心时钟)
*            AHB预分频器               = 2
*            HCLK(Hz)                  = 240000000 (AXI/AHB总线时钟，最大240MHz)
*                                       计算公式：HCLK = SYSCLK / AHB预分频器
*            D2域 APB1预分频器         = 16 (APB1时钟 15MHz，最大120MHz)
*                                       计算公式：APB1 = HCLK / APB1预分频器
*            D2域 APB2预分频器         = 16 (APB2时钟 15MHz，最大240MHz)
*                                       计算公式：APB2 = HCLK / APB2预分频器
*            D1域 APB3预分频器         = 16 (APB3时钟 15MHz，最大120MHz)
*                                       计算公式：APB3 = HCLK / APB3预分频器
*            D3域 APB4预分频器         = 16 (APB4时钟 15MHz，最大120MHz)
*                                       计算公式：APB4 = HCLK / APB4预分频器
*            HSE晶振频率(Hz)           = 25000000
*            PLL分频系数M              = 5    计算：25MHz / 5 = 5MHz (PLL输入时钟)
*            PLL倍频系数N              = 192  计算：5MHz * 192 = 960MHz (PLL VCO输出时钟)
*            PLL分频系数P              = 2    计算：960MHz / 2 = 480MHz (PLL1P输出，用作系统时钟)
*            PLL分频系数Q              = 4    计算：960MHz / 4 = 240MHz (PLL1Q输出)
*            PLL分频系数R              = 2    计算：960MHz / 2 = 480MHz (PLL1R输出)
*            内核电压VDD(V)            = 3.3
*            Flash等待周期(WS)         = 4
* @param  无
* @retval 无
*/
static void SystemClock_Config(void)
{
	// 配置电源供电模式：使用LDO线性稳压器供电
	LL_PWR_ConfigSupply(LL_PWR_LDO_SUPPLY);
	// 设置电压调节等级：等级1（最高性能模式，支持最高主频）
	LL_PWR_SetRegulVoltageScaling(LL_PWR_REGU_VOLTAGE_SCALE1);
	// 等待电压调节稳定，VOS标志置1表示电压已达到设定值
	while (LL_PWR_IsActiveFlag_VOS() == 0)
	{
	}

	// 先关闭HSE外部晶振，确保重新配置时状态正确
	LL_RCC_HSE_Disable();
	// 配置HSE为非旁路模式（使用外部无源晶振，而非外部时钟信号）
	LL_RCC_HSE_DisableBypass();
	// 使能HSE外部高速晶振
	LL_RCC_HSE_Enable();
	// 等待HSE晶振稳定就绪，硬件置1表示稳定
	while(LL_RCC_HSE_IsReady() != 1)
	{
	}

	// 设置Flash存储器等待周期为4个周期，匹配480MHz主频的访问速度
	LL_FLASH_SetLatency(LL_FLASH_LATENCY_4);

	// ==================== 主PLL1配置与使能 ====================
	// 设置PLL时钟源为HSE外部晶振
	LL_RCC_PLL_SetSource(LL_RCC_PLLSOURCE_HSE);
	// 使能PLL1的P通道输出（用于系统时钟）
	LL_RCC_PLL1P_Enable();
	// 使能PLL1的Q通道输出
	LL_RCC_PLL1Q_Enable();
	// 使能PLL1的R通道输出
	LL_RCC_PLL1R_Enable();
	// 关闭PLL1的小数分频功能，使用整数分频
	LL_RCC_PLL1FRACN_Disable();
	// 设置PLL1输入时钟范围：2~4MHz（匹配M分频后的5MHz）
	LL_RCC_PLL1_SetVCOInputRange(LL_RCC_PLLINPUTRANGE_2_4);
	// 设置PLL1 VCO输出范围：宽范围（支持高频输出）
	LL_RCC_PLL1_SetVCOOutputRange(LL_RCC_PLLVCORANGE_WIDE);
	// 配置PLL1预分频系数M=5
	LL_RCC_PLL1_SetM(5);
	// 配置PLL1倍频系数N=192
	LL_RCC_PLL1_SetN(192);
	// 配置PLL1 P通道分频系数P=2
	LL_RCC_PLL1_SetP(2);
	// 配置PLL1 Q通道分频系数Q=4
	LL_RCC_PLL1_SetQ(4);
	// 配置PLL1 R通道分频系数R=2
	LL_RCC_PLL1_SetR(2);
	// 使能PLL1
	LL_RCC_PLL1_Enable();
	// 等待PLL1锁定稳定，硬件置1表示就绪
	while(LL_RCC_PLL1_IsReady() != 1)
	{
	}

	// ==================== 总线时钟分频配置 ====================
	// 系统时钟SYSCLK分频：不分频（直接使用PLL1P输出）
	LL_RCC_SetSysPrescaler(LL_RCC_SYSCLK_DIV_1);
	// AHB总线预分频：2分频（480MHz/2=240MHz）
	LL_RCC_SetAHBPrescaler(LL_RCC_AHB_DIV_2);
	// APB1总线预分频：16分频（240MHz/16=15MHz）
	LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_16);
	// APB2总线预分频：16分频（240MHz/16=15MHz）
	LL_RCC_SetAPB2Prescaler(LL_RCC_APB2_DIV_16);
	// APB3总线预分频：16分频（240MHz/16=15MHz）
	LL_RCC_SetAPB3Prescaler(LL_RCC_APB3_DIV_16);
	// APB4总线预分频：16分频（240MHz/16=15MHz）
	LL_RCC_SetAPB4Prescaler(LL_RCC_APB4_DIV_16);

	// ==================== 选择系统时钟源 ====================
	// 设置系统时钟源为PLL1
	LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL1);
	// 等待时钟源切换完成，确认PLL1成为系统时钟源
	while(LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL1)
	{
	}

	// ==================== 更新全局时钟变量 ====================
	// 更新系统核心时钟全局变量SystemCoreClock，自动计算当前主频
	SystemCoreClockUpdate();

	// 计算并存储各总线时钟频率
	HCLK_Freq = LL_RCC_CALC_HCLK_FREQ(SystemCoreClock, LL_RCC_GetAHBPrescaler());       // 计算AHB总线时钟HCLK
	PCLK1_Freq = LL_RCC_CALC_PCLK1_FREQ(HCLK_Freq, LL_RCC_GetAPB1Prescaler());          // 计算APB1总线时钟PCLK1
	PCLK2_Freq = LL_RCC_CALC_PCLK2_FREQ(HCLK_Freq, LL_RCC_GetAPB2Prescaler());          // 计算APB2总线时钟PCLK2
	PCLK3_Freq = LL_RCC_CALC_PCLK3_FREQ(HCLK_Freq, LL_RCC_GetAPB3Prescaler());          // 计算APB3总线时钟PCLK3
	PCLK4_Freq = LL_RCC_CALC_PCLK4_FREQ(HCLK_Freq, LL_RCC_GetAPB4Prescaler());          // 计算APB4总线时钟PCLK4

	// 获取SPI外设时钟频率
	SPI123_Freq = LL_RCC_GetSPIClockFreq(LL_RCC_SPI123_CLKSOURCE);    // 获取SPI1/2/3时钟频率
	SPI45_Freq = LL_RCC_GetSPIClockFreq(LL_RCC_SPI45_CLKSOURCE);      // 获取SPI4/5时钟频率

	// 获取USART外设时钟频率
	USART234578_Freq = LL_RCC_GetUSARTClockFreq(LL_RCC_USART234578_CLKSOURCE);  // 获取USART2/3/4/5/7/8时钟频率
	USART16_Freq = LL_RCC_GetUSARTClockFreq(LL_RCC_USART16_CLKSOURCE);  	// 获取USART1/6时钟频率
}

static void MPU_Config(void)
{
  // Disables the MPU. 
  LL_MPU_Disable();

  // Region 0: Flash 2MB, Write Back (基地址 0x08000000 )
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER0, 0x00, 0x08000000U,
      LL_MPU_REGION_SIZE_2MB |
      LL_MPU_REGION_PRIV_RO_URO |
      LL_MPU_TEX_LEVEL1 | LL_MPU_ACCESS_CACHEABLE | LL_MPU_ACCESS_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 1: AXI SRAM 512KB, Non-cacheable
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER1, 0x00, 0x24000000U,
      LL_MPU_REGION_SIZE_512KB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 2: SRAM1 128KB, Non-cacheable (for DMA)
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER2, 0x00, 0x30000000U,
      LL_MPU_REGION_SIZE_128KB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 3: SRAM2 128KB, Non-cacheable (for DMA)
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER3, 0x00, 0x30020000U,
      LL_MPU_REGION_SIZE_128KB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 4: SRAM3 32KB, Non-cacheable (for DMA)
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER4, 0x00, 0x30040000U,
      LL_MPU_REGION_SIZE_32KB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 5: SRAM4 64KB, Write Through
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER5, 0x00, 0x38000000U,
      LL_MPU_REGION_SIZE_64KB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 6: SDRAM 64MB, Write Through
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER6, 0x00, 0xD0000000U,
      LL_MPU_REGION_SIZE_64MB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_CACHEABLE | LL_MPU_ACCESS_NOT_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_ENABLE);

  // Region 7: AHB Peripherals (Device)
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER7, 0x00, 0x40000000U,
      LL_MPU_REGION_SIZE_64MB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_DISABLE);

  // Region 8: APB Peripherals (Device)
  LL_MPU_ConfigRegion(LL_MPU_REGION_NUMBER8, 0x00, 0x48000000U,
      LL_MPU_REGION_SIZE_64MB |
      LL_MPU_REGION_FULL_ACCESS |
      LL_MPU_TEX_LEVEL0 | LL_MPU_ACCESS_NOT_CACHEABLE | LL_MPU_ACCESS_BUFFERABLE |
      LL_MPU_INSTRUCTION_ACCESS_DISABLE);

  // 不需要 Region 9，已被 Region 4 覆盖
  LL_MPU_Enable(LL_MPU_CTRL_PRIVILEGED_DEFAULT);
}

/**
* @brief  使能CPU指令缓存和数据缓存，提升程序运行效率
* @note   缓存开启后，CPU读取指令/数据时优先从高速缓存访问，大幅降低访问内存延迟
* @param  无
* @retval 无
*/
static void CPU_CACHE_Enable(void)
{
	/* 使能CPU指令缓存 (I-Cache)
	 * 作用：专门缓存CPU要执行的程序指令，减少从Flash/SRAM取指令的耗时，提升代码执行速度
	 */
	SCB_EnableICache();

	/* 使能CPU数据缓存 (D-Cache)
	 * 作用：专门缓存CPU运算用到的变量、数据，加速数据读写操作
	 */
	SCB_EnableDCache();

	/* 配置SCB_CACR寄存器，设置第2位为1
	 * CACR寄存器：CPU缓存控制寄存器
	 * BIT2 = 1：使能**写透模式(Write Through)**，缓存数据修改时同步写入内存，保证数据一致性
	 * 适用于外设DMA、硬件加速器等需要实时访问内存数据的场景
	 */
	SCB->CACR |= 1 << 2;
}

void SysTick_Handler(void)
{
	u32_timer_1ms++;        // 系统计时器
	u16_led_count++;        // led计数器
	flag_timer_1ms = 1;     // 1ms定时标志，在程序需要定时的地方使用
}
