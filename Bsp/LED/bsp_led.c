
#include "bsp_led.h"


/** \brief Toggle interval time in milliseconds. */
#define LED_TOGGLE_MS  (500)

/**
  * @brief  Initialize LED1 (Green LED).
  * @param  None
  * @retval None
  */
void LED_Init(void)
{
	/* Enable the LED1 Clock */
	LED_GPIO_CLK_ENABLE();

	/* Configure IO in output push-pull mode to drive external LED1 */
	LL_GPIO_SetPinMode(LED_GPIO_PORT, LED_PIN, LL_GPIO_MODE_OUTPUT);
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @param  None
  * @retval None
  */

/* Toggle LED1 */
void LED_Toggle(void)
{
  LL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
}

