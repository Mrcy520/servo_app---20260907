#include "gsa200.h"

#include <string.h>

#include "bsp_uart.h"

#define GSA200_FRAME_SIZE       (32U)
#define GSA200_HEADER_AA        (0xAAU)
#define GSA200_HEADER_55        (0x55U)
#define GSA200_FORMAT_ID        (0x01U)
#define GSA200_BAUD_SWITCH_MS   (700U)
#define GSA200_SAMPLE_PERIOD_S  (0.0005f)

volatile gsa200_data_t g_gsa200;

volatile float gsa200_gyro_x_dps;
volatile float gsa200_gyro_y_dps;
volatile float gsa200_gyro_z_dps;
volatile float gsa200_angle_x_deg;
volatile float gsa200_angle_y_deg;
volatile float gsa200_angle_z_deg;
volatile float gsa200_accel_x_g;
volatile float gsa200_accel_y_g;
volatile float gsa200_accel_z_g;
volatile float gsa200_temperature_c;
volatile uint32_t gsa200_sample_counter;
volatile uint32_t gsa200_valid_frames;
volatile uint32_t gsa200_checksum_errors;
volatile uint32_t gsa200_format_errors;
volatile uint32_t gsa200_dropped_frames;
volatile uint32_t gsa200_frame_word0;
volatile uint32_t gsa200_frame_word1;
volatile uint32_t gsa200_frame_word2;
volatile uint32_t gsa200_frame_word3;
volatile uint32_t gsa200_frame_word4;
volatile uint32_t gsa200_frame_word5;
volatile uint32_t gsa200_frame_word6;
volatile uint32_t gsa200_frame_word7;

static uint8_t frame[GSA200_FRAME_SIZE];
static uint8_t frame_index;
static uint8_t have_previous_counter;
static uint32_t last_baud_switch_ms;

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

static void accept_frame(void)
{
    uint8_t sample_delta = 1U;
    uint8_t expected_counter;
    uint8_t missed;
    uint8_t axis;
    int16_t raw_temperature;

    g_gsa200.gyro_dps[0] = read_float_be(&frame[4]);
    g_gsa200.gyro_dps[1] = read_float_be(&frame[8]);
    g_gsa200.gyro_dps[2] = read_float_be(&frame[12]);
    g_gsa200.accel_g[0] = read_float_be(&frame[16]);
    g_gsa200.accel_g[1] = read_float_be(&frame[20]);
    g_gsa200.accel_g[2] = read_float_be(&frame[24]);

    raw_temperature = (int16_t)(((uint16_t)frame[28] << 8) |
                                (uint16_t)frame[29]);
    g_gsa200.temperature_c = (float)raw_temperature * 0.01f;

    if (have_previous_counter != 0U)
    {
        expected_counter = (uint8_t)(g_gsa200.sample_counter + 1U);
        sample_delta = (uint8_t)(frame[30] - g_gsa200.sample_counter);
        if (frame[30] != expected_counter)
        {
            missed = (uint8_t)(frame[30] - expected_counter);
            g_gsa200.dropped_frames += missed;
        }
    }

    for (axis = 0U; axis < 3U; ++axis)
    {
        g_gsa200.integrated_angle_deg[axis] +=
            g_gsa200.gyro_dps[axis] * GSA200_SAMPLE_PERIOD_S * sample_delta;

        if (g_gsa200.integrated_angle_deg[axis] > 180.0f)
        {
            g_gsa200.integrated_angle_deg[axis] -= 360.0f;
        }
        else if (g_gsa200.integrated_angle_deg[axis] < -180.0f)
        {
            g_gsa200.integrated_angle_deg[axis] += 360.0f;
        }
    }

    g_gsa200.sample_counter = frame[30];
    memcpy((void *)g_gsa200.last_frame, frame, GSA200_FRAME_SIZE);

    gsa200_gyro_x_dps = g_gsa200.gyro_dps[0];
    gsa200_gyro_y_dps = g_gsa200.gyro_dps[1];
    gsa200_gyro_z_dps = g_gsa200.gyro_dps[2];
    gsa200_angle_x_deg = g_gsa200.integrated_angle_deg[0];
    gsa200_angle_y_deg = g_gsa200.integrated_angle_deg[1];
    gsa200_angle_z_deg = g_gsa200.integrated_angle_deg[2];
    gsa200_accel_x_g = g_gsa200.accel_g[0];
    gsa200_accel_y_g = g_gsa200.accel_g[1];
    gsa200_accel_z_g = g_gsa200.accel_g[2];
    gsa200_temperature_c = g_gsa200.temperature_c;
    gsa200_sample_counter = g_gsa200.sample_counter;
    gsa200_valid_frames = g_gsa200.valid_frames;
    gsa200_checksum_errors = g_gsa200.checksum_errors;
    gsa200_format_errors = g_gsa200.format_errors;
    gsa200_dropped_frames = g_gsa200.dropped_frames;
    gsa200_frame_word0 = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16) |
                         ((uint32_t)frame[2] << 8) | frame[3];
    gsa200_frame_word1 = ((uint32_t)frame[4] << 24) | ((uint32_t)frame[5] << 16) |
                         ((uint32_t)frame[6] << 8) | frame[7];
    gsa200_frame_word2 = ((uint32_t)frame[8] << 24) | ((uint32_t)frame[9] << 16) |
                         ((uint32_t)frame[10] << 8) | frame[11];
    gsa200_frame_word3 = ((uint32_t)frame[12] << 24) | ((uint32_t)frame[13] << 16) |
                         ((uint32_t)frame[14] << 8) | frame[15];
    gsa200_frame_word4 = ((uint32_t)frame[16] << 24) | ((uint32_t)frame[17] << 16) |
                         ((uint32_t)frame[18] << 8) | frame[19];
    gsa200_frame_word5 = ((uint32_t)frame[20] << 24) | ((uint32_t)frame[21] << 16) |
                         ((uint32_t)frame[22] << 8) | frame[23];
    gsa200_frame_word6 = ((uint32_t)frame[24] << 24) | ((uint32_t)frame[25] << 16) |
                         ((uint32_t)frame[26] << 8) | frame[27];
    gsa200_frame_word7 = ((uint32_t)frame[28] << 24) | ((uint32_t)frame[29] << 16) |
                         ((uint32_t)frame[30] << 8) | frame[31];
    have_previous_counter = 1U;
    ++g_gsa200.valid_frames;
    g_gsa200.data_valid = 1U;
}

