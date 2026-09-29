#ifndef UART1_TERMINAL_H
#define UART1_TERMINAL_H

#include <stdint.h>

/* Initialize the line parser without transmitting unsolicited data on J9. */
void uart1_terminal_init(void);

/* Feed one non-boot-trigger byte received by boot_poll(). */
void uart1_terminal_process_byte(uint8_t byte);

#endif /* UART1_TERMINAL_H */
