#include "pcu_speed.h"

#include <math.h>

#include "gsa200.h"
#include "pcu.h"
#include "pcu_current_test.h"
#include "pcu_position.h"

#define PCU_SPEED_LOOP_PERIOD_MS       (1U)
#define PCU_SPEED_FEEDBACK_PERIOD_MS   (1U) /* Protocol default: 1 kHz. */
#define PCU_SPEED_FEEDBACK_TIMEOUT_MS  (5U)
#define PCU_SPEED_ANGLE_COUNTS         (16777216.0f)
#define PCU_SPEED_RAW_CURRENT_MAX      (32767.0f)
#define PCU_SPEED_TWO_PI               (6.28318530717958647692f)
#define PCU_SPEED_MIN_SINE_FREQUENCY_HZ (0.01f)
#define PCU_SPEED_MAX_SINE_FREQUENCY_HZ (50.0f)
#define PCU_SPEED_MAX_REFERENCE_RPM    (100000.0f)

/* These are starting values only. Tune them with the connected motor. */
#define PCU_SPEED_DEFAULT_MAX_CURRENT_A (2.0f)
#define PCU_SPEED_DEFAULT_KP_A_PER_RPM  (0.001f)
#define PCU_SPEED_DEFAULT_KI_A_PER_RPM_S (0.01f)

#define PCU_SPEED_FAULT_MASK (PCU_STATUS_ANGLE_FAULT | PCU_STATUS_OVERCURRENT | \
                              PCU_STATUS_OVERTEMPERATURE | PCU_STATUS_MOTOR_OVERLOAD | \
                              PCU_STATUS_BUS_UNDERVOLT | PCU_STATUS_BUS_OVERVOLT)

volatile float pcu_speed_feedback_rpm;
volatile float pcu_speed_pcu_differential_rpm;
volatile float pcu_speed_gyro_rpm;
volatile uint32_t pcu_speed_feedback_age_ms;
volatile float pcu_speed_error_rpm;
volatile float pcu_speed_reference_rpm;
volatile float pcu_speed_current_a;
volatile float pcu_speed_output_norm;
volatile int16_t pcu_speed_current_raw;
volatile uint32_t pcu_speed_control_updates;
volatile uint32_t pcu_speed_feedback_timeouts;
volatile uint32_t pcu_speed_command_errors;

static pcu_speed_state_t speed_state;
static float target_rpm;
static float kp_a_per_rpm;
static float ki_a_per_rpm_s;
static float max_current_a;
static float sine_base_rpm;
static float sine_amplitude_rpm;
static float sine_frequency_hz;
static float sine_phase_rad;
static float position_reference_rpm;
static float integral_a;
static uint32_t last_pcu_frame_count;
static uint32_t last_gyro_frame_count;
static uint32_t last_angle_raw;
static uint32_t last_pcu_feedback_ms;
static uint32_t last_gyro_feedback_ms;
static uint32_t feedback_wait_start_ms;
static uint32_t last_control_ms;
static uint8_t pcu_feedback_initialized;
static uint8_t gyro_feedback_initialized;
static uint8_t schedule_initialized;
static uint8_t sine_active;
static pcu_loop_mode_t loop_mode;
static pcu_speed_feedback_source_t feedback_source;

static float clamp_float(float value, float low, float high)
{
    if (value > high)
    {
        return high;
    }
    if (value < low)
    {
        return low;
    }
    return value;
}

static int32_t signed_angle_delta(uint32_t current, uint32_t previous)
{
    uint32_t delta = (current - previous) & PCU_ANGLE_RAW_MAX;

    if (delta > (PCU_ANGLE_RAW_MAX / 2U))
    {
        return (int32_t)delta - (int32_t)(PCU_ANGLE_RAW_MAX + 1U);
    }
    return (int32_t)delta;
}

static int16_t normalized_to_raw(float output_norm)
{
    float scaled = clamp_float(output_norm, -1.0f, 1.0f) * PCU_SPEED_RAW_CURRENT_MAX;
    long rounded = lroundf(scaled);

    if (rounded > 32767L)
    {
        rounded = 32767L;
    }
    else if (rounded < -32767L)
    {
        rounded = -32767L;
    }
    return (int16_t)rounded;
}

