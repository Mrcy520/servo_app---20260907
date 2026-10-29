#include "pcu_terminal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_uart.h"
#include "gsa200.h"
#include "pcu.h"
#include "pcu_current_test.h"
#include "pcu_position.h"
#include "pcu_speed.h"
#include "stm32h7xx_ll_cortex.h"

#define TERMINAL_LINE_SIZE (64U)
#define PCU_FAULT_MASK (PCU_STATUS_ANGLE_FAULT | PCU_STATUS_OVERCURRENT | \
                        PCU_STATUS_OVERTEMPERATURE | PCU_STATUS_MOTOR_OVERLOAD | \
                        PCU_STATUS_BUS_UNDERVOLT | PCU_STATUS_BUS_OVERVOLT)

volatile uint32_t pcu_terminal_command_count;
volatile uint32_t pcu_terminal_error_count;
volatile int32_t pcu_terminal_setpoint;
volatile uint32_t pcu_terminal_mode;
volatile uint32_t pcu_terminal_output_requested;

static char terminal_line[TERMINAL_LINE_SIZE];
static uint8_t terminal_line_length;

static void terminal_prompt(void)
{
    printf("pcu> ");
}

static void terminal_help(void)
{
    printf("\r\nPCU commands (raw setpoint -32767..32767):\r\n");
    printf("  help                 show this list\r\n");
    printf("  status               show PCU feedback and staged command\r\n");
    printf("  set voltage <raw>    stage voltage open-loop command\r\n");
    printf("  set current <raw>    stage current closed-loop command\r\n");
    printf("  enable               send the staged command and enable output\r\n");
    printf("  disable | stop       immediately disable PCU output\r\n");
    printf("  clear                clear PCU faults with output disabled\r\n");
    printf("  zero <raw>           start PCU automatic zeroing\r\n");
    printf("  gyro bias start      measure X-axis gyro bias while stopped\r\n");
    printf("  gyro status          show gyro and bias-calibration state\r\n");
    printf("  move <deg>           move relative to current angle\r\n");
    printf("  position status      show position-loop state\r\n");
    printf("  position end hold|disable  set action after reaching target\r\n");
    printf("  position kp <rpm/deg> set position proportional gain\r\n");
    printf("  position max_speed <rpm> set position speed limit\r\n");
    printf("  position stop        stop position move and disable output\r\n");
    printf("  speed set <rpm>      set speed-loop target\r\n");
    printf("  speed feedback pcu|gyro  select speed feedback while stopped\r\n");
    printf("  speed feedback status    show selected speed feedback\r\n");
    printf("  speed start|stop     start or stop speed loop\r\n");
    printf("  speed sine set <base> <amplitude> <Hz>\r\n");
    printf("  speed sine start|stop start or stop sine speed loop\r\n");
    printf("  speed sine status     show sine parameters\r\n");
    printf("  speed kp <A/rpm>     set proportional gain\r\n");
    printf("  speed ki <A/rpm/s>   set integral gain\r\n");
    printf("  speed max_current <A> set maximum current\r\n");
    printf("  speed status         show speed-loop state\r\n");
}

static uint32_t terminal_angle_mdeg(void)
{
    return (uint32_t)(((uint64_t)g_pcu_status.angle_raw * 360000ULL) /
                      16777216ULL);
}

static const char *terminal_gyro_bias_state(void)
{
    switch (gsa200_bias_calibration_state)
    {
    case GSA200_BIAS_CALIBRATING:
        return "calibrating";
    case GSA200_BIAS_COMPLETE:
        return "complete";
    default:
        return "not-started";
    }
}

