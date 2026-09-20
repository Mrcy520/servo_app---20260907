
#include "bsp_uart.h"

#if defined(USART1_ENABLE)

volatile uart_receive_packet_t uart1_rx;

/**
  * @brief  USART1 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PA9/PA10为USART1复用功能(TX/RX)
  */
static void uart1_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置USART1引脚 */
	/* 使能GPIOA外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOA);

	/* USART1 TX/RX 引脚配置
	   PA9 -> USART1_TX
	   PA10 -> USART1_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_9 | LL_GPIO_PIN_10;  // 配置PA9/PA10
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_7;                   // 复用功能为AF7(USART1) 
	LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/**
  * @brief  USART1串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart1_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能USART1外设时钟 */
	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);

	/* USART1基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_921600;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(USART1, &USART_InitStruct);

	LL_USART_EnableDirectionTx(USART1);				// 使能USART1发送功能
	LL_USART_EnableDirectionRx(USART1);				// 使能USART1接收功能
	LL_USART_DisableOverrunDetect(USART1);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_DisableDMADeactOnRxErr(USART1);		// 接收错误时保持DMA运行
	LL_USART_EnableDMAReq_TX(USART1);				// 使能USART1 TX DMA请求
	LL_USART_EnableDMAReq_RX(USART1);				// 使能USART1 RX DMA请求

	NVIC_SetPriority(USART1_IRQn, (6UL << 1) + 1UL);				// USART1接收中断优先级
	NVIC_EnableIRQ(USART1_IRQn);					// 使能USART1接收中断	
	LL_USART_EnableIT_IDLE(USART1);					// 使能串口空闲中断（用于不定长数据接收）	
  LL_USART_EnableIT_ERROR(USART1);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(USART1);						// 使能USART1外设
  USART1->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  USART1 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA1 Stream0 -> USART1_TX
  *         DMA1 Stream1 -> USART1_RX
  */
static void uart1_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA1时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* 配置DMA发送通道（USART1_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART1->TDR;  // 外设地址：USART1发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART1_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART1_TX;  // DMA请求映射：USART1_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream0 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_0);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_0, &DMA_InitStruct);

	/* 配置DMA接收通道（USART1_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART1->RDR;  // 外设地址：USART1接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART1_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = UART1_DMA_RX_LEN;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART1_RX; // DMA请求映射：USART1_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream1 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_1);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_1, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_0);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_1);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA1, LL_DMA_STREAM_0);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_1);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA1_Stream1_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA1_Stream1_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  USART1 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart1_init(uint32_t baudrate)
{
	uart1_gpio_config();   // 初始化串口GPIO
	uart1_dma_config();    // 初始化串口DMA
	uart1_config(baudrate);        // 初始化串口参数
	uart1_rx.interrupt_count = 0;	//初始化中断计数器
	uart1_rx.buffer_length = 0;		//初始化接收长度
	uart1_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  UART1 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart1_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream0），确保DMA停止工作
    do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_0);
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_0));

    memcpy((uint8_t *)ADDR_UART1_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到UART1 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_0, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA1 Stream0所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA1->LIFCR, DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0 |
                       DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 |
                       DMA_LIFCR_CFEIF0);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_0);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  USART1 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void USART1_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(USART1) || LL_USART_IsActiveFlag_FE(USART1)
      || LL_USART_IsActiveFlag_NE(USART1))
  {
    LL_USART_ClearFlag_ORE(USART1);
    LL_USART_ClearFlag_FE(USART1);
    LL_USART_ClearFlag_NE(USART1);
    return;
  }


  if (LL_USART_IsActiveFlag_IDLE(USART1))
  {
    //LL_USART_ClearFlag_IDLE(USART1);            // 清除空闲中断
    USART1->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    USART1->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	// 循环等待：先关闭DMA发送流（Stream1），确保DMA停止工作
	do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_1);
	while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_1));

	u8_len = UART1_DMA_RX_LEN - LL_DMA_GetDataLength(DMA1, LL_DMA_STREAM_1);	// 判断DMA中数据数量
	memcpy((void *)uart1_rx.buffer, (uint8_t *)ADDR_UART1_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart1_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用
	uart1_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart1_rx.interrupt_flag = 1;	// 数据拷贝完成后再通知主循环

	LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_1, UART1_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_1, ADDR_UART1_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA1 Stream1所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA1->LIFCR, DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1 |
                       DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 |
                       DMA_LIFCR_CFEIF1);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_1);			// 重新使能DMA接收流，启动DMA接收
}
#endif

#if defined(USART2_ENABLE)

volatile uart_receive_packet_t uart2_rx;
volatile uint32_t uart2_rx_error_count;
static uint16_t uart2_rx_read_index;

/**
  * @brief  USART2 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PA2/PA3为USART2复用功能(TX/RX)
  */