static void speed_reset_measurement(void)
{
    last_pcu_frame_count = g_pcu_status.valid_frames;
    last_gyro_frame_count = gsa200_valid_frames;
    last_angle_raw = g_pcu_status.angle_raw;
    last_pcu_feedback_ms = 0U;
    last_gyro_feedback_ms = 0U;
    feedback_wait_start_ms = 0U;
    pcu_feedback_initialized = 0U;
    gyro_feedback_initialized = 0U;
    pcu_speed_feedback_rpm = 0.0f;
    pcu_speed_pcu_differential_rpm = 0.0f;
    pcu_speed_gyro_rpm = 0.0f;
    pcu_speed_feedback_age_ms = 0U;
    pcu_speed_error_rpm = 0.0f;
}

static void speed_disable_output(void)
{
    pcu_speed_current_a = 0.0f;
    pcu_speed_output_norm = 0.0f;
    pcu_speed_current_raw = 0;
    (void)pcu_disable_output();
}

static void speed_fail(pcu_speed_state_t state)
{
    integral_a = 0.0f;
    speed_disable_output();
    speed_state = state;
}

static void speed_update_pcu_feedback(uint32_t now_ms)
{
    uint32_t frame_count = g_pcu_status.valid_frames;
    uint32_t frame_delta;
    uint32_t angle_raw;
    uint32_t dt_ms;
    int32_t delta_raw;

    if (frame_count == last_pcu_frame_count)
    {
        return;
    }

    angle_raw = g_pcu_status.angle_raw;
    frame_delta = frame_count - last_pcu_frame_count;
    dt_ms = pcu_feedback_initialized != 0U ?
            (now_ms - last_pcu_feedback_ms) : PCU_SPEED_FEEDBACK_PERIOD_MS;
    if (dt_ms == 0U)
    {
        dt_ms = PCU_SPEED_FEEDBACK_PERIOD_MS;
    }
    if ((frame_delta > 1U) && (dt_ms < (frame_delta * PCU_SPEED_FEEDBACK_PERIOD_MS)))
    {
        /* Several frames can be copied by one DMA/idle event. Account for
           their configured 1 ms spacing instead of collapsing them to one ms. */
        dt_ms = frame_delta * PCU_SPEED_FEEDBACK_PERIOD_MS;
    }

    last_pcu_frame_count = frame_count;
    last_pcu_feedback_ms = now_ms;
    if (pcu_feedback_initialized == 0U)
    {
        last_angle_raw = angle_raw;
        pcu_feedback_initialized = 1U;
        return;
    }

    delta_raw = signed_angle_delta(angle_raw, last_angle_raw);
    last_angle_raw = angle_raw;
    pcu_speed_pcu_differential_rpm = ((float)delta_raw * 60000.0f) /
                                     (PCU_SPEED_ANGLE_COUNTS * (float)dt_ms);
}

static void speed_update_gyro_feedback(uint32_t now_ms)
{
    uint32_t frame_count = gsa200_valid_frames;
    float gyro_rpm;

    if (frame_count == last_gyro_frame_count)
    {
        return;
    }

    last_gyro_frame_count = frame_count;
    if ((gsa200_data_valid == 0U) ||
        (gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE) ||
        !isfinite(gsa200_gyro_x_corrected_dps))
    {
        return;
    }

    gyro_rpm = gsa200_gyro_x_corrected_dps / 6.0f;
    if (!isfinite(gyro_rpm))
    {
        return;
    }

    pcu_speed_gyro_rpm = gyro_rpm;
    last_gyro_feedback_ms = now_ms;
    gyro_feedback_initialized = 1U;
}

static uint8_t speed_select_feedback(uint32_t now_ms)
{
    if (feedback_source == PCU_SPEED_FEEDBACK_GYRO)
    {
        if (gyro_feedback_initialized == 0U)
        {
            return 0U;
        }
        pcu_speed_feedback_rpm = pcu_speed_gyro_rpm;
        pcu_speed_feedback_age_ms = now_ms - last_gyro_feedback_ms;
    }
    else
    {
        if (pcu_feedback_initialized == 0U)
        {
            return 0U;
        }
        pcu_speed_feedback_rpm = pcu_speed_pcu_differential_rpm;
        pcu_speed_feedback_age_ms = now_ms - last_pcu_feedback_ms;
    }

    return (uint8_t)isfinite(pcu_speed_feedback_rpm);
}

