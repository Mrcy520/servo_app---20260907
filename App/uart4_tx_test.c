#include "uart4_tx_test.h"

#include <string.h>

#include "pcu.h"

volatile uart4_tx_test_status_t g_uart4_tx_test_status;

static uint8_t transmit_attempted;

void uart4_tx_test_init(void)
{
    pcu_init(UART4_TX_TEST_BAUDRATE, PCU_DEFAULT_DEVICE_ID);
    memset((void *)&g_uart4_tx_test_status, 0,
           sizeof(g_uart4_tx_test_status));
    transmit_attempted = 0U;
}

uint8_t uart4_tx_test_send_once(void)
{
    uint8_t result;

    if (transmit_attempted != 0U)
    {
        return g_uart4_tx_test_status.last_result;
    }

    transmit_attempted = 1U;
    g_uart4_tx_test_status.transmit_attempts++;
    result = pcu_set_voltage(UART4_TX_TEST_SETPOINT_50_PERCENT);
    g_uart4_tx_test_status.last_result = result;
    if (result != 0U)
    {
        g_uart4_tx_test_status.transmit_successes++;
    }

    return result;
}

void uart4_tx_test_poll(uint32_t now_ms)
{
    if ((transmit_attempted != 0U) ||
        (now_ms < UART4_TX_TEST_START_DELAY_MS))
    {
        return;
    }

    (void)uart4_tx_test_send_once();
}
