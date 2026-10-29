#include "gsa200_uart5.h"

#include <string.h>

#include "stm32h7xx_ll_bus.h"
#include "stm32h7xx_ll_dma.h"
#include "stm32h7xx_ll_gpio.h"
#include "stm32h7xx_ll_usart.h"
#include "bsp_uart.h"

#define GSA200_UART5_FRAME_SIZE       (32U)
#define GSA200_UART5_HEADER_AA        (0xAAU)
#define GSA200_UART5_HEADER_55        (0x55U)
#define GSA200_UART5_FORMAT_ID        (0x01U)
#define GSA200_UART5_DATA_TIMEOUT_MS  (1000U)
#define GSA200_UART5_SAMPLE_PERIOD_S  (0.0005f)
#define GSA200_UART5_DMA_RX_LEN       (256U)
#define GSA200_UART5_RX_ADDRESS       (ADDR_UART5_RX)

volatile gsa200_uart5_data_t g_gsa200_uart5;

volatile float gsa200_uart5_gyro_x_dps;
volatile float gsa200_uart5_gyro_y_dps;
volatile float gsa200_uart5_gyro_z_dps;
volatile float gsa200_uart5_angle_x_deg;
volatile float gsa200_uart5_angle_y_deg;
volatile float gsa200_uart5_angle_z_deg;
volatile float gsa200_uart5_accel_x_g;
volatile float gsa200_uart5_accel_y_g;
volatile float gsa200_uart5_accel_z_g;
volatile float gsa200_uart5_temperature_c;
volatile uint32_t gsa200_uart5_sample_counter;
volatile uint32_t gsa200_uart5_valid_frames;
volatile uint32_t gsa200_uart5_checksum_errors;
volatile uint32_t gsa200_uart5_format_errors;
volatile uint32_t gsa200_uart5_dropped_frames;
volatile uint8_t gsa200_uart5_data_valid;
volatile uint32_t gsa200_uart5_active_baudrate;

static uint8_t frame[GSA200_UART5_FRAME_SIZE];
static uint8_t frame_index;
static uint8_t have_previous_counter;
static uint16_t rx_read_index;
static uint32_t last_valid_frame_ms;

static void uart5_gpio_config(void)
{
    LL_GPIO_InitTypeDef gpio = {0};

    LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOC);
    LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOD);

    gpio.Pin = LL_GPIO_PIN_12;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Pull = LL_GPIO_PULL_UP;
    gpio.Speed = LL_GPIO_SPEED_HIGH;
    gpio.Alternate = LL_GPIO_AF_8;
    LL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = LL_GPIO_PIN_2;
    LL_GPIO_Init(GPIOD, &gpio);
}

static void uart5_dma_config(void)
{
    LL_DMA_InitTypeDef dma = {0};

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA2);

    dma.PeriphOrM2MSrcAddress = (uint32_t)&UART5->RDR;
    dma.MemoryOrM2MDstAddress = GSA200_UART5_RX_ADDRESS;
    dma.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    dma.Mode = LL_DMA_MODE_CIRCULAR;
    dma.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    dma.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    dma.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    dma.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    dma.NbData = GSA200_UART5_DMA_RX_LEN;
    dma.PeriphRequest = LL_DMAMUX1_REQ_UART5_RX;
    dma.Priority = LL_DMA_PRIORITY_HIGH;
    dma.FIFOMode = LL_DMA_FIFOMODE_DISABLE;
    dma.FIFOThreshold = LL_DMA_FIFOTHRESHOLD_1_4;
    dma.MemBurst = LL_DMA_MBURST_SINGLE;
    dma.PeriphBurst = LL_DMA_PBURST_SINGLE;
    dma.DoubleBufferMode = LL_DMA_DOUBLEBUFFER_MODE_DISABLE;
    dma.TargetMemInDoubleBufferMode = LL_DMA_CURRENTTARGETMEM0;

    LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_1);
    LL_DMA_Init(DMA2, LL_DMA_STREAM_1, &dma);
    WRITE_REG(DMA2->LIFCR, DMA_LIFCR_CHTIF1 | DMA_LIFCR_CTCIF1 |
                           DMA_LIFCR_CTEIF1 | DMA_LIFCR_CDMEIF1 |
                           DMA_LIFCR_CFEIF1);
    LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_1);
}