static void uart2_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置USART2引脚 */
	/* 使能GPIOA外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOA);

	/* USART2 TX/RX 引脚配置
	   PA2 -> USART2_TX
	   PA3 -> USART2_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_2 | LL_GPIO_PIN_3;  // 配置PA2/PA3
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_7;                   // 复用功能为AF7(USART2) 
	LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/**
  * @brief  USART2串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart2_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能USART1外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);

	/* USART2基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_460800;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(USART2, &USART_InitStruct);

	LL_USART_EnableDirectionTx(USART2);				// 使能USART2发送功能
	LL_USART_EnableDirectionRx(USART2);				// 使能USART2接收功能
	LL_USART_DisableOverrunDetect(USART2);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(USART2);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(USART2);				// 使能USART2 TX DMA请求
	LL_USART_EnableDMAReq_RX(USART2);				// 使能USART2 RX DMA请求

	NVIC_SetPriority(USART2_IRQn, (6UL << 1) + 1UL);				// USART2接收中断优先级
	NVIC_EnableIRQ(USART2_IRQn);					// 使能USART2接收中断	
	LL_USART_DisableIT_IDLE(USART2);				// 连续高速数据由循环 DMA 轮询读取
  LL_USART_EnableIT_ERROR(USART2);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(USART2);						// 使能USART2外设
  USART2->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  USART2 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA1 Stream2 -> USART2_TX
  *         DMA1 Stream3 -> USART2_RX
  */
static void uart2_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA1时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* 配置DMA发送通道（USART2_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART2->TDR;  // 外设地址：USART2发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART2_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART2_TX;  // DMA请求映射：USART2_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream2 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_2);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_2, &DMA_InitStruct);

	/* 配置DMA接收通道（USART2_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART2->RDR;  // 外设地址：USART2接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART2_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_CIRCULAR;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = UART2_DMA_RX_LEN;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART2_RX; // DMA请求映射：USART2_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream3 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_3);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_3, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_2);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_3);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA1, LL_DMA_STREAM_2);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_3);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA1_Stream3_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA1_Stream3_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  USART2 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart2_init(uint32_t baudrate)
{
	if (LL_USART_IsEnabled(USART2) != 0U)
	{
		LL_USART_Disable(USART2);
	}
	uart2_rx_read_index = 0U;
	uart2_rx_error_count = 0U;
	memset((void *)ADDR_UART2_RX, 0, UART2_DMA_RX_LEN);
	uart2_gpio_config();   // 初始化串口GPIO
	uart2_dma_config();    // 初始化串口DMA
	uart2_config(baudrate);        // 初始化串口参数
	uart2_rx.interrupt_count = 0;	//初始化中断计数器
	uart2_rx.buffer_length = 0;		//初始化接收长度
	uart2_rx.interrupt_flag = 0;	//初始化中断标志
}

uint8_t uart2_read_byte(uint8_t *data)
{
	uint16_t write_index;

	if (data == NULL)
	{
		return 0U;
	}

	write_index = (uint16_t)(UART2_DMA_RX_LEN -
	                         LL_DMA_GetDataLength(DMA1, LL_DMA_STREAM_3));
	if (uart2_rx_read_index == write_index)
	{
		return 0U;
	}

	*data = *(volatile uint8_t *)(ADDR_UART2_RX + uart2_rx_read_index);
	uart2_rx_read_index = (uint16_t)((uart2_rx_read_index + 1U) &
	                                 (UART2_DMA_RX_LEN - 1U));
	return 1U;
}

/**
 * @brief  UART2 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart2_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream2），确保DMA停止工作
    do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_2);
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_2));

    memcpy((uint8_t *)ADDR_UART2_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到UART2 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_2, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA1 Stream2所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA1->LIFCR, DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTCIF2 |
                       DMA_LIFCR_CTEIF2 | DMA_LIFCR_CDMEIF2 |
                       DMA_LIFCR_CFEIF2);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_2);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  USART2 error interrupt handler
 * @note   RX data remains in the circular DMA buffer and is polled by main.
 */
void USART2_IRQHandler(void)
{
	if (LL_USART_IsActiveFlag_ORE(USART2) ||
	    LL_USART_IsActiveFlag_FE(USART2) ||
	    LL_USART_IsActiveFlag_NE(USART2))
	{
		++uart2_rx_error_count;
		LL_USART_ClearFlag_ORE(USART2);
		LL_USART_ClearFlag_FE(USART2);
		LL_USART_ClearFlag_NE(USART2);
	}
}
#endif

#if defined(USART3_ENABLE)

volatile uart_receive_packet_t uart3_rx = {0};

/**
  * @brief  USART3 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PB10/PB11为USART3复用功能(TX/RX)
  */
static void uart3_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置USART3引脚 */
	/* 使能GPIOB外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOB);

	/* USART3 TX/RX 引脚配置
	   PB10 -> USART3_TX
	   PB11 -> USART3_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_10 | LL_GPIO_PIN_11;  // 配置PB10/PB11
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;           // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                  // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;               // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_7;                     // 复用功能为AF7(USART3) 
	LL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/**
  * @brief  USART3串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart3_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能USART3外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART3);

	/* USART3基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_921600;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(USART3, &USART_InitStruct);

	LL_USART_EnableDirectionTx(USART3);				// 使能USART3发送功能
	LL_USART_EnableDirectionRx(USART3);				// 使能USART3接收功能
	LL_USART_DisableOverrunDetect(USART3);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(USART3);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(USART3);				// 使能USART3 TX DMA请求
	LL_USART_EnableDMAReq_RX(USART3);				// 使能USART3 RX DMA请求

	NVIC_SetPriority(USART3_IRQn, (6UL << 1) + 1UL);				// USART3接收中断优先级
	NVIC_EnableIRQ(USART3_IRQn);					// 使能USART3接收中断	
	LL_USART_EnableIT_IDLE(USART3);					// 使能串口空闲中断（用于不定长数据接收）	
  LL_USART_EnableIT_ERROR(USART3);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(USART3);						// 使能USART3外设
  USART3->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  USART3 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA1 Stream4 -> USART3_TX
  *         DMA1 Stream5 -> USART3_RX
  */
