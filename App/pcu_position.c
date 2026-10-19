#include "pcu_position.h"

#include <math.h>

#include "pcu.h"
#include "pcu_speed.h"

/* Conservative values calibrated against the connected motor. */
#define POSITION_LOOP_PERIOD_MS       (5U)
#define POSITION_FEEDBACK_TIMEOUT_MS  (5000U)
#define POSITION_MOVE_TIMEOUT_MS      (10000U)
#define POSITION_SETTLE_TIME_MS       (100U)
#define POSITION_TOLERANCE_DEG        (0.5f)
#define POSITION_DEFAULT_KP_RPM_PER_DEG (5.0f)
#define POSITION_DEFAULT_MAX_SPEED_RPM  (1000.0f)
#define POSITION_DIRECTION            (1.0f)

#define PCU_POSITION_FAULT_MASK (PCU_STATUS_ANGLE_FAULT | PCU_STATUS_OVERCURRENT | \
                                 PCU_STATUS_OVERTEMPERATURE | PCU_STATUS_MOTOR_OVERLOAD | \
                                 PCU_STATUS_BUS_UNDERVOLT | PCU_STATUS_BUS_OVERVOLT)

volatile int32_t pcu_position_current_mdeg;
volatile int32_t pcu_position_target_mdeg;
volatile int32_t pcu_position_error_mdeg;
volatile float pcu_position_speed_reference_rpm;

static pcu_position_state_t position_state;
static float current_angle_deg;
static float target_angle_deg;
static float previous_wrapped_angle_deg;
static uint8_t feedback_initialized;
static uint32_t last_feedback_frames;
static uint32_t last_feedback_ms;
static uint32_t move_start_ms;
static uint32_t settle_start_ms;
static uint32_t last_command_ms;
static uint8_t feedback_seen_during_move;
static float position_kp_rpm_per_deg;
static float position_max_speed_rpm;
static pcu_position_end_mode_t position_end_mode;

