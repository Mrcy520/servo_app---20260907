#ifndef WAVEFORM_UART7_H
#define WAVEFORM_UART7_H

#include <stdint.h>

#define WAVEFORM_UART7_CHANNEL_COUNT (9U)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	WAVEFORM_UART7_OK = 0,
	WAVEFORM_UART7_BUSY,
	WAVEFORM_UART7_INVALID_ARGUMENT,
	WAVEFORM_UART7_INVALID_VALUE,
	WAVEFORM_UART7_SEND_ERROR
} waveform_uart7_status_t;

/**
 * @brief  Send nine waveform channel values in one frame through UART7.
 * @param  channel_values Ch1..Ch9 values in array indexes 0..8.
 * @retval WAVEFORM_UART7_OK when the 20-byte frame has been submitted.
 *         WAVEFORM_UART7_BUSY when the previous UART7 frame is still sending.
 *
 * Every value is encoded as a signed big-endian int16(value * 100). Finite
 * values outside -327.68..327.67 are saturated to the protocol limits.
 */
waveform_uart7_status_t waveform_uart7_send9(
	const float channel_values[WAVEFORM_UART7_CHANNEL_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* WAVEFORM_UART7_H */