static void uart3_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA1时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* 配置DMA发送通道（USART3_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART3->TDR;  // 外设地址：USART3发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART3_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART3_TX;  // DMA请求映射：USART3_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream4 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_4);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_4, &DMA_InitStruct);

	/* 配置DMA接收通道（USART3_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART3->RDR;  // 外设地址：USART3接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART3_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART3_RX; // DMA请求映射：USART3_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream5 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_5);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_5, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_4);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_5);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA1, LL_DMA_STREAM_4);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_5);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA1_Stream5_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA1_Stream5_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  USART3 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart3_init(uint32_t baudrate)
{
	uart3_gpio_config();   // 初始化串口GPIO
	uart3_dma_config();    // 初始化串口DMA
	uart3_config(baudrate);        // 初始化串口参数
	uart3_rx.interrupt_count = 0;	//初始化中断计数器
	uart3_rx.buffer_length = 0;		//初始化接收长度
	uart3_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  UART3 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart3_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream4），确保DMA停止工作
    do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_4);
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_4));

    memcpy((uint8_t *)ADDR_UART3_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到UART3 DMA发送缓冲区

    //SCB_CleanDCache_by_Addr((void volatile *)ADDR_UART3_TX, u8_len_temp);     // 清理缓存，把数据强制刷到内存

    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_4, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA1 Stream4所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA1->HIFCR, DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTCIF4 |
                       DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 |
                       DMA_HIFCR_CFEIF4);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_4);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  USART3 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void USART3_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(USART3) || LL_USART_IsActiveFlag_FE(USART3)
      || LL_USART_IsActiveFlag_NE(USART3))
  {
    LL_USART_ClearFlag_ORE(USART3);
    LL_USART_ClearFlag_FE(USART3);
    LL_USART_ClearFlag_NE(USART3);
    return;
  }
  
  if (LL_USART_IsActiveFlag_IDLE(USART3))
  {
    //LL_USART_ClearFlag_IDLE(USART3);            // 清除空闲中断
    USART3->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    USART3->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }
	uart3_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart3_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream5），确保DMA停止工作
	do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_5);
	while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_5));

	u8_len = UART3_DMA_RX_LEN - LL_DMA_GetDataLength(DMA1, LL_DMA_STREAM_5);	// 判断DMA中数据数量

  //SCB_InvalidateDCache_by_Addr((void volatile *)ADDR_UART3_RX, u8_len);     // 失效DMA接收缓冲区

	memcpy((void *)uart3_rx.buffer, (uint8_t *)ADDR_UART3_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart3_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_5, UART3_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_5, ADDR_UART3_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA1 Stream5所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA1->HIFCR, DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5 |
                       DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 |
                       DMA_HIFCR_CFEIF5);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_5);			// 重新使能DMA接收流，启动DMA接收
}
#endif

#if defined(UART4_ENABLE)

volatile uart_receive_packet_t uart4_rx;

/**
  * @brief  UART4 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PC10/PC11为UART4复用功能(TX/RX)
  */
static void uart4_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置UART4引脚 */
	/* 使能GPIOC外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOC);

	/* UART4 TX/RX 引脚配置
	   PC10 -> UART4_TX
	   PC11 -> UART4_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_10 | LL_GPIO_PIN_11;  // 配置PC10/PC11
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_8;                   // 复用功能为AF8(UART4) 
	LL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

/**
  * @brief  UART4串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart4_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能UART4外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART4);

	/* UART4基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_921600;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(UART4, &USART_InitStruct);

	LL_USART_EnableDirectionTx(UART4);				// 使能UART4发送功能
	LL_USART_EnableDirectionRx(UART4);				// 使能UART4接收功能
	LL_USART_DisableOverrunDetect(UART4);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(UART4);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(UART4);				// 使能UART4 TX DMA请求
	LL_USART_EnableDMAReq_RX(UART4);				// 使能UART4 RX DMA请求

	NVIC_SetPriority(UART4_IRQn, (6UL << 1) + 1UL);				// UART4接收中断优先级
	NVIC_EnableIRQ(UART4_IRQn);					// 使能UART4接收中断	
	LL_USART_EnableIT_IDLE(UART4);					// 使能串口空闲中断（用于不定长数据接收）
//  LL_USART_SetRxTimeout(UART4, 20);
//  LL_USART_EnableRxTimeout(UART4);
//  LL_USART_EnableIT_RTO(UART4);

  LL_USART_EnableIT_ERROR(UART4);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(UART4);						// 使能UART4外设
  UART4->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  UART4 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA1 Stream6 -> UART4_TX
  *         DMA1 Stream7 -> UART4_RX
  */
