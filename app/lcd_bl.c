#include "app_stm32.h"
#include "main.h"
#include "lcd_bl.h"

extern TIM_HandleTypeDef htim12;

static uint8_t _brightness = 0;

uint8_t
lcd_bl_get_brightness(void)
{
  return _brightness;
}

void
lcd_bl_set_brightness(uint8_t brightness)
{
  // Clip value to 100% maximum to protect against parameter bugs
  if (brightness > 100) brightness = 100;

  // Convert percentage (0-100) to raw pulse width values (0-1000)
  // Math: (brightness * ARR) / 100 -> (brightness * 1000) / 100 = brightness * 10
  uint32_t pulse_value = (uint32_t)brightness * 10;

  // Direct macro write to the hardware registers (Zero CPU overhead)
  __HAL_TIM_SET_COMPARE(&htim12, TIM_CHANNEL_1, pulse_value);
  _brightness = brightness;
}

void
lcd_bl_init(void)
{
  lcd_bl_set_brightness(10);
  HAL_TIM_PWM_Start(&htim12, TIM_CHANNEL_1);
}