static void uart5_config(uint32_t baudrate)
{
    LL_USART_InitTypeDef usart = {0};

    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_UART5);

    usart.PrescalerValue = LL_USART_PRESCALER_DIV1;
    usart.BaudRate = baudrate;
    usart.DataWidth = LL_USART_DATAWIDTH_8B;
    usart.StopBits = LL_USART_STOPBITS_1;
    usart.Parity = LL_USART_PARITY_NONE;
    usart.TransferDirection = LL_USART_DIRECTION_RX;
    usart.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    usart.OverSampling = LL_USART_OVERSAMPLING_8;
    LL_USART_Init(UART5, &usart);

    LL_USART_EnableDirectionRx(UART5);
    LL_USART_DisableOverrunDetect(UART5);
    LL_USART_EnableDMADeactOnRxErr(UART5);
    LL_USART_EnableDMAReq_RX(UART5);
    LL_USART_Enable(UART5);
    UART5->ICR = 0x123B3FU;
}

static uint8_t uart5_read_byte(uint8_t *data)
{
    uint16_t write_index;

    if (data == NULL)
    {
        return 0U;
    }

    write_index = (uint16_t)((GSA200_UART5_DMA_RX_LEN -
                              LL_DMA_GetDataLength(DMA2, LL_DMA_STREAM_1)) &
                             (GSA200_UART5_DMA_RX_LEN - 1U));
    if (rx_read_index == write_index)
    {
        return 0U;
    }

    *data = *(volatile uint8_t *)(GSA200_UART5_RX_ADDRESS + rx_read_index);
    rx_read_index = (uint16_t)((rx_read_index + 1U) &
                               (GSA200_UART5_DMA_RX_LEN - 1U));
    return 1U;
}

