#include <stdint.h>
#include <stdbool.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "main.h"

#include "led.h"

#define BLINK1_INTERVAL      100

static void
blink_task(void *argument)
{
  (void)argument;

  while(true)
  {
    HAL_GPIO_TogglePin(LED_BLUE_GPIO_Port, LED_BLUE_Pin);
    vTaskDelay(pdMS_TO_TICKS(BLINK1_INTERVAL));
  }
}

void
led_init(void)
{
  xTaskCreate(
    blink_task,               // Task function pointer
    "blink_task",             // Text name for debugging
    128,                      // Stack depth in WORDS (128 words = 512 bytes)
    NULL,                     // Task parameter passed to argument
    tskIDLE_PRIORITY + 2,     // Task priority (Normal equivalent)
    NULL                      // Task handle output (not needed here)
  );
}