static void speed_run_pi(uint32_t dt_ms)
{
    float dt_s = (float)dt_ms * 0.001f;
    float reference_rpm;
    float error;
    float candidate_integral;
    float unsaturated;
    float max_a;
    float output_a;
    uint8_t saturated;

    if (loop_mode == PCU_LOOP_MODE_POSITION)
    {
        reference_rpm = position_reference_rpm;
    }
    else if (sine_active != 0U)
    {
        reference_rpm = sine_base_rpm +
                        sine_amplitude_rpm * sinf(sine_phase_rad);
        sine_phase_rad = fmodf(sine_phase_rad +
                               (PCU_SPEED_TWO_PI * sine_frequency_hz * dt_s),
                               PCU_SPEED_TWO_PI);
    }
    else
    {
        reference_rpm = target_rpm;
    }
    pcu_speed_reference_rpm = reference_rpm;
    error = reference_rpm - pcu_speed_feedback_rpm;
    candidate_integral = integral_a + ki_a_per_rpm_s * error * dt_s;
    unsaturated = kp_a_per_rpm * error + candidate_integral;
    max_a = max_current_a;
    output_a = clamp_float(unsaturated, -max_a, max_a);
    // saturated = (uint8_t)(output_a != unsaturated);

    // /* Conditional integration prevents windup while allowing the integral to
    //    unwind when the speed error points back into the available range. */
    // if ((saturated == 0U) ||
    //     ((unsaturated > max_a) && (error < 0.0f)) ||
    //     ((unsaturated < -max_a) && (error > 0.0f)))
    // {
    //     integral_a = clamp_float(candidate_integral, -max_a, max_a);
    //     unsaturated = kp_a_per_rpm * error + integral_a;
    //     output_a = clamp_float(unsaturated, -max_a, max_a);
    // }

    pcu_speed_error_rpm = error;
    pcu_speed_current_a = output_a;
    pcu_speed_output_norm = clamp_float(output_a / max_a, -1.0f, 1.0f);
    pcu_speed_current_raw = normalized_to_raw(pcu_speed_output_norm);
}

void pcu_speed_init(void)
{
    speed_state = PCU_SPEED_IDLE;
    target_rpm = 0.0f;
    sine_base_rpm = 0.0f;
    sine_amplitude_rpm = 0.0f;
    sine_frequency_hz = 1.0f;
    sine_phase_rad = 0.0f;
    position_reference_rpm = 0.0f;
    sine_active = 0U;
    loop_mode = PCU_LOOP_MODE_NONE;
    feedback_source = PCU_SPEED_FEEDBACK_PCU_DIFFERENTIAL;
    kp_a_per_rpm = PCU_SPEED_DEFAULT_KP_A_PER_RPM;
    ki_a_per_rpm_s = PCU_SPEED_DEFAULT_KI_A_PER_RPM_S;
    max_current_a = PCU_SPEED_DEFAULT_MAX_CURRENT_A;
    integral_a = 0.0f;
    schedule_initialized = 0U;
    speed_reset_measurement();
    pcu_speed_reference_rpm = 0.0f;
    pcu_speed_current_a = 0.0f;
    pcu_speed_output_norm = 0.0f;
    pcu_speed_current_raw = 0;
    pcu_speed_control_updates = 0U;
    pcu_speed_feedback_timeouts = 0U;
    pcu_speed_command_errors = 0U;
}

