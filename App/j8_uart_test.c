#include "j8_uart_test.h"

#include "bsp_uart.h"

#define J8_TEST_INTERVAL_MS (2000U)

static uint32_t last_transmit_ms;
static const uint8_t test_message[] = "NB\r\n";

void j8_uart_test_init(void)
{
    uart6_init(BAUDRATE_115200);
    last_transmit_ms = 0U;
}

void j8_uart_test_poll(uint32_t now_ms)
{
    if ((uint32_t)(now_ms - last_transmit_ms) < J8_TEST_INTERVAL_MS)
    {
        return;
    }

    last_transmit_ms = now_ms;
    uart6_transmit(test_message, (uint8_t)(sizeof(test_message) - 1U));
}