static int32_t position_mdeg(float angle_deg)
{
    float scaled = angle_deg * 1000.0f;

    return (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void position_publish(float error_deg)
{
    pcu_position_current_mdeg = position_mdeg(current_angle_deg);
    pcu_position_target_mdeg = position_mdeg(target_angle_deg);
    pcu_position_error_mdeg = position_mdeg(error_deg);
}

static void position_disable(void)
{
    pcu_speed_stop();
}

static void position_fail(pcu_position_state_t state)
{
    position_disable();
    position_state = state;
    settle_start_ms = 0U;
}

static void position_update_feedback(uint32_t now_ms)
{
    float wrapped_angle = g_pcu_status.angle_deg;
    float delta_deg;

    if (g_pcu_status.valid_frames == last_feedback_frames)
    {
        return;
    }

    last_feedback_frames = g_pcu_status.valid_frames;
    last_feedback_ms = now_ms;
    feedback_seen_during_move = 1U;

    if (feedback_initialized == 0U)
    {
        previous_wrapped_angle_deg = wrapped_angle;
        current_angle_deg = wrapped_angle;
        feedback_initialized = 1U;
        position_publish(0.0f);
        return;
    }

    delta_deg = wrapped_angle - previous_wrapped_angle_deg;
    if (delta_deg > 180.0f)
    {
        delta_deg -= 360.0f;
    }
    else if (delta_deg < -180.0f)
    {
        delta_deg += 360.0f;
    }

    current_angle_deg += delta_deg;
    previous_wrapped_angle_deg = wrapped_angle;
}

void pcu_position_init(void)
{
    position_state = PCU_POSITION_IDLE;
    current_angle_deg = 0.0f;
    target_angle_deg = 0.0f;
    previous_wrapped_angle_deg = 0.0f;
    feedback_initialized = 0U;
    last_feedback_frames = 0U;
    last_feedback_ms = 0U;
    move_start_ms = 0U;
    settle_start_ms = 0U;
    last_command_ms = 0U;
    feedback_seen_during_move = 0U;
    pcu_position_current_mdeg = 0;
    pcu_position_target_mdeg = 0;
    pcu_position_error_mdeg = 0;
    pcu_position_speed_reference_rpm = 0.0f;
    position_kp_rpm_per_deg = POSITION_DEFAULT_KP_RPM_PER_DEG;
    position_max_speed_rpm = POSITION_DEFAULT_MAX_SPEED_RPM;
    position_end_mode = PCU_POSITION_END_DISABLE;
}

uint8_t pcu_position_start_relative(float delta_deg)
{
    if (!isfinite(delta_deg) || (delta_deg < -3600.0f) ||
        (delta_deg > 3600.0f) || (g_pcu_status.data_valid == 0U) ||
        ((g_pcu_status.status & PCU_POSITION_FAULT_MASK) != 0U) ||
        (feedback_initialized == 0U) ||
        (position_state == PCU_POSITION_MOVING))
    {
        return 0U;
    }

    /* A held position can accept another relative move without leaving the
       position-control mode. A standalone speed loop remains exclusive. */
    if ((pcu_speed_is_active() != 0U) &&
        (pcu_speed_get_loop_mode() != PCU_LOOP_MODE_POSITION))
    {
        return 0U;
    }

    /* Manual current/voltage commands may move the shaft while the position
       loop is idle. Re-anchor the relative move to the latest encoder frame
       instead of using a stale unwrapped value from an earlier move. */
    current_angle_deg = g_pcu_status.angle_deg;
    previous_wrapped_angle_deg = g_pcu_status.angle_deg;
    target_angle_deg = current_angle_deg + delta_deg;
    if ((pcu_speed_is_active() == 0U) &&
        (pcu_speed_start_position() == 0U))
    {
        return 0U;
    }
    position_state = PCU_POSITION_MOVING;
    move_start_ms = 0U;
    last_feedback_ms = 0U;
    feedback_seen_during_move = 0U;
    settle_start_ms = 0U;
    last_command_ms = 0U;
    position_publish(delta_deg);
    pcu_position_speed_reference_rpm = 0.0f;
    (void)pcu_speed_set_position_reference_rpm(0.0f);
    return 1U;
}

void pcu_position_stop(void)
{
    if (pcu_position_is_active() != 0U)
    {
        position_disable();
    }
    position_state = PCU_POSITION_IDLE;
    settle_start_ms = 0U;
    pcu_position_speed_reference_rpm = 0.0f;
}

void pcu_position_poll(uint32_t now_ms)
{
    float error_deg;
    float speed_reference_rpm;

    position_update_feedback(now_ms);
    if ((position_state != PCU_POSITION_MOVING) &&
        (position_state != PCU_POSITION_HOLDING))
    {
        return;
    }

    if ((pcu_speed_is_active() == 0U) ||
        (pcu_speed_get_loop_mode() != PCU_LOOP_MODE_POSITION))
    {
        position_fail(PCU_POSITION_FAULT);
        return;
    }

    if (move_start_ms == 0U)
    {
        move_start_ms = now_ms;
    }

    if ((g_pcu_status.status & PCU_POSITION_FAULT_MASK) != 0U)
    {
        position_fail(PCU_POSITION_FAULT);
        return;
    }
    if ((!feedback_seen_during_move &&
         ((uint32_t)(now_ms - move_start_ms) > POSITION_FEEDBACK_TIMEOUT_MS)) ||
        (feedback_seen_during_move &&
         ((uint32_t)(now_ms - last_feedback_ms) > POSITION_FEEDBACK_TIMEOUT_MS)))
    {
        position_fail(PCU_POSITION_TIMEOUT);
        return;
    }
    if ((position_state == PCU_POSITION_MOVING) &&
        ((uint32_t)(now_ms - move_start_ms) > POSITION_MOVE_TIMEOUT_MS))
    {
        position_fail(PCU_POSITION_TIMEOUT);
        return;
    }
    if ((uint32_t)(now_ms - last_command_ms) < POSITION_LOOP_PERIOD_MS)
    {
        return;
    }
    last_command_ms = now_ms;

    error_deg = target_angle_deg - current_angle_deg;
    position_publish(error_deg);

    if ((position_state == PCU_POSITION_MOVING) &&
        (fabsf(error_deg) <= POSITION_TOLERANCE_DEG))
    {
        if (settle_start_ms == 0U)
        {
            settle_start_ms = now_ms;
        }
        if ((uint32_t)(now_ms - settle_start_ms) >= POSITION_SETTLE_TIME_MS)
        {
            if (position_end_mode == PCU_POSITION_END_DISABLE)
            {
                position_disable();
                position_state = PCU_POSITION_REACHED;
                pcu_position_speed_reference_rpm = 0.0f;
                return;
            }
            position_state = PCU_POSITION_HOLDING;
        }
    }
    else if (fabsf(error_deg) > POSITION_TOLERANCE_DEG)
    {
        settle_start_ms = 0U;
    }
    speed_reference_rpm = error_deg * position_kp_rpm_per_deg *
                          POSITION_DIRECTION;
    if (speed_reference_rpm > position_max_speed_rpm)
    {
        speed_reference_rpm = position_max_speed_rpm;
    }
    else if (speed_reference_rpm < -position_max_speed_rpm)
    {
        speed_reference_rpm = -position_max_speed_rpm;
    }
    pcu_position_speed_reference_rpm = speed_reference_rpm;
    if (pcu_speed_set_position_reference_rpm(speed_reference_rpm) == 0U)
    {
        position_fail(PCU_POSITION_FAULT);
    }
}

uint8_t pcu_position_set_kp(float value)
{
    if (!isfinite(value) || (value < 0.0f))
    {
        return 0U;
    }
    position_kp_rpm_per_deg = value;
    return 1U;
}

uint8_t pcu_position_set_end_mode(pcu_position_end_mode_t mode)
{
    if (((mode != PCU_POSITION_END_DISABLE) &&
         (mode != PCU_POSITION_END_HOLD)) ||
        (pcu_position_is_active() != 0U))
    {
        return 0U;
    }
    position_end_mode = mode;
    return 1U;
}

pcu_position_end_mode_t pcu_position_get_end_mode(void)
{
    return position_end_mode;
}

uint8_t pcu_position_set_max_speed(float value)
{
    if (!isfinite(value) || (value <= 0.0f))
    {
        return 0U;
    }
    position_max_speed_rpm = value;
    return 1U;
}

float pcu_position_get_kp(void)
{
    return position_kp_rpm_per_deg;
}

float pcu_position_get_max_speed(void)
{
    return position_max_speed_rpm;
}

pcu_position_state_t pcu_position_get_state(void)
{
    return position_state;
}

uint8_t pcu_position_is_active(void)
{
    return (uint8_t)((position_state == PCU_POSITION_MOVING) ||
                     (position_state == PCU_POSITION_HOLDING));
}