static float read_float_be(const uint8_t *source)
{
    uint32_t bits = ((uint32_t)source[0] << 24) |
                    ((uint32_t)source[1] << 16) |
                    ((uint32_t)source[2] << 8) |
                    (uint32_t)source[3];
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void publish_status(void)
{
    gsa200_uart5_gyro_x_dps = g_gsa200_uart5.gyro_dps[0];
    gsa200_uart5_gyro_y_dps = g_gsa200_uart5.gyro_dps[1];
    gsa200_uart5_gyro_z_dps = g_gsa200_uart5.gyro_dps[2];
    gsa200_uart5_angle_x_deg = g_gsa200_uart5.integrated_angle_deg[0];
    gsa200_uart5_angle_y_deg = g_gsa200_uart5.integrated_angle_deg[1];
    gsa200_uart5_angle_z_deg = g_gsa200_uart5.integrated_angle_deg[2];
    gsa200_uart5_accel_x_g = g_gsa200_uart5.accel_g[0];
    gsa200_uart5_accel_y_g = g_gsa200_uart5.accel_g[1];
    gsa200_uart5_accel_z_g = g_gsa200_uart5.accel_g[2];
    gsa200_uart5_temperature_c = g_gsa200_uart5.temperature_c;
    gsa200_uart5_sample_counter = g_gsa200_uart5.sample_counter;
    gsa200_uart5_valid_frames = g_gsa200_uart5.valid_frames;
    gsa200_uart5_checksum_errors = g_gsa200_uart5.checksum_errors;
    gsa200_uart5_format_errors = g_gsa200_uart5.format_errors;
    gsa200_uart5_dropped_frames = g_gsa200_uart5.dropped_frames;
    gsa200_uart5_data_valid = g_gsa200_uart5.data_valid;
    gsa200_uart5_active_baudrate = g_gsa200_uart5.active_baudrate;
}

static void accept_frame(void)
{
    uint8_t sample_delta = 1U;
    uint8_t delta;
    uint8_t axis;
    int16_t raw_temperature;

    g_gsa200_uart5.gyro_dps[0] = read_float_be(&frame[4]);
    g_gsa200_uart5.gyro_dps[1] = read_float_be(&frame[8]);
    g_gsa200_uart5.gyro_dps[2] = read_float_be(&frame[12]);
    g_gsa200_uart5.accel_g[0] = read_float_be(&frame[16]);
    g_gsa200_uart5.accel_g[1] = read_float_be(&frame[20]);
    g_gsa200_uart5.accel_g[2] = read_float_be(&frame[24]);
    raw_temperature = (int16_t)(((uint16_t)frame[28] << 8) | frame[29]);
    g_gsa200_uart5.temperature_c = (float)raw_temperature * 0.01f;

    if (have_previous_counter != 0U)
    {
        delta = (uint8_t)(frame[30] - g_gsa200_uart5.sample_counter);
        if ((delta > 1U) && (delta < 128U))
        {
            g_gsa200_uart5.dropped_frames += (uint32_t)(delta - 1U);
            sample_delta = delta;
        }
        else if (delta != 1U)
        {
            sample_delta = 0U;
        }
    }

    for (axis = 0U; axis < 3U; ++axis)
    {
        g_gsa200_uart5.integrated_angle_deg[axis] +=
            g_gsa200_uart5.gyro_dps[axis] * GSA200_UART5_SAMPLE_PERIOD_S * sample_delta;
        if (g_gsa200_uart5.integrated_angle_deg[axis] > 180.0f)
        {
            g_gsa200_uart5.integrated_angle_deg[axis] -= 360.0f;
        }
        else if (g_gsa200_uart5.integrated_angle_deg[axis] < -180.0f)
        {
            g_gsa200_uart5.integrated_angle_deg[axis] += 360.0f;
        }
    }

    g_gsa200_uart5.sample_counter = frame[30];
    memcpy((void *)g_gsa200_uart5.last_frame, frame, GSA200_UART5_FRAME_SIZE);
    ++g_gsa200_uart5.valid_frames;
    g_gsa200_uart5.data_valid = 1U;
    have_previous_counter = 1U;
    publish_status();
}

static void process_byte(uint8_t byte)
{
    uint16_t checksum_sum = 0U;
    uint8_t checksum = 0U;
    uint8_t index;

    if (frame_index == 0U)
    {
        if (byte == GSA200_UART5_HEADER_AA)
        {
            frame[frame_index++] = byte;
        }
        return;
    }

    if (frame_index == 1U)
    {
        if (byte == GSA200_UART5_HEADER_55)
        {
            frame[frame_index++] = byte;
        }
        else if (byte == GSA200_UART5_HEADER_AA)
        {
            frame[0] = byte;
        }
        else
        {
            frame_index = 0U;
        }
        return;
    }

    frame[frame_index++] = byte;
    if (frame_index == 4U)
    {
        if ((frame[2] != GSA200_UART5_FORMAT_ID) ||
            (frame[3] != GSA200_UART5_FRAME_SIZE))
        {
            ++g_gsa200_uart5.format_errors;
            publish_status();
            frame_index = 0U;
        }
        return;
    }
    if (frame_index < GSA200_UART5_FRAME_SIZE)
    {
        return;
    }

    for (index = 0U; index < (GSA200_UART5_FRAME_SIZE - 1U); ++index)
    {
        checksum_sum = (uint16_t)(checksum_sum + frame[index]);
    }
    checksum = (uint8_t)checksum_sum;
    if (checksum == frame[GSA200_UART5_FRAME_SIZE - 1U])
    {
        accept_frame();
    }
    else
    {
        ++g_gsa200_uart5.checksum_errors;
        publish_status();
    }
    frame_index = 0U;
}

void gsa200_uart5_init(uint32_t baudrate)
{
    memset((void *)&g_gsa200_uart5, 0, sizeof(g_gsa200_uart5));
    memset(frame, 0, sizeof(frame));
    memset((void *)GSA200_UART5_RX_ADDRESS, 0, GSA200_UART5_DMA_RX_LEN);
    frame_index = 0U;
    have_previous_counter = 0U;
    rx_read_index = 0U;
    last_valid_frame_ms = 0U;
    g_gsa200_uart5.active_baudrate = baudrate;
    publish_status();
    uart5_gpio_config();
    uart5_dma_config();
    uart5_config(baudrate);
}

void gsa200_uart5_poll(uint32_t now_ms)
{
    uint8_t byte;
    uint32_t valid_frames_before = g_gsa200_uart5.valid_frames;

    while (uart5_read_byte(&byte) != 0U)
    {
        process_byte(byte);
    }

    if (g_gsa200_uart5.valid_frames != valid_frames_before)
    {
        last_valid_frame_ms = now_ms;
    }
    if ((g_gsa200_uart5.data_valid != 0U) &&
        ((uint32_t)(now_ms - last_valid_frame_ms) >= GSA200_UART5_DATA_TIMEOUT_MS))
    {
        g_gsa200_uart5.data_valid = 0U;
        gsa200_uart5_data_valid = 0U;
    }
}