static void uart4_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA1时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	/* 配置DMA发送通道（UART4_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART4->TDR;  // 外设地址：UART4发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART4_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART4_TX;  // DMA请求映射：UART4_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream6 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_6);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_6, &DMA_InitStruct);

	/* 配置DMA接收通道（UART4_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART4->RDR;  // 外设地址：UART4接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART4_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART4_RX; // DMA请求映射：UART4_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA1 Stream1 */
	LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_7);
	LL_DMA_Init(DMA1, LL_DMA_STREAM_7, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_6);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_7);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA1, LL_DMA_STREAM_6);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_7);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA1_Stream7_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA1_Stream7_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  UART4 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart4_init(uint32_t baudrate)
{
	uart4_gpio_config();   // 初始化串口GPIO
	uart4_dma_config();    // 初始化串口DMA
	uart4_config(baudrate);        // 初始化串口参数
	uart4_rx.interrupt_count = 0;	//初始化中断计数器
	uart4_rx.buffer_length = 0;		//初始化接收长度
	uart4_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  UART4 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart4_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream6），确保DMA停止工作
    do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_6);
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_6));

    memcpy((uint8_t *)ADDR_UART4_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到uart4 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_6, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA1 Stream6所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA1->HIFCR, DMA_HIFCR_CHTIF6 | DMA_HIFCR_CTCIF6 |
                       DMA_HIFCR_CTEIF6 | DMA_HIFCR_CDMEIF6 |
                       DMA_HIFCR_CFEIF6);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_6);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  UART4 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void UART4_IRQHandler(void)
{
	__IO uint8_t u8_len;

  // 错误中断标志处理
  if (LL_USART_IsActiveFlag_ORE(UART4) || LL_USART_IsActiveFlag_FE(UART4)
      || LL_USART_IsActiveFlag_NE(UART4))
  {
    LL_USART_ClearFlag_ORE(UART4);
    LL_USART_ClearFlag_FE(UART4);
    LL_USART_ClearFlag_NE(UART4);
    return;
  }

  if (LL_USART_IsActiveFlag_IDLE(UART4))
  {
    //LL_USART_ClearFlag_IDLE(UART4);            // 清除空闲中断
    UART4->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    UART4->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	uart4_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart4_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream7），确保DMA停止工作
	do LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_7);
	while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_7));

	u8_len = UART4_DMA_RX_LEN - LL_DMA_GetDataLength(DMA1, LL_DMA_STREAM_7);	// 判断DMA中数据数量
	memcpy((void *)&uart4_rx.buffer, (uint8_t *)ADDR_UART4_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart4_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_7, UART4_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_7, ADDR_UART4_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA1 Stream7所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA1->HIFCR, DMA_HIFCR_CHTIF7 | DMA_HIFCR_CTCIF7 |
                       DMA_HIFCR_CTEIF7 | DMA_HIFCR_CDMEIF7 |
                       DMA_HIFCR_CFEIF7);
	LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_7);			// 重新使能DMA接收流，启动DMA接收
//  uart4_rx.buffer[uart4_rx.interrupt_count % 127] = LL_USART_ReceiveData8(UART4);
}
#endif

#if defined(UART5_ENABLE)

volatile uart_receive_packet_t uart5_rx;

/**
  * @brief  UART5 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PC12/PD2为UART5复用功能(TX/RX)
  */
static void uart5_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置UART5引脚 */
	/* 使能GPIOC，GPIOD外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOC);

	/* UART5 TX/RX 引脚配置
	   PC12 -> UART5_TX
	   PD2 -> UART5_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_12;  				// 配置PC12
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_8;                   // 复用功能为AF8(UART5) 
	LL_GPIO_Init(GPIOC, &GPIO_InitStruct);
	
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOD);
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_2;  				// 配置PD2
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_8;                   // 复用功能为AF8(UART5) 
	LL_GPIO_Init(GPIOD, &GPIO_InitStruct);
}

/**
  * @brief  UART5串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart5_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能UART5外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART5);

	/* UART5基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_460800;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(UART5, &USART_InitStruct);

	LL_USART_EnableDirectionTx(UART5);				// 使能UART5发送功能
	LL_USART_EnableDirectionRx(UART5);				// 使能UART5接收功能
	LL_USART_DisableOverrunDetect(UART5);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(UART5);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(UART5);				// 使能UART5 TX DMA请求
	LL_USART_EnableDMAReq_RX(UART5);				// 使能UART5 RX DMA请求

	NVIC_SetPriority(UART5_IRQn, (6UL << 1) + 1UL);				// UART5接收中断优先级
	NVIC_EnableIRQ(UART5_IRQn);					// 使能UART5接收中断	
	LL_USART_EnableIT_IDLE(UART5);					// 使能串口空闲中断（用于不定长数据接收）
  LL_USART_EnableIT_ERROR(UART5);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(UART5);						// 使能UART5外设
  UART5->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  UART5 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA2 Stream0 -> UART5_TX
  *         DMA2 Stream1 -> UART5_RX
  */
