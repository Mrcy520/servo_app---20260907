#ifndef GSA200_UART5_H
#define GSA200_UART5_H

#include <stdint.h>

/* UART5/J2 receives the same 32-byte GSA200 frame as USART2/J5. */
typedef struct
{
    float gyro_dps[3];
    float integrated_angle_deg[3];
    float accel_g[3];
    float temperature_c;
    uint8_t sample_counter;
    uint8_t last_frame[32];
    uint32_t valid_frames;
    uint32_t checksum_errors;
    uint32_t format_errors;
    uint32_t dropped_frames;
    uint8_t data_valid;
    uint32_t active_baudrate;
} gsa200_uart5_data_t;

extern volatile gsa200_uart5_data_t g_gsa200_uart5;

extern volatile float gsa200_uart5_gyro_x_dps;
extern volatile float gsa200_uart5_gyro_y_dps;
extern volatile float gsa200_uart5_gyro_z_dps;
extern volatile float gsa200_uart5_angle_x_deg;
extern volatile float gsa200_uart5_angle_y_deg;
extern volatile float gsa200_uart5_angle_z_deg;
extern volatile float gsa200_uart5_accel_x_g;
extern volatile float gsa200_uart5_accel_y_g;
extern volatile float gsa200_uart5_accel_z_g;
extern volatile float gsa200_uart5_temperature_c;
extern volatile uint32_t gsa200_uart5_sample_counter;
extern volatile uint32_t gsa200_uart5_valid_frames;
extern volatile uint32_t gsa200_uart5_checksum_errors;
extern volatile uint32_t gsa200_uart5_format_errors;
extern volatile uint32_t gsa200_uart5_dropped_frames;
extern volatile uint8_t gsa200_uart5_data_valid;
extern volatile uint32_t gsa200_uart5_active_baudrate;

void gsa200_uart5_init(uint32_t baudrate);
void gsa200_uart5_poll(uint32_t now_ms);

#endif /* GSA200_UART5_H */
