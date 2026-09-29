#ifndef UART4_TX_TEST_H
#define UART4_TX_TEST_H

#include <stdint.h>

/* Set to 0 to restore normal PCU ownership of UART4. */
#define UART4_TX_TEST_ENABLED       (0U)

#define UART4_TX_TEST_BAUDRATE      (921600U)
#define UART4_TX_TEST_START_DELAY_MS (1000U)
#define UART4_TX_TEST_SETPOINT_50_PERCENT (16384)

typedef struct
{
    uint32_t transmit_attempts;
    uint32_t transmit_successes;
    uint8_t last_result;
} uart4_tx_test_status_t;

extern volatile uart4_tx_test_status_t g_uart4_tx_test_status;

void uart4_tx_test_init(void);
void uart4_tx_test_poll(uint32_t now_ms);
uint8_t uart4_tx_test_send_once(void);

#endif /* UART4_TX_TEST_H */