static void uart5_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA2时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

	/* 配置DMA发送通道（UART5_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART5->TDR;  // 外设地址：UART5发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART5_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART5_TX;  // DMA请求映射：UART5_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream0 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_0);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_0, &DMA_InitStruct);

	/* 配置DMA接收通道（UART5_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART5->RDR;  // 外设地址：UART5接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART5_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART5_RX; // DMA请求映射：UART5_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream1 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_1);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_1, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_0);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_1);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA2, LL_DMA_STREAM_0);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_1);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA2_Stream1_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA2_Stream1_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  UART5 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart5_init(uint32_t baudrate)
{
	uart5_gpio_config();   // 初始化串口GPIO
	uart5_dma_config();    // 初始化串口DMA
	uart5_config(baudrate);        // 初始化串口参数
	uart5_rx.interrupt_count = 0;	//初始化中断计数器
	uart5_rx.buffer_length = 0;		//初始化接收长度
	uart5_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  uart5 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart5_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream0），确保DMA停止工作
    do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_0);
    while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_0));

    memcpy((uint8_t *)ADDR_UART5_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到uart5 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_0, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA2 Stream0所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA2->LIFCR, DMA_LIFCR_CHTIF0 | DMA_LIFCR_CTCIF0 |
                       DMA_LIFCR_CTEIF0 | DMA_LIFCR_CDMEIF0 |
                       DMA_LIFCR_CFEIF0);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_0);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  UART5 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void UART5_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(UART5) || LL_USART_IsActiveFlag_FE(UART5)
      || LL_USART_IsActiveFlag_NE(UART5))
  {
    LL_USART_ClearFlag_ORE(UART5);
    LL_USART_ClearFlag_FE(UART5);
    LL_USART_ClearFlag_NE(UART5);
    return;
  }

  if (LL_USART_IsActiveFlag_IDLE(UART5))
  {
    //LL_USART_ClearFlag_IDLE(UART5);            // 清除空闲中断
    UART5->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    UART5->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	uart5_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart5_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream1），确保DMA停止工作
	do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_1);
	while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_1));

	u8_len = UART5_DMA_RX_LEN - LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_1);	// 判断DMA中数据数量
	memcpy((void *)uart5_rx.buffer, (uint8_t *)ADDR_UART5_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart5_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_1, UART5_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_1, ADDR_UART5_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA2 Stream1所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA2->LIFCR, DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1 |
                       DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 |
                       DMA_LIFCR_CFEIF1);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_1);			// 重新使能DMA接收流，启动DMA接收
}
#endif

#if defined(USART6_ENABLE)

volatile uart_receive_packet_t uart6_rx;

/**
  * @brief  USART6 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PC6/PC7为USART6复用功能(TX/RX)
  */
static void uart6_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置USART6引脚 */
	/* 使能GPIOC外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOC);

	/* USART6 TX/RX 引脚配置
	   PC6 -> USART6_TX
	   PC7 -> USART6_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_6 | LL_GPIO_PIN_7;  // 配置PC6/PC7
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_7;                   // 复用功能为AF7(USART6) 
	LL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

/**
  * @brief  USART6串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart6_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能USART6外设时钟 */
	LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART6);

	/* USART6基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_460800;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(USART6, &USART_InitStruct);

	LL_USART_EnableDirectionTx(USART6);				// 使能USART6发送功能
	LL_USART_EnableDirectionRx(USART6);				// 使能USART6接收功能
	LL_USART_DisableOverrunDetect(USART6);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(USART6);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(USART6);				// 使能USART6 TX DMA请求
	LL_USART_EnableDMAReq_RX(USART6);				// 使能USART6 RX DMA请求

	NVIC_SetPriority(USART6_IRQn, (6UL << 1) + 1UL);				// USART6接收中断优先级
	NVIC_EnableIRQ(USART6_IRQn);					// 使能USART6接收中断	
	LL_USART_EnableIT_IDLE(USART6);					// 使能串口空闲中断（用于不定长数据接收）
  LL_USART_EnableIT_ERROR(USART6);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(USART6);						// 使能USART6外设
  USART6->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  USART6 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA2 Stream2 -> USART6_TX
  *         DMA2 Stream3 -> USART6_RX
  */
static void uart6_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA2时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

	/* 配置DMA发送通道（USART6_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART6->TDR;  // 外设地址：USART6发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART6_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART6_TX;  // DMA请求映射：USART6_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream2 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_2, &DMA_InitStruct);

	/* 配置DMA接收通道（USART6_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&USART6->RDR;  // 外设地址：USART6接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART6_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_USART6_RX; // DMA请求映射：USART6_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream3 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_3);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_3, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_3);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA2, LL_DMA_STREAM_2);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_3);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA2_Stream3_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA2_Stream3_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  USART6 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart6_init(uint32_t baudrate)
{
	uart6_gpio_config();   // 初始化串口GPIO
	uart6_dma_config();    // 初始化串口DMA
	uart6_config(baudrate);        // 初始化串口参数
	uart6_rx.interrupt_count = 0;	//初始化中断计数器
	uart6_rx.buffer_length = 0;		//初始化接收长度
	uart6_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  UART6 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart6_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream2），确保DMA停止工作
    do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_2);
    while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_2));

    memcpy((uint8_t *)ADDR_UART6_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到UART6 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_2, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA2 Stream2所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA2->LIFCR, DMA_LIFCR_CHTIF2 | DMA_LIFCR_CTCIF2 |
                       DMA_LIFCR_CTEIF2 | DMA_LIFCR_CDMEIF2 |
                       DMA_LIFCR_CFEIF2);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_2);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  USART6 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void USART6_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(USART6) || LL_USART_IsActiveFlag_FE(USART6)
      || LL_USART_IsActiveFlag_NE(USART6))
  {
    LL_USART_ClearFlag_ORE(USART6);
    LL_USART_ClearFlag_FE(USART6);
    LL_USART_ClearFlag_NE(USART6);
    return;
  }

  if (LL_USART_IsActiveFlag_IDLE(USART6))
  {
    //LL_USART_ClearFlag_IDLE(USART6);            // 清除空闲中断
    USART6->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    USART6->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	uart6_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart6_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream3），确保DMA停止工作
	do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_3);
	while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_3));

	u8_len = UART6_DMA_RX_LEN - LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_3);	// 判断DMA中数据数量
	memcpy((void *)uart6_rx.buffer, (uint8_t *)ADDR_UART6_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart6_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_3, UART6_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_3, ADDR_UART6_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA2 Stream3所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA2->LIFCR, DMA_LIFCR_CHTIF3 | DMA_LIFCR_CTCIF3 |
                       DMA_LIFCR_CTEIF3 | DMA_LIFCR_CDMEIF3 |
                       DMA_LIFCR_CFEIF3);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_3);			// 重新使能DMA接收流，启动DMA接收
}
#endif


#if defined(UART7_ENABLE)

volatile uart_receive_packet_t uart7_rx;

/**
  * @brief  UART7 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PE8/PE7为UART7复用功能(TX/RX)
  */