static uint8_t speed_start_internal(uint8_t sine_mode, pcu_loop_mode_t mode)
{
    if (pcu_current_test_is_enabled() != 0U)
    {
        return 0U;
    }

    if ((g_pcu_status.data_valid == 0U) ||
        ((g_pcu_status.status & PCU_SPEED_FAULT_MASK) != 0U) ||
        (max_current_a <= 0.0f) || !isfinite(max_current_a))
    {
        return 0U;
    }

    if ((feedback_source == PCU_SPEED_FEEDBACK_GYRO) &&
        ((gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE) ||
         (gsa200_data_valid == 0U) ||
         !isfinite(gsa200_gyro_x_corrected_dps)))
    {
        return 0U;
    }

    /* Speed and position loops are mutually exclusive. */
    if ((mode == PCU_LOOP_MODE_SPEED) &&
        (pcu_position_is_active() != 0U))
    {
        return 0U;
    }

    integral_a = 0.0f;
    speed_reset_measurement();
    schedule_initialized = 0U;
    sine_active = sine_mode;
    loop_mode = mode;
    sine_phase_rad = 0.0f;
    position_reference_rpm = 0.0f;
    pcu_speed_reference_rpm = mode == PCU_LOOP_MODE_POSITION ?
                              0.0f :
                              (sine_mode != 0U ? sine_base_rpm : target_rpm);
    speed_state = PCU_SPEED_RUNNING;
    return 1U;
}

uint8_t pcu_speed_start(void)
{
    return speed_start_internal(0U, PCU_LOOP_MODE_SPEED);
}

uint8_t pcu_speed_start_position(void)
{
    return speed_start_internal(0U, PCU_LOOP_MODE_POSITION);
}

uint8_t pcu_speed_sine_start(void)
{
    return speed_start_internal(1U, PCU_LOOP_MODE_SPEED);
}

void pcu_speed_stop(void)
{
    if (speed_state == PCU_SPEED_RUNNING)
    {
        speed_disable_output();
    }
    else
    {
        /* Even when the local state is already idle, force the drive output
           disabled. The drive may have retained an enabled state across a
           controller reset. */
        (void)pcu_disable_output();
    }
    integral_a = 0.0f;
    schedule_initialized = 0U;
    sine_active = 0U;
    loop_mode = PCU_LOOP_MODE_NONE;
    position_reference_rpm = 0.0f;
    sine_phase_rad = 0.0f;
    pcu_speed_reference_rpm = 0.0f;
    speed_state = PCU_SPEED_IDLE;
}

uint8_t pcu_speed_set_sine_params(float base_rpm, float amplitude_rpm,
                                  float frequency_hz)
{
    if (!isfinite(base_rpm) || !isfinite(amplitude_rpm) ||
        !isfinite(frequency_hz) || (amplitude_rpm < 0.0f) ||
        (frequency_hz < PCU_SPEED_MIN_SINE_FREQUENCY_HZ) ||
        (frequency_hz > PCU_SPEED_MAX_SINE_FREQUENCY_HZ) ||
        ((base_rpm - amplitude_rpm) < -PCU_SPEED_MAX_REFERENCE_RPM) ||
        ((base_rpm + amplitude_rpm) > PCU_SPEED_MAX_REFERENCE_RPM))
    {
        return 0U;
    }

    sine_base_rpm = base_rpm;
    sine_amplitude_rpm = amplitude_rpm;
    sine_frequency_hz = frequency_hz;
    return 1U;
}

void pcu_speed_sine_stop(void)
{
    pcu_speed_stop();
}

void pcu_speed_poll(uint32_t now_ms)
{
    uint32_t dt_ms;
    uint8_t command_ok;

    if (speed_state != PCU_SPEED_RUNNING)
    {
        return;
    }
    if (schedule_initialized == 0U)
    {
        last_control_ms = now_ms;
        feedback_wait_start_ms = now_ms;
        schedule_initialized = 1U;
        return;
    }
    dt_ms = now_ms - last_control_ms;
    if (dt_ms < PCU_SPEED_LOOP_PERIOD_MS)
    {
        return;
    }
    last_control_ms = now_ms;

    speed_update_pcu_feedback(now_ms);
    speed_update_gyro_feedback(now_ms);

    if ((feedback_source == PCU_SPEED_FEEDBACK_GYRO) &&
        ((gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE) ||
         (gsa200_data_valid == 0U)))
    {
        pcu_speed_feedback_timeouts++;
        speed_fail(PCU_SPEED_TIMEOUT);
        return;
    }

    if (speed_select_feedback(now_ms) == 0U)
    {
        if ((uint32_t)(now_ms - feedback_wait_start_ms) >=
            PCU_SPEED_FEEDBACK_TIMEOUT_MS)
        {
            pcu_speed_feedback_timeouts++;
            speed_fail(PCU_SPEED_TIMEOUT);
        }
        return;
    }

    if (pcu_speed_feedback_age_ms >= PCU_SPEED_FEEDBACK_TIMEOUT_MS)
    {
        pcu_speed_feedback_timeouts++;
        speed_fail(PCU_SPEED_TIMEOUT);
        return;
    }
    if ((g_pcu_status.status & PCU_SPEED_FAULT_MASK) != 0U)
    {
        speed_fail(PCU_SPEED_FAULT);
        return;
    }

    speed_run_pi(dt_ms);
    command_ok = pcu_send_command(PCU_OUTPUT_ENABLED,
                                  PCU_CONTROL_CURRENT_CLOSED_LOOP,
                                  pcu_speed_current_raw);
    if (command_ok == 0U)
    {
        pcu_speed_command_errors++;
        speed_fail(PCU_SPEED_FAULT);
        return;
    }
    pcu_speed_control_updates++;
}

