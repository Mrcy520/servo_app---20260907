#ifndef __GSA200_H__
#define __GSA200_H__

#include <stdint.h>

typedef struct
{
    float gyro_dps[3];
    float integrated_angle_deg[3];
    float accel_g[3];
    float temperature_c;
    uint32_t valid_frames;
    uint32_t checksum_errors;
    uint32_t format_errors;
    uint32_t dropped_frames;
    uint32_t active_baudrate;
    uint8_t sample_counter;
    uint8_t data_valid;
    uint8_t last_frame[32];
} gsa200_data_t;

extern volatile gsa200_data_t g_gsa200;

/* Flat debug mirrors for Cortex-Debug Live Watch. */
extern volatile float gsa200_gyro_x_dps;
extern volatile float gsa200_gyro_y_dps;
extern volatile float gsa200_gyro_z_dps;
extern volatile float gsa200_angle_x_deg;
extern volatile float gsa200_angle_y_deg;
extern volatile float gsa200_angle_z_deg;
extern volatile float gsa200_accel_x_g;
extern volatile float gsa200_accel_y_g;
extern volatile float gsa200_accel_z_g;
extern volatile float gsa200_temperature_c;
extern volatile uint32_t gsa200_sample_counter;
extern volatile uint32_t gsa200_valid_frames;
extern volatile uint32_t gsa200_checksum_errors;
extern volatile uint32_t gsa200_format_errors;
extern volatile uint32_t gsa200_dropped_frames;
extern volatile uint32_t gsa200_frame_word0;
extern volatile uint32_t gsa200_frame_word1;
extern volatile uint32_t gsa200_frame_word2;
extern volatile uint32_t gsa200_frame_word3;
extern volatile uint32_t gsa200_frame_word4;
extern volatile uint32_t gsa200_frame_word5;
extern volatile uint32_t gsa200_frame_word6;
extern volatile uint32_t gsa200_frame_word7;

void gsa200_init(void);
void gsa200_poll(uint32_t now_ms);

#endif /* __GSA200_H__ */