static void terminal_gyro_status(void)
{
    printf("gyro: valid=%u frames=%lu raw_x=%.6f dps corrected_x=%.6f dps\r\n",
           (unsigned int)gsa200_data_valid,
           (unsigned long)gsa200_valid_frames,
           (double)gsa200_gyro_x_dps,
           (double)gsa200_gyro_x_corrected_dps);
    printf("gyro bias: state=%s samples=%lu/%lu bias_x=%.6f dps\r\n",
           terminal_gyro_bias_state(),
           (unsigned long)gsa200_bias_calibration_samples,
           (unsigned long)GSA200_BIAS_CALIBRATION_SAMPLES,
           (double)gsa200_gyro_x_bias_dps);
    printf("gyro rx: checksum=%lu format=%lu dropped=%lu uart_overflow=%lu\r\n",
           (unsigned long)gsa200_checksum_errors,
           (unsigned long)gsa200_format_errors,
           (unsigned long)gsa200_dropped_frames,
           (unsigned long)uart2_rx_overflow_count);
}

static void terminal_status(void)
{
    uint32_t status = g_pcu_status.status;
    uint8_t tx_frame[PCU_FRAME_SIZE];
    int16_t tx_setpoint;

    memcpy(tx_frame, (const void *)g_pcu_status.last_tx_frame, PCU_FRAME_SIZE);
    tx_setpoint = pcu_setpoint_get((const pcu_command_frame_t *)tx_frame);

    printf("\r\nPCU valid=%u frames=%lu angle=%lu.%03lu deg status=0x%02lX\r\n",
           (unsigned int)g_pcu_status.data_valid,
           (unsigned long)g_pcu_status.valid_frames,
           (unsigned long)(terminal_angle_mdeg() / 1000U),
           (unsigned long)(terminal_angle_mdeg() % 1000U),
           (unsigned long)status);
    printf("faults: angle=%u overcurrent=%u overtemp=%u overload=%u undervolt=%u overvolt=%u enabled=%u\r\n",
           (unsigned int)((status & PCU_STATUS_ANGLE_FAULT) != 0U),
           (unsigned int)((status & PCU_STATUS_OVERCURRENT) != 0U),
           (unsigned int)((status & PCU_STATUS_OVERTEMPERATURE) != 0U),
           (unsigned int)((status & PCU_STATUS_MOTOR_OVERLOAD) != 0U),
           (unsigned int)((status & PCU_STATUS_BUS_UNDERVOLT) != 0U),
           (unsigned int)((status & PCU_STATUS_BUS_OVERVOLT) != 0U),
           (unsigned int)((status & PCU_STATUS_ENABLED) != 0U));
    printf("rx errors: checksum=%lu format=%lu; staged mode=%lu setpoint=%ld requested=%lu\r\n",
           (unsigned long)g_pcu_status.checksum_errors,
           (unsigned long)g_pcu_status.format_errors,
           (unsigned long)pcu_terminal_mode,
           (long)pcu_terminal_setpoint,
           (unsigned long)pcu_terminal_output_requested);
    printf("tx: count=%lu errors=%lu frame=%02X %02X %02X %02X %02X %02X %02X raw=%d checksum=%s\r\n",
           (unsigned long)g_pcu_status.tx_frames,
           (unsigned long)g_pcu_status.tx_errors,
           (unsigned int)tx_frame[0], (unsigned int)tx_frame[1],
           (unsigned int)tx_frame[2], (unsigned int)tx_frame[3],
           (unsigned int)tx_frame[4], (unsigned int)tx_frame[5],
           (unsigned int)tx_frame[6], (int)tx_setpoint,
           pcu_checksum_valid(tx_frame) != 0U ? "OK" : "BAD");
    printf("position: state=%u end=%s current_mdeg=%ld target_mdeg=%ld error_mdeg=%ld\r\n",
           (unsigned int)pcu_position_get_state(),
           pcu_position_get_end_mode() == PCU_POSITION_END_HOLD ?
           "hold" : "disable",
           (long)pcu_position_current_mdeg,
           (long)pcu_position_target_mdeg,
           (long)pcu_position_error_mdeg);
    printf("position p: kp=%.6f rpm/deg max_speed=%.3f rpm output=%.3f rpm\r\n",
           (double)pcu_position_get_kp(),
           (double)pcu_position_get_max_speed(),
           (double)pcu_position_speed_reference_rpm);
    printf("control: loop_mode=%s\r\n",
           pcu_speed_get_loop_mode() == PCU_LOOP_MODE_SPEED ? "speed" :
           (pcu_speed_get_loop_mode() == PCU_LOOP_MODE_POSITION ?
            "position" : "none"));
    printf("speed: state=%u waveform=%s target=%.3f rpm reference=%.3f rpm feedback=%.3f rpm error=%.3f rpm\r\n",
           (unsigned int)pcu_speed_get_state(),
           pcu_speed_sine_is_active() != 0U ? "sine" : "constant",
           (double)pcu_speed_get_target_rpm(),
           (double)pcu_speed_get_reference_rpm(),
           (double)pcu_speed_feedback_rpm,
           (double)pcu_speed_error_rpm);
    printf("speed feedback: source=%s pcu_diff=%.3f rpm gyro=%.3f rpm pi_input=%.3f rpm age=%lu ms\r\n",
           pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO ?
           "gyro" : "pcu",
           (double)pcu_speed_pcu_differential_rpm,
           (double)pcu_speed_gyro_rpm,
           (double)pcu_speed_feedback_rpm,
           (unsigned long)pcu_speed_feedback_age_ms);
    printf("speed sine: active=%u base=%.3f rpm amplitude=%.3f rpm frequency=%.3f Hz\r\n",
           (unsigned int)pcu_speed_sine_is_active(),
           (double)pcu_speed_get_sine_base_rpm(),
           (double)pcu_speed_get_sine_amplitude_rpm(),
           (double)pcu_speed_get_sine_frequency_hz());
    printf("speed pi: kp=%.6f ki=%.6f max_current=%.3f A current=%.3f A norm=%.3f raw=%d\r\n",
           (double)pcu_speed_get_kp(),
           (double)pcu_speed_get_ki(),
           (double)pcu_speed_get_max_current(),
           (double)pcu_speed_current_a,
           (double)pcu_speed_output_norm,
           (int)pcu_speed_current_raw);
    printf("speed counters: updates=%lu feedback_timeouts=%lu command_errors=%lu\r\n",
           (unsigned long)pcu_speed_control_updates,
           (unsigned long)pcu_speed_feedback_timeouts,
           (unsigned long)pcu_speed_command_errors);
    terminal_gyro_status();
}

