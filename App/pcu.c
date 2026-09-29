#include "pcu.h"

#include <string.h>

#include "bsp_uart.h"
#include "stm32h7xx_ll_cortex.h"

volatile pcu_status_t g_pcu_status;

/* 当前驱动器 ID。协议默认值为 0x55，也可以在 pcu_init() 中传入其他 ID。 */
static uint8_t pcu_device_id;

/* UART4 接收帧缓存和当前已接收字节数。 */
static uint8_t pcu_rx_frame[PCU_FRAME_SIZE];
static uint8_t pcu_rx_index;

/* 检查控制模式是否属于协议定义的有效值。 */
static uint8_t pcu_control_mode_valid(pcu_control_mode_t control_mode)
{
    switch (control_mode)
    {
    case PCU_CONTROL_VOLTAGE_OPEN_LOOP:
    case PCU_CONTROL_CURRENT_CLOSED_LOOP:
    case PCU_CONTROL_AUTO_ZERO:
    case PCU_CONTROL_FAULT_CLEAR:
    case PCU_CONTROL_RESERVED_7:
    case PCU_CONTROL_RESERVED_8:
    case PCU_CONTROL_RESERVED_9:
        return 1U;
    default:
        return 0U;
    }
}

static void pcu_copy_last_frame(const uint8_t frame[PCU_FRAME_SIZE])
{
    uint8_t index;

    /* g_pcu_status 是 volatile，逐字节复制可避免直接使用不兼容的指针类型。 */
    for (index = 0U; index < PCU_FRAME_SIZE; ++index)
    {
        g_pcu_status.last_frame[index] = frame[index];
    }
}

static void pcu_accept_status_frame(const uint8_t frame[PCU_FRAME_SIZE])
{
    pcu_status_frame_t status_frame;
    uint32_t angle_raw;

    /* 状态帧在协议中按字节排列，先复制到结构体，再提取 24 位角度。 */
    memcpy(&status_frame, frame, sizeof(status_frame));
    angle_raw = pcu_angle_raw_get(&status_frame);

    g_pcu_status.device_id = status_frame.device_id;
    g_pcu_status.status = status_frame.status;
    g_pcu_status.angle_raw = angle_raw;
    /* 角度原始值范围为 0~0xFFFFFF，对应 0~360 度。 */
    g_pcu_status.angle_deg = ((float)angle_raw * 360.0f) / 16777216.0f;
    g_pcu_status.data_valid = 1U;
    g_pcu_status.valid_frames++;
    pcu_copy_last_frame(frame);
}

static void pcu_process_byte(uint8_t byte)
{
    if (pcu_rx_index == 0U)
    {
        /* 空闲时只寻找帧头，避免从任意位置误解析。 */
        if (byte == PCU_FRAME_HEADER)
        {
            pcu_rx_frame[pcu_rx_index++] = byte;
        }
        return;
    }

    /* 已找到帧头，继续收集剩余 6 个字节。 */
    pcu_rx_frame[pcu_rx_index++] = byte;
    if (pcu_rx_index < PCU_FRAME_SIZE)
    {
        return;
    }

    /* 先检查设备 ID，再检查校验和，最后才更新有效状态。 */
    if (pcu_rx_frame[1] != pcu_device_id)
    {
        g_pcu_status.format_errors++;
    }
    else if (!pcu_checksum_valid(pcu_rx_frame))
    {
        g_pcu_status.checksum_errors++;
    }
    else
    {
        pcu_accept_status_frame(pcu_rx_frame);
    }

    pcu_rx_index = 0U;
}

void pcu_init(uint32_t baudrate, uint8_t device_id)
{
    /* 清除上一次运行遗留的状态、统计值和半帧数据。 */
    memset((void *)&g_pcu_status, 0, sizeof(g_pcu_status));
    memset(pcu_rx_frame, 0, sizeof(pcu_rx_frame));
    pcu_rx_index = 0U;
    pcu_device_id = device_id;

    /* UART4 的 GPIO、DMA、空闲中断和串口参数由 BSP 统一初始化。 */
    uart4_init(baudrate);
}

void pcu_poll(void)
{
    uint8_t length;
    uint8_t index;

    /* ISR 会同时修改 uart4_rx，读取前暂时关闭 UART4 中断，避免竞态。 */
    NVIC_DisableIRQ(UART4_IRQn);
    if (uart4_rx.interrupt_flag == 0U)
    {
        NVIC_EnableIRQ(UART4_IRQn);
        return;
    }

    /* UART4 ISR 在检测到总线空闲时，会把本次 DMA 收到的数据放入该缓冲区。 */
    length = uart4_rx.buffer_length;
    uart4_rx.interrupt_flag = 0U;

    for (index = 0U; index < length; ++index)
    {
        pcu_process_byte(uart4_rx.buffer[index]);
    }

    NVIC_EnableIRQ(UART4_IRQn);
}