uint8_t pcu_speed_set_target_rpm(float value)
{
    if (!isfinite(value))
    {
        return 0U;
    }
    target_rpm = value;
    return 1U;
}

uint8_t pcu_speed_set_position_reference_rpm(float value)
{
    if (!isfinite(value) || (value < -PCU_SPEED_MAX_REFERENCE_RPM) ||
        (value > PCU_SPEED_MAX_REFERENCE_RPM))
    {
        return 0U;
    }
    position_reference_rpm = value;
    return 1U;
}

float pcu_speed_get_target_rpm(void)
{
    return target_rpm;
}

uint8_t pcu_speed_set_kp(float value)
{
    if (!isfinite(value) || (value < 0.0f))
    {
        return 0U;
    }
    kp_a_per_rpm = value;
    return 1U;
}

uint8_t pcu_speed_set_ki(float value)
{
    if (!isfinite(value) || (value < 0.0f))
    {
        return 0U;
    }
    ki_a_per_rpm_s = value;
    return 1U;
}

uint8_t pcu_speed_set_max_current(float value)
{
    if (!isfinite(value) || (value <= 0.0f))
    {
        return 0U;
    }
    max_current_a = value;
    integral_a = clamp_float(integral_a, -max_current_a, max_current_a);
    return 1U;
}

uint8_t pcu_speed_set_feedback_source(pcu_speed_feedback_source_t source)
{
    if (((source != PCU_SPEED_FEEDBACK_PCU_DIFFERENTIAL) &&
         (source != PCU_SPEED_FEEDBACK_GYRO)) ||
        (pcu_speed_is_active() != 0U) ||
        (pcu_position_is_active() != 0U) ||
        ((g_pcu_status.status & PCU_STATUS_ENABLED) != 0U))
    {
        return 0U;
    }

    feedback_source = source;
    integral_a = 0.0f;
    schedule_initialized = 0U;
    speed_reset_measurement();
    return 1U;
}

float pcu_speed_get_kp(void)
{
    return kp_a_per_rpm;
}

float pcu_speed_get_ki(void)
{
    return ki_a_per_rpm_s;
}

float pcu_speed_get_max_current(void)
{
    return max_current_a;
}

pcu_speed_feedback_source_t pcu_speed_get_feedback_source(void)
{
    return feedback_source;
}

pcu_speed_state_t pcu_speed_get_state(void)
{
    return speed_state;
}

pcu_loop_mode_t pcu_speed_get_loop_mode(void)
{
    return loop_mode;
}

uint8_t pcu_speed_is_active(void)
{
    return (uint8_t)(speed_state == PCU_SPEED_RUNNING);
}

uint8_t pcu_speed_sine_is_active(void)
{
    return (uint8_t)((speed_state == PCU_SPEED_RUNNING) &&
                     (sine_active != 0U));
}

float pcu_speed_get_sine_base_rpm(void)
{
    return sine_base_rpm;
}

float pcu_speed_get_sine_amplitude_rpm(void)
{
    return sine_amplitude_rpm;
}

float pcu_speed_get_sine_frequency_hz(void)
{
    return sine_frequency_hz;
}

float pcu_speed_get_reference_rpm(void)
{
    return pcu_speed_reference_rpm;
}