static uint8_t terminal_parse_setpoint(const char *text, int16_t *value)
{
    char *end;
    long parsed;

    while (*text == ' ')
    {
        ++text;
    }
    if (*text == '\0')
    {
        return 0U;
    }

    parsed = strtol(text, &end, 10);
    while (*end == ' ')
    {
        ++end;
    }
    if ((*end != '\0') || (parsed < PCU_SETPOINT_MIN) ||
        (parsed > PCU_SETPOINT_MAX))
    {
        return 0U;
    }

    *value = (int16_t)parsed;
    return 1U;
}

static uint8_t terminal_parse_angle(const char *text, float *value)
{
    char *end;
    float parsed;

    while (*text == ' ')
    {
        ++text;
    }
    if (*text == '\0')
    {
        return 0U;
    }

    parsed = strtof(text, &end);
    while (*end == ' ')
    {
        ++end;
    }
    if ((*end != '\0') || !isfinite(parsed) || (parsed < -3600.0f) ||
        (parsed > 3600.0f))
    {
        return 0U;
    }

    *value = parsed;
    return 1U;
}

static uint8_t terminal_parse_float(const char *text, float minimum,
                                    float maximum, float *value)
{
    char *end;
    float parsed;

    while (*text == ' ')
    {
        ++text;
    }
    if (*text == '\0')
    {
        return 0U;
    }

    parsed = strtof(text, &end);
    while (*end == ' ')
    {
        ++end;
    }
    if ((*end != '\0') || !isfinite(parsed) ||
        (parsed < minimum) || (parsed > maximum))
    {
        return 0U;
    }

    *value = parsed;
    return 1U;
}

