#ifndef PCU_SPEED_H
#define PCU_SPEED_H

#include <stdint.h>

typedef enum
{
    PCU_SPEED_IDLE = 0U,
    PCU_SPEED_RUNNING,
    PCU_SPEED_FAULT,
    PCU_SPEED_TIMEOUT
} pcu_speed_state_t;

typedef enum
{
    PCU_LOOP_MODE_NONE = 0U,
    PCU_LOOP_MODE_SPEED,
    PCU_LOOP_MODE_POSITION
} pcu_loop_mode_t;

typedef enum
{
    PCU_SPEED_FEEDBACK_PCU_DIFFERENTIAL = 0U,
    PCU_SPEED_FEEDBACK_GYRO = 1U
} pcu_speed_feedback_source_t;

void pcu_speed_init(void);
void pcu_speed_poll(uint32_t now_ms);

uint8_t pcu_speed_start(void);
uint8_t pcu_speed_start_position(void);
void pcu_speed_stop(void);
uint8_t pcu_speed_set_target_rpm(float target_rpm);
uint8_t pcu_speed_set_position_reference_rpm(float reference_rpm);
float pcu_speed_get_target_rpm(void);
uint8_t pcu_speed_set_sine_params(float base_rpm, float amplitude_rpm,
                                  float frequency_hz);
uint8_t pcu_speed_sine_start(void);
void pcu_speed_sine_stop(void);
uint8_t pcu_speed_sine_is_active(void);
float pcu_speed_get_sine_base_rpm(void);
float pcu_speed_get_sine_amplitude_rpm(void);
float pcu_speed_get_sine_frequency_hz(void);
float pcu_speed_get_reference_rpm(void);
uint8_t pcu_speed_set_kp(float kp_a_per_rpm);
uint8_t pcu_speed_set_ki(float ki_a_per_rpm_s);
uint8_t pcu_speed_set_max_current(float max_current_a);
uint8_t pcu_speed_set_feedback_source(pcu_speed_feedback_source_t source);
float pcu_speed_get_kp(void);
float pcu_speed_get_ki(void);
float pcu_speed_get_max_current(void);
pcu_speed_feedback_source_t pcu_speed_get_feedback_source(void);
pcu_speed_state_t pcu_speed_get_state(void);
pcu_loop_mode_t pcu_speed_get_loop_mode(void);
uint8_t pcu_speed_is_active(void);

extern volatile float pcu_speed_feedback_rpm;
extern volatile float pcu_speed_pcu_differential_rpm;
extern volatile float pcu_speed_gyro_rpm;
extern volatile uint32_t pcu_speed_feedback_age_ms;
extern volatile float pcu_speed_error_rpm;
extern volatile float pcu_speed_reference_rpm;
extern volatile float pcu_speed_current_a;
extern volatile float pcu_speed_output_norm;
extern volatile int16_t pcu_speed_current_raw;
extern volatile uint32_t pcu_speed_control_updates;
extern volatile uint32_t pcu_speed_feedback_timeouts;
extern volatile uint32_t pcu_speed_command_errors;

#endif /* PCU_SPEED_H */
