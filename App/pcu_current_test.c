#include "pcu_current_test.h"

#include <math.h>

#include "pcu.h"
#include "pcu_position.h"
#include "pcu_speed.h"

#define PCU_CURRENT_TEST_LOOP_PERIOD_MS (1U)
#define PCU_CURRENT_TEST_TWO_PI         (6.28318530717958647692f)
#define PCU_CURRENT_TEST_RAW_MAX        (32767.0f)
#define PCU_CURRENT_TEST_RAW_MIN        (-32767.0f)
#define PCU_CURRENT_TEST_MAX_FREQUENCY_HZ (100.0f)
#define PCU_CURRENT_TEST_FAULT_MASK     (PCU_STATUS_ANGLE_FAULT | \
                                         PCU_STATUS_OVERCURRENT | \
                                         PCU_STATUS_OVERTEMPERATURE | \
                                         PCU_STATUS_MOTOR_OVERLOAD | \
                                         PCU_STATUS_BUS_UNDERVOLT | \
                                         PCU_STATUS_BUS_OVERVOLT)

volatile uint8_t pcu_current_test_enable;
volatile float pcu_current_test_offset_a;
volatile float pcu_current_test_amplitude_a;
volatile float pcu_current_test_frequency_hz;

volatile uint8_t pcu_current_test_active;
volatile float pcu_current_test_command_a;
volatile float pcu_current_test_phase_rad;
volatile int16_t pcu_current_test_raw;
volatile uint32_t pcu_current_test_updates;
volatile uint32_t pcu_current_test_command_errors;

static uint32_t last_update_ms;
static uint8_t schedule_initialized;

static float clamp_float(float value, float low, float high)
{
    if (value < low)
    {
        return low;
    }
    if (value > high)
    {
        return high;
    }
    return value;
}

static int16_t current_a_to_raw(float current_a, float max_current_a)
{
    float normalized;
    float raw;

    if (!isfinite(current_a) || !isfinite(max_current_a) ||
        (max_current_a <= 0.0f))
    {
        return 0;
    }

    normalized = current_a / max_current_a;
    raw = clamp_float(normalized * PCU_CURRENT_TEST_RAW_MAX,
                      PCU_CURRENT_TEST_RAW_MIN,
                      PCU_CURRENT_TEST_RAW_MAX);
    if (raw >= 0.0f)
    {
        raw += 0.5f;
    }
    else
    {
        raw -= 0.5f;
    }
    return (int16_t)raw;
}

static void current_test_disable_output(void)
{
    pcu_current_test_command_a = 0.0f;
    pcu_current_test_raw = 0;
    (void)pcu_disable_output();
}

void pcu_current_test_init(void)
{
    pcu_current_test_enable = 0U;
    pcu_current_test_offset_a = 0.0f;
    pcu_current_test_amplitude_a = 0.0f;
    pcu_current_test_frequency_hz = 0.0f;
    pcu_current_test_active = 0U;
    pcu_current_test_command_a = 0.0f;
    pcu_current_test_phase_rad = 0.0f;
    pcu_current_test_raw = 0;
    pcu_current_test_updates = 0U;
    pcu_current_test_command_errors = 0U;
    last_update_ms = 0U;
    schedule_initialized = 0U;
}

uint8_t pcu_current_test_is_enabled(void)
{
    return pcu_current_test_enable != 0U ? 1U : 0U;
}

uint8_t pcu_current_test_is_active(void)
{
    return pcu_current_test_active;
}

void pcu_current_test_poll(uint32_t now_ms)
{
    uint32_t dt_ms;
    float max_current_a;
    float offset_a;
    float amplitude_a;
    float frequency_hz;
    float command_a;
    int16_t command_raw;

    if (pcu_current_test_enable == 0U)
    {
        if (pcu_current_test_active != 0U)
        {
            current_test_disable_output();
            pcu_current_test_active = 0U;
        }
        schedule_initialized = 0U;
        return;
    }

    if (pcu_current_test_active == 0U)
    {
        /* This test mode owns the PCU output. Stop both closed-loop modes
           before the first current-test frame is sent. */
        pcu_speed_stop();
        pcu_position_stop();
        pcu_current_test_phase_rad = 0.0f;
        pcu_current_test_active = 1U;
        schedule_initialized = 0U;
        return;
    }

    if (schedule_initialized == 0U)
    {
        last_update_ms = now_ms;
        schedule_initialized = 1U;
        return;
    }

    dt_ms = now_ms - last_update_ms;
    if (dt_ms < PCU_CURRENT_TEST_LOOP_PERIOD_MS)
    {
        return;
    }
    last_update_ms = now_ms;

    max_current_a = pcu_speed_get_max_current();
    offset_a = pcu_current_test_offset_a;
    amplitude_a = pcu_current_test_amplitude_a;
    frequency_hz = pcu_current_test_frequency_hz;

    /* Debugger writes can occur at any time; invalid values must never reach
       the PCU command frame. */
    if (!isfinite(offset_a))
    {
        offset_a = 0.0f;
    }
    if (!isfinite(amplitude_a) || (amplitude_a < 0.0f))
    {
        amplitude_a = 0.0f;
    }
    if (!isfinite(frequency_hz) || (frequency_hz < 0.0f))
    {
        frequency_hz = 0.0f;
    }
    if (frequency_hz > PCU_CURRENT_TEST_MAX_FREQUENCY_HZ)
    {
        frequency_hz = PCU_CURRENT_TEST_MAX_FREQUENCY_HZ;
    }

    if ((g_pcu_status.data_valid == 0U) ||
        ((g_pcu_status.status & PCU_CURRENT_TEST_FAULT_MASK) != 0U))
    {
        current_test_disable_output();
        pcu_current_test_active = 0U;
        return;
    }

    command_a = offset_a + amplitude_a * sinf(pcu_current_test_phase_rad);
    if (isfinite(max_current_a) && (max_current_a > 0.0f))
    {
        command_a = clamp_float(command_a, -max_current_a, max_current_a);
    }
    else
    {
        command_a = 0.0f;
    }
    command_raw = current_a_to_raw(command_a, max_current_a);
    pcu_current_test_command_a = command_a;
    pcu_current_test_raw = command_raw;

    if (pcu_send_command(PCU_OUTPUT_ENABLED,
                         PCU_CONTROL_CURRENT_CLOSED_LOOP,
                         command_raw) == 0U)
    {
        pcu_current_test_command_errors++;
        current_test_disable_output();
        pcu_current_test_active = 0U;
        return;
    }

    pcu_current_test_updates++;
    if (frequency_hz > 0.0f)
    {
        pcu_current_test_phase_rad = fmodf(
            pcu_current_test_phase_rad +
            (PCU_CURRENT_TEST_TWO_PI * frequency_hz *
             ((float)dt_ms * 0.001f)),
            PCU_CURRENT_TEST_TWO_PI);
    }
    else
    {
        pcu_current_test_phase_rad = 0.0f;
    }
}