static uint8_t terminal_parse_sine_params(const char *text, float *base_rpm,
                                          float *amplitude_rpm,
                                          float *frequency_hz)
{
    char *end;
    float values[3];
    uint8_t index;

    for (index = 0U; index < 3U; ++index)
    {
        while (*text == ' ')
        {
            ++text;
        }
        if (*text == '\0')
        {
            return 0U;
        }

        values[index] = strtof(text, &end);
        if ((end == text) || !isfinite(values[index]))
        {
            return 0U;
        }
        text = end;
    }

    while (*text == ' ')
    {
        ++text;
    }
    if (*text != '\0')
    {
        return 0U;
    }

    *base_rpm = values[0];
    *amplitude_rpm = values[1];
    *frequency_hz = values[2];
    return 1U;
}

static uint8_t terminal_drive_ready(void)
{
    if (g_pcu_status.data_valid == 0U)
    {
        printf("ERR: no valid PCU feedback; output remains disabled\r\n");
        return 0U;
    }
    if ((g_pcu_status.status & PCU_FAULT_MASK) != 0U)
    {
        printf("ERR: PCU fault status=0x%02X; output remains disabled\r\n",
               (unsigned int)g_pcu_status.status);
        return 0U;
    }
    return 1U;
}

static void terminal_execute(char *line)
{
    int16_t value;
    float angle;
    float float_value;
    float sine_base_rpm;
    float sine_amplitude_rpm;
    float sine_frequency_hz;
    uint8_t sent = 0U;
    char *cursor;

    for (cursor = line; *cursor != '\0'; ++cursor)
    {
        *cursor = (char)tolower((unsigned char)*cursor);
    }

    if ((strcmp(line, "help") == 0) || (strcmp(line, "?") == 0))
    {
        terminal_help();
    }
    else if (strcmp(line, "status") == 0)
    {
        terminal_status();
    }
    else if (strcmp(line, "gyro status") == 0)
    {
        terminal_gyro_status();
    }
    else if (strcmp(line, "gyro bias start") == 0)
    {
        if ((pcu_speed_is_active() != 0U) ||
            (pcu_position_is_active() != 0U) ||
            ((g_pcu_status.status & PCU_STATUS_ENABLED) != 0U))
        {
            printf("ERR: stop and disable the motor before gyro bias calibration\r\n");
        }
        else if (gsa200_data_valid == 0U)
        {
            printf("ERR: no valid GSA200 data; calibration not started\r\n");
        }
        else
        {
            gsa200_start_bias_calibration();
            printf("OK: gyro X-axis bias calibration started; keep motor still\r\n");
        }
    }
    else if (strcmp(line, "position status") == 0)
    {
        terminal_status();
    }
    else if (strcmp(line, "position end hold") == 0)
    {
        if (pcu_position_set_end_mode(PCU_POSITION_END_HOLD) != 0U)
        {
            printf("OK: position end mode=hold\r\n");
        }
        else
        {
            printf("ERR: stop position control before changing end mode\r\n");
        }
    }
    else if (strcmp(line, "position end disable") == 0)
    {
        if (pcu_position_set_end_mode(PCU_POSITION_END_DISABLE) != 0U)
        {
            printf("OK: position end mode=disable\r\n");
        }
        else
        {
            printf("ERR: stop position control before changing end mode\r\n");
        }
    }
    else if (strncmp(line, "position kp ", 12U) == 0)
    {
        if (terminal_parse_float(&line[12], 0.0f, 100000.0f,
                                 &float_value) != 0U &&
            pcu_position_set_kp(float_value) != 0U)
        {
            printf("OK: position kp=%.6f rpm/deg\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: position kp must be >= 0\r\n");
        }
    }
    else if (strncmp(line, "position max_speed ", 19U) == 0)
    {
        if (terminal_parse_float(&line[19], 0.001f, 100000.0f,
                                 &float_value) != 0U &&
            pcu_position_set_max_speed(float_value) != 0U)
        {
            printf("OK: position max speed=%.3f rpm\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: position max speed must be > 0 rpm\r\n");
        }
    }
    else if (strcmp(line, "position stop") == 0)
    {
        pcu_position_stop();
        pcu_terminal_output_requested = 0U;
        printf("OK: position move stopped\r\n");
    }
    else if (strcmp(line, "speed status") == 0)
    {
        terminal_status();
    }
    else if (strcmp(line, "speed feedback status") == 0)
    {
        printf("speed feedback: source=%s pcu_diff=%.3f rpm gyro=%.3f rpm pi_input=%.3f rpm age=%lu ms\r\n",
               pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO ?
               "gyro" : "pcu",
               (double)pcu_speed_pcu_differential_rpm,
               (double)pcu_speed_gyro_rpm,
               (double)pcu_speed_feedback_rpm,
               (unsigned long)pcu_speed_feedback_age_ms);
    }
    else if (strcmp(line, "speed feedback pcu") == 0)
    {
        if (pcu_speed_set_feedback_source(
                PCU_SPEED_FEEDBACK_PCU_DIFFERENTIAL) != 0U)
        {
            printf("OK: speed feedback source=pcu differential\r\n");
        }
        else
        {
            printf("ERR: stop all control and disable motor before changing feedback source\r\n");
        }
    }
    else if (strcmp(line, "speed feedback gyro") == 0)
    {
        if (pcu_speed_set_feedback_source(PCU_SPEED_FEEDBACK_GYRO) != 0U)
        {
            printf("OK: speed feedback source=gyro; calibration is required before start\r\n");
        }
        else
        {
            printf("ERR: stop all control and disable motor before changing feedback source\r\n");
        }
    }
    else if (strcmp(line, "speed sine status") == 0)
    {
        terminal_status();
    }
    else if (strncmp(line, "speed sine set ", 15U) == 0)
    {
        if (terminal_parse_sine_params(&line[15], &sine_base_rpm,
                                       &sine_amplitude_rpm,
                                       &sine_frequency_hz) != 0U &&
            pcu_speed_set_sine_params(sine_base_rpm, sine_amplitude_rpm,
                                      sine_frequency_hz) != 0U)
        {
            printf("OK: sine base=%.3f rpm amplitude=%.3f rpm frequency=%.3f Hz\r\n",
                   (double)sine_base_rpm, (double)sine_amplitude_rpm,
                   (double)sine_frequency_hz);
        }
        else
        {
            printf("ERR: sine expects base rpm, non-negative amplitude, and frequency 0.01..50 Hz\r\n");
        }
    }
    else if (strcmp(line, "speed sine start") == 0)
    {
        if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
            (gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE))
        {
            printf("ERR: gyro bias calibration is not complete\r\n");
        }
        else if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
                 (gsa200_data_valid == 0U))
        {
            printf("ERR: no valid gyro feedback\r\n");
        }
        else if (pcu_speed_sine_start() != 0U)
        {
            pcu_terminal_output_requested = 1U;
            printf("OK: sine speed loop started\r\n");
        }
        else
        {
            printf("ERR: sine speed loop rejected; check parameters, PCU feedback, faults, and max current\r\n");
        }
    }
    else if (strcmp(line, "speed sine stop") == 0)
    {
        pcu_position_stop();
        pcu_speed_sine_stop();
        pcu_terminal_output_requested = 0U;
        printf("OK: sine speed loop stopped\r\n");
    }
    else if (strcmp(line, "speed start") == 0)
    {
        if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
            (gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE))
        {
            printf("ERR: gyro bias calibration is not complete\r\n");
        }
        else if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
                 (gsa200_data_valid == 0U))
        {
            printf("ERR: no valid gyro feedback\r\n");
        }
        else if (pcu_speed_start() != 0U)
        {
            pcu_terminal_output_requested = 1U;
            printf("OK: speed loop started\r\n");
        }
        else
        {
            printf("ERR: speed loop rejected; check PCU feedback, faults, and max current\r\n");
        }
    }
    else if (strcmp(line, "speed stop") == 0)
    {
        pcu_position_stop();
        pcu_speed_stop();
        pcu_terminal_output_requested = 0U;
        printf("OK: speed loop stopped\r\n");
    }
    else if (strncmp(line, "speed set ", 10U) == 0)
    {
        if (terminal_parse_float(&line[10], -100000.0f, 100000.0f,
                                 &float_value) != 0U &&
            pcu_speed_set_target_rpm(float_value) != 0U)
        {
            printf("OK: speed target=%.3f rpm\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: speed target must be a finite value\r\n");
        }
    }
    else if (strncmp(line, "speed kp ", 9U) == 0)
    {
        if (terminal_parse_float(&line[9], 0.0f, 100000.0f,
                                 &float_value) != 0U &&
            pcu_speed_set_kp(float_value) != 0U)
        {
            printf("OK: speed kp=%.6f A/rpm\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: speed kp must be >= 0\r\n");
        }
    }
    else if (strncmp(line, "speed ki ", 9U) == 0)
    {
        if (terminal_parse_float(&line[9], 0.0f, 100000.0f,
                                 &float_value) != 0U &&
            pcu_speed_set_ki(float_value) != 0U)
        {
            printf("OK: speed ki=%.6f A/rpm/s\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: speed ki must be >= 0\r\n");
        }
    }
    else if (strncmp(line, "speed max_current ", 18U) == 0)
    {
        if (terminal_parse_float(&line[18], 0.001f, 1000.0f,
                                 &float_value) != 0U &&
            pcu_speed_set_max_current(float_value) != 0U)
        {
            printf("OK: speed max current=%.3f A\r\n", (double)float_value);
        }
        else
        {
            printf("ERR: max current must be > 0 A\r\n");
        }
    }
    else if ((strcmp(line, "disable") == 0) || (strcmp(line, "stop") == 0))
    {
        pcu_speed_stop();
        pcu_position_stop();
        sent = pcu_disable_output();
        pcu_terminal_output_requested = 0U;
        printf(sent != 0U ? "OK: output disabled\r\n" : "ERR: disable send failed\r\n");
    }
    else if (strcmp(line, "clear") == 0)
    {
        pcu_speed_stop();
        pcu_position_stop();
        (void)pcu_disable_output();
        sent = pcu_clear_fault();
        pcu_terminal_output_requested = 0U;
        printf(sent != 0U ? "OK: fault-clear sent\r\n" : "ERR: fault-clear send failed\r\n");
    }
    else if (strncmp(line, "set voltage ", 12U) == 0)
    {
        if (terminal_parse_setpoint(&line[12], &value) != 0U)
        {
            pcu_speed_stop();
            pcu_position_stop();
            pcu_terminal_mode = PCU_CONTROL_VOLTAGE_OPEN_LOOP;
            pcu_terminal_setpoint = value;
            printf("OK: staged voltage raw=%d; type enable to run\r\n", (int)value);
        }
        else
        {
            printf("ERR: raw value must be -32767..32767\r\n");
        }
    }
    else if (strncmp(line, "set current ", 12U) == 0)
    {
        if (terminal_parse_setpoint(&line[12], &value) != 0U)
        {
            pcu_speed_stop();
            pcu_position_stop();
            pcu_terminal_mode = PCU_CONTROL_CURRENT_CLOSED_LOOP;
            pcu_terminal_setpoint = value;
            printf("OK: staged current raw=%d; type enable to run\r\n", (int)value);
        }
        else
        {
            printf("ERR: raw value must be -32767..32767\r\n");
        }
    }
    else if (strcmp(line, "enable") == 0)
    {
        if (pcu_current_test_is_enabled() != 0U)
        {
            printf("ERR: current test mode is active; disable it first\r\n");
        }
        else if ((pcu_position_is_active() != 0U) ||
            (pcu_speed_is_active() != 0U))
        {
            printf("ERR: position move is active; stop it before manual enable\r\n");
        }
        else if (terminal_drive_ready() != 0U)
        {
            sent = pcu_send_command(PCU_OUTPUT_ENABLED,
                                    (pcu_control_mode_t)pcu_terminal_mode,
                                    (int16_t)pcu_terminal_setpoint);
            pcu_terminal_output_requested = sent;
            printf(sent != 0U ? "OK: output command sent\r\n" : "ERR: enable send failed\r\n");
        }
    }
    else if (strncmp(line, "zero ", 5U) == 0)
    {
        if (terminal_parse_setpoint(&line[5], &value) == 0U)
        {
            printf("ERR: raw value must be -32767..32767\r\n");
        }
        else if (terminal_drive_ready() != 0U)
        {
            pcu_position_stop();
            (void)pcu_disable_output();
            sent = pcu_auto_zero(value);
            pcu_terminal_output_requested = sent;
            printf(sent != 0U ? "OK: auto-zero command sent\r\n" : "ERR: auto-zero send failed\r\n");
        }
    }
    else if (strncmp(line, "move ", 5U) == 0)
    {
        if (terminal_parse_angle(&line[5], &angle) == 0U)
        {
            printf("ERR: move angle must be -3600..3600 degrees\r\n");
        }
        else if ((pcu_speed_is_active() != 0U) &&
                 (pcu_speed_get_loop_mode() != PCU_LOOP_MODE_POSITION))
        {
            printf("ERR: speed loop is active; stop it before position move\r\n");
        }
        else if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
                 (gsa200_bias_calibration_state != GSA200_BIAS_COMPLETE))
        {
            printf("ERR: gyro bias calibration is not complete\r\n");
        }
        else if ((pcu_speed_get_feedback_source() == PCU_SPEED_FEEDBACK_GYRO) &&
                 (gsa200_data_valid == 0U))
        {
            printf("ERR: no valid gyro feedback\r\n");
        }
        else if (pcu_position_start_relative(angle) == 0U)
        {
            printf("ERR: position move rejected; check PCU feedback, faults, or active move\r\n");
        }
        else
        {
            printf("OK: relative position move started\r\n");
        }
    }
    else if (line[0] != '\0')
    {
        printf("ERR: unknown command; type help\r\n");
        pcu_terminal_error_count++;
    }

    pcu_terminal_command_count++;
}