static void process_byte(uint8_t byte)
{
    uint8_t checksum = 0U;
    uint8_t index;

    if (frame_index == 0U)
    {
        if ((byte == GSA200_HEADER_AA) || (byte == GSA200_HEADER_55))
        {
            frame[frame_index++] = byte;
        }
        return;
    }

    if (frame_index == 1U)
    {
        if (((frame[0] == GSA200_HEADER_AA) && (byte == GSA200_HEADER_55)) ||
            ((frame[0] == GSA200_HEADER_55) && (byte == GSA200_HEADER_AA)))
        {
            frame[frame_index++] = byte;
        }
        else if ((byte == GSA200_HEADER_AA) || (byte == GSA200_HEADER_55))
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
        if ((frame[2] != GSA200_FORMAT_ID) ||
            (frame[3] != GSA200_FRAME_SIZE))
        {
            ++g_gsa200.format_errors;
            frame_index = 0U;
        }
        return;
    }

    if (frame_index < GSA200_FRAME_SIZE)
    {
        return;
    }

    for (index = 0U; index < (GSA200_FRAME_SIZE - 1U); ++index)
    {
        checksum = (uint8_t)(checksum + frame[index]);
    }

    if (checksum == frame[GSA200_FRAME_SIZE - 1U])
    {
        accept_frame();
    }
    else
    {
        ++g_gsa200.checksum_errors;
    }
    frame_index = 0U;
}

void gsa200_init(void)
{
    memset((void *)&g_gsa200, 0, sizeof(g_gsa200));
    g_gsa200.active_baudrate = BAUDRATE_460800;
    memset(frame, 0, sizeof(frame));
    frame_index = 0U;
    have_previous_counter = 0U;
    last_baud_switch_ms = 0U;

    gsa200_gyro_x_dps = 0.0f;
    gsa200_gyro_y_dps = 0.0f;
    gsa200_gyro_z_dps = 0.0f;
    gsa200_angle_x_deg = 0.0f;
    gsa200_angle_y_deg = 0.0f;
    gsa200_angle_z_deg = 0.0f;
    gsa200_accel_x_g = 0.0f;
    gsa200_accel_y_g = 0.0f;
    gsa200_accel_z_g = 0.0f;
    gsa200_temperature_c = 0.0f;
    gsa200_sample_counter = 0U;
    gsa200_valid_frames = 0U;
    gsa200_checksum_errors = 0U;
    gsa200_format_errors = 0U;
    gsa200_dropped_frames = 0U;
    gsa200_frame_word0 = 0U;
    gsa200_frame_word1 = 0U;
    gsa200_frame_word2 = 0U;
    gsa200_frame_word3 = 0U;
    gsa200_frame_word4 = 0U;
    gsa200_frame_word5 = 0U;
    gsa200_frame_word6 = 0U;
    gsa200_frame_word7 = 0U;
}

void gsa200_poll(uint32_t now_ms)
{
    uint8_t byte;

    while (uart2_read_byte(&byte) != 0U)
    {
        process_byte(byte);
    }

    if ((g_gsa200.data_valid == 0U) &&
        ((now_ms - last_baud_switch_ms) >= GSA200_BAUD_SWITCH_MS))
    {
        switch (g_gsa200.active_baudrate)
        {
        case BAUDRATE_460800:
            g_gsa200.active_baudrate = BAUDRATE_921600;
            break;
        case BAUDRATE_921600:
            g_gsa200.active_baudrate = BAUDRATE_115200;
            break;
        case BAUDRATE_115200:
            g_gsa200.active_baudrate = BAUDRATE_230400;
            break;
        default:
            g_gsa200.active_baudrate = BAUDRATE_460800;
            break;
        }
        uart2_init(g_gsa200.active_baudrate);
        frame_index = 0U;
        have_previous_counter = 0U;
        last_baud_switch_ms = now_ms;
    }
}
