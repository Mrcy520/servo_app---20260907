#ifndef __PCU_TERMINAL_H__
#define __PCU_TERMINAL_H__

#include <stdint.h>

/* J7 / USART3 command terminal. USART3 is also used by printf output. */
void pcu_terminal_init(void);
void pcu_terminal_poll(void);

extern volatile uint32_t pcu_terminal_command_count;
extern volatile uint32_t pcu_terminal_error_count;
extern volatile int32_t pcu_terminal_setpoint;
extern volatile uint32_t pcu_terminal_mode;
extern volatile uint32_t pcu_terminal_output_requested;

#endif /* __PCU_TERMINAL_H__ */