static void uart7_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置UART7引脚 */
	/* 使能GPIOD外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOE);

	/* UART7 TX/RX 引脚配置
	   PE8 -> UART7_TX
	   PE7 -> UART7_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_7 | LL_GPIO_PIN_8;  // 配置PE8/PE7
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_7;                   // 复用功能为AF7(UART7) 
	LL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

/**
  * @brief  UART7串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart7_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能UART7外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART7);

	/* UART7基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_921600;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(UART7, &USART_InitStruct);

	LL_USART_EnableDirectionTx(UART7);				// 使能UART7发送功能
	LL_USART_EnableDirectionRx(UART7);				// 使能UART7接收功能
	LL_USART_DisableOverrunDetect(UART7);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(UART7);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(UART7);				// 使能UART7 TX DMA请求
	LL_USART_EnableDMAReq_RX(UART7);				// 使能UART7 RX DMA请求

	NVIC_SetPriority(UART7_IRQn, (6UL << 1) + 1UL);				// UART7接收中断优先级
	NVIC_EnableIRQ(UART7_IRQn);					// 使能UART7接收中断	
	LL_USART_EnableIT_IDLE(UART7);					// 使能串口空闲中断（用于不定长数据接收）
  LL_USART_EnableIT_ERROR(UART7);   // 使能错误中断（EIE=1），增强接收鲁棒性
	LL_USART_Enable(UART7);						// 使能UART7外设
  UART7->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  UART7 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA2 Stream4 -> UART7_TX
  *         DMA2 Stream5 -> UART7_RX
  */
static void uart7_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA2时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

	/* 配置DMA发送通道（UART7_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART7->TDR;  // 外设地址：UART7发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART7_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART7_TX;  // DMA请求映射：UART7_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream4 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_4);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_4, &DMA_InitStruct);

	/* 配置DMA接收通道（UART7_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART7->RDR;  // 外设地址：UART7接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART7_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART7_RX; // DMA请求映射：UART7_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream5 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_5);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_5, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_4);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_5);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA2, LL_DMA_STREAM_4);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_5);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA2_Stream5_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA2_Stream5_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  UART7 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart7_init(uint32_t baudrate)
{
	uart7_gpio_config();   // 初始化串口GPIO
	uart7_dma_config();    // 初始化串口DMA
	uart7_config(baudrate);        // 初始化串口参数
	uart7_rx.interrupt_count = 0;	//初始化中断计数器
	uart7_rx.buffer_length = 0;		//初始化接收长度
	uart7_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  uart7 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart7_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream4），确保DMA停止工作
    do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_4);
    while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_4));

    memcpy((uint8_t *)ADDR_UART7_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到uart7 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_4, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA2 Stream4所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA2->HIFCR, DMA_HIFCR_CHTIF4 | DMA_HIFCR_CTCIF4 |
                       DMA_HIFCR_CTEIF4 | DMA_HIFCR_CDMEIF4 |
                       DMA_HIFCR_CFEIF4);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_4);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  UART7 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void UART7_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(UART7) || LL_USART_IsActiveFlag_FE(UART7)
      || LL_USART_IsActiveFlag_NE(UART7))
  {
    LL_USART_ClearFlag_ORE(UART7);
    LL_USART_ClearFlag_FE(UART7);
    LL_USART_ClearFlag_NE(UART7);
    return;
  }

  if (LL_USART_IsActiveFlag_IDLE(UART7))
  {
    //LL_USART_ClearFlag_IDLE(UART7);            // 清除空闲中断
    UART7->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    UART7->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	uart7_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart7_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream5），确保DMA停止工作
	do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_5);
	while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_5));

	u8_len = UART7_DMA_RX_LEN - LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_5);	// 判断DMA中数据数量
	memcpy((void *)uart7_rx.buffer, (uint8_t *)ADDR_UART7_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart7_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_5, UART7_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_5, ADDR_UART7_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA2 Stream5所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA2->HIFCR, DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5 |
                       DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 |
                       DMA_HIFCR_CFEIF5);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_5);			// 重新使能DMA接收流，启动DMA接收
}
#endif

#if defined(UART8_ENABLE)

volatile uart_receive_packet_t uart8_rx;

/**
  * @brief  UART8 GPIO引脚初始化配置
  * @param  无
  * @retval 无
  * @note   配置PE1/PE0为UART8复用功能(TX/RX)
  */
static void uart8_gpio_config(void)
{
	/* GPIO初始化结构体定义 */
	LL_GPIO_InitTypeDef  GPIO_InitStruct;

	/* 开启GPIO时钟，配置UART8引脚 */
	/* 使能GPIOE外设时钟 */
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOE);

	/* UART8 TX/RX 引脚配置
	   PE1 -> UART8_TX
	   PE0 -> UART8_RX
	*/
	GPIO_InitStruct.Pin       = LL_GPIO_PIN_1 | LL_GPIO_PIN_0;  // 配置PE1/PE0
	GPIO_InitStruct.Mode      = LL_GPIO_MODE_ALTERNATE;         // 复用功能模式
	GPIO_InitStruct.Pull      = LL_GPIO_PULL_UP;                // 上拉输入
	GPIO_InitStruct.Speed     = LL_GPIO_SPEED_HIGH;             // 高速IO
	GPIO_InitStruct.Alternate = LL_GPIO_AF_8;                   // 复用功能为AF8(UART8) 
	LL_GPIO_Init(GPIOE, &GPIO_InitStruct);
}