uint8_t pcu_send_command(uint8_t output_enable,
                         pcu_control_mode_t control_mode,
                         int16_t setpoint)
{
    pcu_command_frame_t command;

    /* 协议只允许 0/1 使能值、规定的控制模式和 -32767~32767 设定值。 */
    if ((output_enable > PCU_OUTPUT_ENABLED) ||
        (!pcu_control_mode_valid(control_mode)) ||
        (setpoint < PCU_SETPOINT_MIN))
    {
        return 0U;
    }

    /* 按协议填写 7 字节控制帧。设定值为有符号 16 位小端格式。 */
    command.header = PCU_FRAME_HEADER;
    command.device_id = pcu_device_id;
    command.output_enable = output_enable;
    command.control_mode = (uint8_t)control_mode;
    pcu_setpoint_set(&command, setpoint);
    /* 校验和是前 6 个字节累加后的低 8 位。 */
    command.checksum = pcu_checksum((const uint8_t *)&command);

    return (uint8_t)(uart4_transmit((const uint8_t *)&command,
                                    PCU_FRAME_SIZE) == PCU_FRAME_SIZE);
}

uint8_t pcu_set_voltage(int16_t setpoint)
{
    /* 电压开环：-32767~32767 对应母线电压的 -100%~100%。 */
    return pcu_send_command(PCU_OUTPUT_ENABLED,
                            PCU_CONTROL_VOLTAGE_OPEN_LOOP,
                            setpoint);
}

uint8_t pcu_set_current(int16_t setpoint)
{
    /* 电流闭环：-32767~32767 对应最大输出电流的 -100%~100%。 */
    return pcu_send_command(PCU_OUTPUT_ENABLED,
                            PCU_CONTROL_CURRENT_CLOSED_LOOP,
                            setpoint);
}

uint8_t pcu_disable_output(void)
{
    /* 关闭输出时发送使能无效，设定值清零。 */
    return pcu_send_command(PCU_OUTPUT_DISABLED,
                            PCU_CONTROL_VOLTAGE_OPEN_LOOP,
                            0);
}

uint8_t pcu_auto_zero(int16_t zero_voltage)
{
    /* 自动找零命令，zero_voltage 为找零过程使用的电压设定值。 */
    return pcu_send_command(PCU_OUTPUT_ENABLED,
                            PCU_CONTROL_AUTO_ZERO,
                            zero_voltage);
}

uint8_t pcu_clear_fault(void)
{
    /* 故障清除命令通常在输出关闭后发送。 */
    return pcu_send_command(PCU_OUTPUT_DISABLED,
                            PCU_CONTROL_FAULT_CLEAR,
                            0);
}

uint8_t pcu_checksum(const uint8_t frame[PCU_FRAME_SIZE])
{
    uint8_t checksum = 0U;
    uint8_t index;

    /* 不包含最后一个校验和字节本身。 */
    for (index = 0U; index < (PCU_FRAME_SIZE - 1U); ++index)
    {
        checksum = (uint8_t)(checksum + frame[index]);
    }
    return checksum;
}

uint8_t pcu_checksum_valid(const uint8_t frame[PCU_FRAME_SIZE])
{
    return (uint8_t)(pcu_checksum(frame) == frame[PCU_FRAME_SIZE - 1U]);
}

uint8_t pcu_frame_header_valid(const uint8_t frame[PCU_FRAME_SIZE])
{
    return (uint8_t)(frame[0] == PCU_FRAME_HEADER);
}

uint32_t pcu_angle_raw_get(const pcu_status_frame_t *frame)
{
    /* 状态帧中的角度按高字节在前的 24 位格式传输。 */
    return ((uint32_t)frame->angle_msb << 16) |
           ((uint32_t)frame->angle_mid << 8) |
           (uint32_t)frame->angle_lsb;
}

void pcu_angle_raw_set(pcu_status_frame_t *frame, uint32_t raw)
{
    /* 只保留角度原始值的低 24 位。 */
    raw &= PCU_ANGLE_RAW_MAX;
    frame->angle_msb = (uint8_t)(raw >> 16);
    frame->angle_mid = (uint8_t)(raw >> 8);
    frame->angle_lsb = (uint8_t)raw;
}

int16_t pcu_setpoint_get(const pcu_command_frame_t *frame)
{
    /* 控制帧中的设定值按低字节在前的小端格式传输。 */
    uint16_t raw = (uint16_t)frame->setpoint_lsb |
                   ((uint16_t)frame->setpoint_msb << 8);
    return (int16_t)raw;
}

void pcu_setpoint_set(pcu_command_frame_t *frame, int16_t value)
{
    uint16_t raw = (uint16_t)value;
    frame->setpoint_lsb = (uint8_t)raw;
    frame->setpoint_msb = (uint8_t)(raw >> 8);
}
