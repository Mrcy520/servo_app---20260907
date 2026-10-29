#include "waveform_uart7.h"

#include "bsp_uart.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#define WAVEFORM_UART7_FRAME_HEADER       (0xAAU)
#define WAVEFORM_UART7_FRAME_SIZE         (20U)
#define WAVEFORM_UART7_VALUE_SCALE        (100.0f)
#define WAVEFORM_UART7_VALUE_MAX          (327.67f)
#define WAVEFORM_UART7_VALUE_MIN          (-327.68f)

static uint8_t waveform_uart7_is_busy(void)
{
	/* Wait for both the DMA transfer and the final UART stop bit. */
	if (LL_DMA_IsEnabledStream(DMA2, LL_DMA_STREAM_4) != 0U)
	{
		return 1U;
	}

	if (LL_USART_IsActiveFlag_TC(UART7) == 0U)
	{
		return 1U;
	}

	return 0U;
}

static int16_t waveform_uart7_encode_value(float value)
{
	float scaled_value;

	if (value >= WAVEFORM_UART7_VALUE_MAX)
	{
		return INT16_MAX;
	}

	if (value <= WAVEFORM_UART7_VALUE_MIN)
	{
		return INT16_MIN;
	}

	scaled_value = value * WAVEFORM_UART7_VALUE_SCALE;
	if (scaled_value >= 0.0f)
	{
		scaled_value += 0.5f;
	}
	else
	{
		scaled_value -= 0.5f;
	}

	return (int16_t)scaled_value;
}

waveform_uart7_status_t waveform_uart7_send9(
	const float channel_values[WAVEFORM_UART7_CHANNEL_COUNT])
{
	uint8_t frame[WAVEFORM_UART7_FRAME_SIZE];
	uint8_t checksum = 0U;
	uint8_t data_offset;
	uint8_t index;
	uint16_t encoded_bits;
	int16_t encoded_value;

	if (channel_values == NULL)
	{
		return WAVEFORM_UART7_INVALID_ARGUMENT;
	}

	/* Reject NaN and infinities before starting a new DMA transfer. */
	for (index = 0U; index < WAVEFORM_UART7_CHANNEL_COUNT; ++index)
	{
		float value = channel_values[index];

		if ((value != value) || (value > FLT_MAX) || (value < -FLT_MAX))
		{
			return WAVEFORM_UART7_INVALID_VALUE;
		}
	}

	if (waveform_uart7_is_busy() != 0U)
	{
		return WAVEFORM_UART7_BUSY;
	}

	memset(frame, 0, sizeof(frame));
	frame[0] = WAVEFORM_UART7_FRAME_HEADER;

	for (index = 0U; index < WAVEFORM_UART7_CHANNEL_COUNT; ++index)
	{
		encoded_value = waveform_uart7_encode_value(channel_values[index]);
		encoded_bits = (uint16_t)encoded_value;
		data_offset = (uint8_t)(1U + (index * 2U));
		frame[data_offset] = (uint8_t)(encoded_bits >> 8);
		frame[data_offset + 1U] = (uint8_t)encoded_bits;
	}

	for (index = 0U; index < (WAVEFORM_UART7_FRAME_SIZE - 1U); ++index)
	{
		checksum = (uint8_t)(checksum + frame[index]);
	}
	frame[WAVEFORM_UART7_FRAME_SIZE - 1U] = checksum;

	if (uart7_transmit(frame, WAVEFORM_UART7_FRAME_SIZE) !=
	    WAVEFORM_UART7_FRAME_SIZE)
	{
		return WAVEFORM_UART7_SEND_ERROR;
	}

	return WAVEFORM_UART7_OK;
}