/**
  * @brief  UART8串口参数配置
  * @param  无
  * @retval 无
  * @note   波特率460800bps，8位数据，无校验，1位停止位，开启DMA收发
  */
static void uart8_config(uint32_t baudrate)
{
	/* USART初始化结构体定义 */
	LL_USART_InitTypeDef   USART_InitStruct;

	/* 使能UART8外设时钟 */
	LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART8);

	/* UART8基础参数配置 */
	USART_InitStruct.PrescalerValue      = LL_USART_PRESCALER_DIV1;    // 时钟预分频1
	USART_InitStruct.BaudRate            = baudrate;//USART_BAUDRATE_460800;      // 波特率460800bps
	USART_InitStruct.DataWidth           = LL_USART_DATAWIDTH_8B;       // 8位数据位
	USART_InitStruct.StopBits            = LL_USART_STOPBITS_1;         // 1位停止位
	USART_InitStruct.Parity              = LL_USART_PARITY_NONE;        // 无校验位
	USART_InitStruct.TransferDirection   = LL_USART_DIRECTION_TX_RX;    // 收发模式
	USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;     // 无硬件流控
	USART_InitStruct.OverSampling        = LL_USART_OVERSAMPLING_8;     // 8倍过采样
	LL_USART_Init(UART8, &USART_InitStruct);

	LL_USART_EnableDirectionTx(UART8);				// 使能UART8发送功能
	LL_USART_EnableDirectionRx(UART8);				// 使能UART8接收功能
	LL_USART_DisableOverrunDetect(UART8);			// 关闭溢出检测（避免数据丢失时串口卡死）
	LL_USART_EnableDMADeactOnRxErr(UART8);			// 接收错误时不关闭DMA（保证DMA持续运行）	
	LL_USART_EnableDMAReq_TX(UART8);				// 使能UART8 TX DMA请求
	LL_USART_EnableDMAReq_RX(UART8);				// 使能UART8 RX DMA请求

	NVIC_SetPriority(UART8_IRQn, (6UL << 1) + 1UL);				// UART8接收中断优先级
	NVIC_EnableIRQ(UART8_IRQn);					// 使能UART8接收中断	
	LL_USART_EnableIT_IDLE(UART8);					// 使能串口空闲中断（用于不定长数据接收）
  LL_USART_EnableIT_ERROR(UART8);   // 使能错误中断（EIE=1），增强接收鲁棒性	
	LL_USART_Enable(UART8);						// 使能UART8外设
  UART8->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
}

/**
  * @brief  UART8 DMA收发配置
  * @param  无
  * @retval 无
  * @note   DMA2 Stream6 -> UART8_TX
  *         DMA2 Stream7 -> UART8_RX
  */
static void uart8_dma_config(void)
{
	LL_DMA_InitTypeDef DMA_InitStruct;
	/* 使能DMA2时钟 */
	LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

	/* 配置DMA发送通道（UART8_TX）*/
	/* DMA初始化结构体默认值 */
	/* 发送方向：内存 -> 外设 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART8->TDR;  // 外设地址：UART8发送数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART8_TX;           // 内存地址：发送缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;      // DMA模式（非循环，非外设）
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT; // 外设地址不自增
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;   // 内存地址自增
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;    // 外设数据宽度：字节
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;    // 内存数据宽度：字节
	DMA_InitStruct.NbData                      = 0x000000FFU;                // 待传输数据长度（初始0）
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART8_TX;  // DMA请求映射：UART8_TX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_LOW;       // 优先级：低
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;    // 关闭FIFO
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;       // 单次传输
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE; // 关闭双缓冲
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream6 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_6);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_6, &DMA_InitStruct);

	/* 配置DMA接收通道（UART8_RX）*/
	/* 接收方向：外设 -> 内存 */
	DMA_InitStruct.PeriphOrM2MSrcAddress       = (uint32_t)&UART8->RDR;  // 外设地址：UART8接收数据寄存器
	DMA_InitStruct.MemoryOrM2MDstAddress       = ADDR_UART8_RX;           // 内存地址：接收缓冲区
	DMA_InitStruct.Direction                   = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
	DMA_InitStruct.Mode                        = LL_DMA_MODE_NORMAL;
	DMA_InitStruct.PeriphOrM2MSrcIncMode       = LL_DMA_PERIPH_NOINCREMENT;
	DMA_InitStruct.MemoryOrM2MDstIncMode       = LL_DMA_MEMORY_INCREMENT;
	DMA_InitStruct.PeriphOrM2MSrcDataSize      = LL_DMA_PDATAALIGN_BYTE;
	DMA_InitStruct.MemoryOrM2MDstDataSize      = LL_DMA_MDATAALIGN_BYTE;
	DMA_InitStruct.NbData                      = 0x000000FFU;
	DMA_InitStruct.PeriphRequest               = LL_DMAMUX1_REQ_UART8_RX; // DMA请求映射：UART8_RX
	DMA_InitStruct.Priority                    = LL_DMA_PRIORITY_HIGH;      // 优先级：高
	DMA_InitStruct.FIFOMode                    = LL_DMA_FIFOMODE_DISABLE;
	DMA_InitStruct.FIFOThreshold               = LL_DMA_FIFOTHRESHOLD_1_4;
	DMA_InitStruct.MemBurst                    = LL_DMA_MBURST_SINGLE;
	DMA_InitStruct.PeriphBurst                 = LL_DMA_PBURST_SINGLE;
	DMA_InitStruct.DoubleBufferMode            = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
	DMA_InitStruct.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

	/* 初始化DMA2 Stream7 */
	LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_7);
	LL_DMA_Init(DMA2, LL_DMA_STREAM_7, &DMA_InitStruct);

	/* 使能DMA发送/接收流 */
