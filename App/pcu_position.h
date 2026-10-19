#ifndef PCU_POSITION_H
#define PCU_POSITION_H

#include <stdint.h>

typedef enum
{
    PCU_POSITION_IDLE = 0U,
    PCU_POSITION_MOVING,
    PCU_POSITION_REACHED,
    PCU_POSITION_HOLDING,
    PCU_POSITION_FAULT,
    PCU_POSITION_TIMEOUT
} pcu_position_state_t;

typedef enum
{
    PCU_POSITION_END_DISABLE = 0U,
    PCU_POSITION_END_HOLD
} pcu_position_end_mode_t;

void pcu_position_init(void);
void pcu_position_poll(uint32_t now_ms);
uint8_t pcu_position_start_relative(float delta_deg);
void pcu_position_stop(void);
uint8_t pcu_position_set_end_mode(pcu_position_end_mode_t mode);
pcu_position_end_mode_t pcu_position_get_end_mode(void);
uint8_t pcu_position_set_kp(float kp_rpm_per_deg);
uint8_t pcu_position_set_max_speed(float max_speed_rpm);
float pcu_position_get_kp(void);
float pcu_position_get_max_speed(void);
pcu_position_state_t pcu_position_get_state(void);
uint8_t pcu_position_is_active(void);

extern volatile int32_t pcu_position_current_mdeg;
extern volatile int32_t pcu_position_target_mdeg;
extern volatile int32_t pcu_position_error_mdeg;
extern volatile float pcu_position_speed_reference_rpm;

#endif /* PCU_POSITION_H */
