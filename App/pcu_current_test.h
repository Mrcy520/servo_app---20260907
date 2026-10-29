#ifndef PCU_CURRENT_TEST_H
#define PCU_CURRENT_TEST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set these variables from the debugger while the target is running. */
extern volatile uint8_t pcu_current_test_enable;
extern volatile float pcu_current_test_offset_a;
extern volatile float pcu_current_test_amplitude_a;
extern volatile float pcu_current_test_frequency_hz;

/* Published test values for the Watch/UART7 waveform monitor. */
extern volatile uint8_t pcu_current_test_active;
extern volatile float pcu_current_test_command_a;
extern volatile float pcu_current_test_phase_rad;
extern volatile int16_t pcu_current_test_raw;
extern volatile uint32_t pcu_current_test_updates;
extern volatile uint32_t pcu_current_test_command_errors;

void pcu_current_test_init(void);
void pcu_current_test_poll(uint32_t now_ms);
uint8_t pcu_current_test_is_enabled(void);
uint8_t pcu_current_test_is_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PCU_CURRENT_TEST_H */
