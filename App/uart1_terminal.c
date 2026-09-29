#include "uart1_terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_uart.h"
#include "pcu.h"
#include "uart4_tx_test.h"

#define TERMINAL_LINE_SIZE       (64U)
#define TERMINAL_RESPONSE_SIZE   (192U)
#define TERMINAL_TX_WAIT_LIMIT   (1000000UL)

static char terminal_line[TERMINAL_LINE_SIZE];
static uint8_t terminal_line_length;
static uint8_t terminal_line_overflow;

static void terminal_send(const char *text)
{
    uint32_t timeout = TERMINAL_TX_WAIT_LIMIT;
    size_t length = strlen(text);

    /* Do not let a new response truncate an earlier USART1 DMA transfer. */
    while (LL_DMA_IsEnabledStream(DMA1, LL_DMA_STREAM_0))
    {
        if (timeout-- == 0U)
        {
            return;
        }
    }

    timeout = TERMINAL_TX_WAIT_LIMIT;
    while (LL_USART_IsActiveFlag_TC(USART1) == 0U)
    {
        if (timeout-- == 0U)
        {
            return;
        }
    }

    if (length > UART_RX_PACKET_MAX_LEN)
    {
        length = UART_RX_PACKET_MAX_LEN;
    }
    uart1_transmit((const uint8_t *)text, (uint8_t)length);
}

static uint8_t terminal_no_argument(const char *argument)
{
    while (*argument == ' ' || *argument == '\t')
    {
        ++argument;
    }
    return (uint8_t)(*argument == '\0');
}

static uint8_t terminal_parse_setpoint(const char *argument, int16_t *setpoint)
{
    char *end;
    long value;

    while (*argument == ' ' || *argument == '\t')
    {
        ++argument;
    }
    if (*argument == '\0')
    {
        return 0U;
    }

    value = strtol(argument, &end, 10);
    if (end == argument)
    {
        return 0U;
    }
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    if ((*end != '\0') ||
        (value < PCU_SETPOINT_MIN) ||
        (value > PCU_SETPOINT_MAX))
    {
        return 0U;
    }

    *setpoint = (int16_t)value;
    return 1U;
}

static void terminal_send_command_result(const char *command, uint8_t success)
{
    char response[TERMINAL_RESPONSE_SIZE];

    if (success != 0U)
    {
        (void)snprintf(response, sizeof(response), "OK %s\r\n", command);
    }
    else
    {
        (void)snprintf(response, sizeof(response), "ERR %s failed\r\n", command);
    }
    terminal_send(response);
}

static void terminal_send_status(void)
{
    char response[TERMINAL_RESPONSE_SIZE];

    if (g_pcu_status.data_valid == 0U)
    {
        (void)snprintf(response, sizeof(response),
                       "ERR no PCU status; frames=%lu checksum_errors=%lu format_errors=%lu\r\n",
                       (unsigned long)g_pcu_status.valid_frames,
                       (unsigned long)g_pcu_status.checksum_errors,
                       (unsigned long)g_pcu_status.format_errors);
    }
    else
    {
        uint32_t angle_mdeg = (uint32_t)
            ((((uint64_t)g_pcu_status.angle_raw * 360000ULL) + 8388608ULL) /
             16777216ULL);

        (void)snprintf(response, sizeof(response),
                       "OK id=0x%02X status=0x%02X angle_mdeg=%lu frames=%lu "
                       "checksum_errors=%lu format_errors=%lu\r\n",
                       g_pcu_status.device_id,
                       g_pcu_status.status,
                       (unsigned long)angle_mdeg,
                       (unsigned long)g_pcu_status.valid_frames,
                       (unsigned long)g_pcu_status.checksum_errors,
                       (unsigned long)g_pcu_status.format_errors);
    }
    terminal_send(response);
}

static void terminal_execute_line(char *line)
{
    char *argument;
    int16_t setpoint;
    char *cursor = line;

    while (*cursor == ' ' || *cursor == '\t')
    {
        ++cursor;
    }
    if (*cursor == '\0')
    {
        return;
    }

    argument = cursor;
    while ((*argument != '\0') && (*argument != ' ') && (*argument != '\t'))
    {
        if ((*argument >= 'A') && (*argument <= 'Z'))
        {
            *argument = (char)(*argument - 'A' + 'a');
        }
        ++argument;
    }
    if (*argument != '\0')
    {
        *argument++ = '\0';
    }

#if UART4_TX_TEST_ENABLED
    if ((strcmp(cursor, "voltage") == 0) ||
        (strcmp(cursor, "current") == 0) ||
        (strcmp(cursor, "zero") == 0) ||
        (strcmp(cursor, "stop") == 0) ||
        (strcmp(cursor, "clear") == 0) ||
        (strcmp(cursor, "status") == 0))
    {
        terminal_send("ERR PCU disabled: UART4 TX test mode is active\r\n");
        return;
    }
#endif

    if ((strcmp(cursor, "voltage") == 0) || (strcmp(cursor, "current") == 0) ||
        (strcmp(cursor, "zero") == 0))
    {
        if (terminal_parse_setpoint(argument, &setpoint) == 0U)
        {
            terminal_send("ERR setpoint must be -32767..32767\r\n");
            return;
        }

        if (strcmp(cursor, "voltage") == 0)
        {
            terminal_send_command_result(cursor, pcu_set_voltage(setpoint));
        }
        else if (strcmp(cursor, "current") == 0)
        {
            terminal_send_command_result(cursor, pcu_set_current(setpoint));
        }
        else
        {
            terminal_send_command_result(cursor, pcu_auto_zero(setpoint));
        }
        return;
    }

    if (terminal_no_argument(argument) == 0U)
    {
        terminal_send("ERR unexpected argument\r\n");
        return;
    }

    if (strcmp(cursor, "stop") == 0)
    {
        terminal_send_command_result(cursor, pcu_disable_output());
    }
    else if (strcmp(cursor, "clear") == 0)
    {
        terminal_send_command_result(cursor, pcu_clear_fault());
    }
    else if (strcmp(cursor, "status") == 0)
    {
        terminal_send_status();
    }
    else if (strcmp(cursor, "help") == 0)
    {
        terminal_send("Commands: voltage <n>, current <n>, stop, zero <n>, clear, status, help\r\n"
                      "Setpoint range: -32767..32767\r\n");
    }
    else
    {
        terminal_send("ERR unknown command; enter help\r\n");
    }
}

void uart1_terminal_init(void)
{
    memset(terminal_line, 0, sizeof(terminal_line));
    terminal_line_length = 0U;
    terminal_line_overflow = 0U;
}

void uart1_terminal_process_byte(uint8_t byte)
{
    if ((byte == '\r') || (byte == '\n'))
    {
        if (terminal_line_overflow != 0U)
        {
            terminal_send("ERR command too long\r\n");
        }
        else if (terminal_line_length != 0U)
        {
            terminal_line[terminal_line_length] = '\0';
            terminal_execute_line(terminal_line);
        }

        terminal_line_length = 0U;
        terminal_line_overflow = 0U;
        return;
    }

    if ((byte == 0x08U) && (terminal_line_length != 0U))
    {
        --terminal_line_length;
        return;
    }

    if ((byte < 0x20U) || (byte > 0x7EU) || (terminal_line_overflow != 0U))
    {
        return;
    }

    if (terminal_line_length >= (TERMINAL_LINE_SIZE - 1U))
    {
        terminal_line_overflow = 1U;
        return;
    }

    terminal_line[terminal_line_length++] = (char)byte;
}