static void terminal_process_byte(uint8_t byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if (terminal_line_length != 0U)
        {
            terminal_line[terminal_line_length] = '\0';
            terminal_execute(terminal_line);
            terminal_line_length = 0U;
            terminal_prompt();
        }
        return;
    }

    if ((byte == 0x08U) || (byte == 0x7FU))
    {
        if (terminal_line_length != 0U)
        {
            terminal_line_length--;
        }
        return;
    }

    if (isprint((int)byte) == 0)
    {
        return;
    }
    if (terminal_line_length >= (TERMINAL_LINE_SIZE - 1U))
    {
        terminal_line_length = 0U;
        pcu_terminal_error_count++;
        printf("\r\nERR: command too long\r\n");
        terminal_prompt();
        return;
    }
    terminal_line[terminal_line_length++] = (char)byte;
}

void pcu_terminal_init(void)
{
    memset(terminal_line, 0, sizeof(terminal_line));
    terminal_line_length = 0U;
    pcu_terminal_command_count = 0U;
    pcu_terminal_error_count = 0U;
    pcu_terminal_setpoint = 0;
    pcu_terminal_mode = PCU_CONTROL_CURRENT_CLOSED_LOOP;
    pcu_terminal_output_requested = 0U;

    printf("\r\nPCU terminal ready on J7/USART3 at 921600 8N1. Type help.\r\n");
    terminal_prompt();
}

void pcu_terminal_poll(void)
{
    uint8_t local_buffer[UART_RX_PACKET_MAX_LEN];
    uint8_t length;
    uint8_t index;

    if (uart3_rx.interrupt_flag == 0U)
    {
        return;
    }

    NVIC_DisableIRQ(USART3_IRQn);
    length = uart3_rx.buffer_length;
    memcpy(local_buffer, (const void *)uart3_rx.buffer, length);
    uart3_rx.interrupt_flag = 0U;
    NVIC_EnableIRQ(USART3_IRQn);

    for (index = 0U; index < length; ++index)
    {
        terminal_process_byte(local_buffer[index]);
    }
}