//	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_6);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_7);

	/* DMA中断配置 */
//	LL_DMA_DisableIT_TC(DMA2, LL_DMA_STREAM_6);  // 关闭TX DMA完成中断
//	LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_7);   // 使能RX DMA完成中断
//	NVIC_SetPriority(DMA2_Stream7_IRQn, 0);      // DMA接收中断优先级
//	NVIC_EnableIRQ(DMA2_Stream7_IRQn);           // 使能DMA接收中断
}

/**
  * @brief  UART8 总初始化函数
  * @param  无
  * @retval 无
  * @note   调用顺序：GPIO -> DMA -> USART
  */
void uart8_init(uint32_t baudrate)
{
	uart8_gpio_config();   // 初始化串口GPIO
	uart8_dma_config();    // 初始化串口DMA
	uart8_config(baudrate);        // 初始化串口参数
	uart8_rx.interrupt_count = 0;	//初始化中断计数器
	uart8_rx.buffer_length = 0;		//初始化接收长度
	uart8_rx.interrupt_flag = 0;	//初始化中断标志
}

/**
 * @brief  uart8 DMA方式发送数据
 * @param  u8Datas: 待发送的数据缓冲区指针
 * @param  u8Lenght: 待发送的数据长度
 * @retval 实际发送长度（本函数直接返回传入长度）
 */
uint8_t uart8_transmit(const uint8_t u8_data[], uint8_t u8_length)
{
    // 定义返回值，初始化为要发送的长度（可在此处做长度越界判断）
    __IO uint8_t u8_len_temp = u8_length;

    // 循环等待：先关闭DMA发送流（Stream6），确保DMA停止工作
    do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_6);
    while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_6));

    memcpy((uint8_t *)ADDR_UART8_TX, u8_data, u8_len_temp);// 将待发送数据拷贝到uart8 DMA发送缓冲区

    LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_6, u8_len_temp);// 设置DMA本次要发送的数据长度

    // 清除DMA2 Stream6所有中断标志位（半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
    WRITE_REG(DMA2->HIFCR, DMA_HIFCR_CHTIF6 | DMA_HIFCR_CTCIF6 |
                       DMA_HIFCR_CTEIF6 | DMA_HIFCR_CDMEIF6 |
                       DMA_HIFCR_CFEIF6);

    // 重新使能DMA发送流，启动DMA发送
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_6);

    // 返回实际发送长度
    return u8_len_temp;
}

/**
 * @brief  UART8 中断服务函数（空闲中断 + DMA 不定长接收）
 * @note   串口空闲中断触发 → 表示一帧数据接收完成
 *         关闭DMA → 读取数据长度 → 拷贝数据 → 重启DMA
 */
void UART8_IRQHandler(void)
{
	__IO uint8_t u8_len;

  if (LL_USART_IsActiveFlag_ORE(UART8) || LL_USART_IsActiveFlag_FE(UART8)
      || LL_USART_IsActiveFlag_NE(UART8))
  {
    LL_USART_ClearFlag_ORE(UART8);
    LL_USART_ClearFlag_FE(UART8);
    LL_USART_ClearFlag_NE(UART8);
    return;
  }

  if (LL_USART_IsActiveFlag_IDLE(UART8))
  {
    //LL_USART_ClearFlag_IDLE(UART8);            // 清除空闲中断
    UART8->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
  }
  else
  {
    UART8->ICR = 0x123B3F;       // 请所有中断标志，本项目中只开了idle中断
    return;
  }

	uart8_rx.interrupt_count++;		// 中断计数器累加，中断心跳
	uart8_rx.interrupt_flag = 1;	// 中断标志，在主程序中查询解包，然后清除

	// 循环等待：先关闭DMA发送流（Stream7），确保DMA停止工作
	do LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_7);
	while (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_7));

	u8_len = UART8_DMA_RX_LEN - LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_7);	// 判断DMA中数据数量
	memcpy((void *)uart8_rx.buffer, (uint8_t *)ADDR_UART8_RX, u8_len);	// 将DMA接收数据拷贝到数组，主程序解包
	uart8_rx.buffer_length = u8_len;	// 保存本次接收的数据长度，供主程序使用

	LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_7, UART8_DMA_RX_LEN);	// 重新设置 DMA 传输长度
	LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_7, ADDR_UART8_RX);	// 重新设置 DMA 目标内存地址
	// 清除DMA2 Stream7所有中断标志位
	// （半传输、传输完成、传输错误、直接模式错误、FIFO 错误等）
  WRITE_REG(DMA2->HIFCR, DMA_HIFCR_CHTIF7 | DMA_HIFCR_CTCIF7 |
                       DMA_HIFCR_CTEIF7 | DMA_HIFCR_CDMEIF7 |
                       DMA_HIFCR_CFEIF7);
	LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_7);			// 重新使能DMA接收流，启动DMA接收
}
#endif
