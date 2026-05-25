#include <stdint.h>
#include <stdbool.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "main.h"

void
dwt_delay_us(uint32_t us)
{
  uint32_t cycles = (us * (SystemCoreClock / 1000000));
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < cycles);
}

void
dwt_init(void)
{
  // 1. Enable the Core Debug Trace Monitor
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

  // 2. Unlock DWT access (Required for Cortex-M7 cores, optional but safe for M3/M4)
  DWT->LAR = 0xC5ACCE55; 
  __DSB();

  // 3. Reset the cycle counter register
  DWT->CYCCNT = 0;

  // 4. Enable the cycle counter
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

