#include <stdint.h>
#include <stdbool.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "main.h"

#include "sdram.h"

static void sdram_init_sequence(SDRAM_HandleTypeDef *hsdram)
{
  FMC_SDRAM_CommandTypeDef Command;

  // 1. Enable Clock
  Command.CommandMode = FMC_SDRAM_CMD_CLK_ENABLE;
  Command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  Command.AutoRefreshNumber = 1;
  Command.ModeRegisterDefinition = 0;
  HAL_SDRAM_SendCommand(hsdram, &Command, HAL_MAX_DELAY);
  osDelay(1);

  // 2. Precharge All
  Command.CommandMode = FMC_SDRAM_CMD_PALL;
  Command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  Command.AutoRefreshNumber = 1;
  Command.ModeRegisterDefinition = 0;
  HAL_SDRAM_SendCommand(hsdram, &Command, HAL_MAX_DELAY);

  // 3. Auto-Refresh (8 cycles required)
  Command.CommandMode = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
  Command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  Command.AutoRefreshNumber = 8;
  Command.ModeRegisterDefinition = 0;
  HAL_SDRAM_SendCommand(hsdram, &Command, HAL_MAX_DELAY);

  // 4. Load Mode Register (Configures CAS Latency 3, Burst Length 1, Standard Mode)
  Command.CommandMode = FMC_SDRAM_CMD_LOAD_MODE;
  Command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  Command.AutoRefreshNumber = 1;
  //Command.ModeRegisterDefinition = 0x0230; 
  // Correct Value for: CAS 3, Burst Length 1, Sequential, Single Write
  Command.ModeRegisterDefinition = 0x0200; 

  HAL_SDRAM_SendCommand(hsdram, &Command, HAL_MAX_DELAY);

  HAL_SDRAM_ProgramRefreshRate(hsdram, 1855);
}

void
sdram_test(void)
{
  volatile uint32_t *sdram_ptr = (volatile uint32_t *)0xC0000000;
  uint32_t total_words = (32 * 1024 * 1024) / 4; 

  // Phase 1: Write walking pattern to saturate the 16-bit data bus and 13-bit address lines
  for (uint32_t i = 0; i < total_words; i++)
  {
    // Generates a unique pseudo-random pattern for every unique memory offset
    sdram_ptr[i] = i ^ 0x5A5A5A5A;
  }

  // Phase 2: Read back and verify every single byte across the entire 32MB space
  for (uint32_t i = 0; i < total_words; i++)
  {
    uint32_t expected = i ^ 0x5A5A5A5A;
    if (sdram_ptr[i] != expected)
    {
      while(1)
        ;
    }
  }
}

void
sdram_init(void)
{
  extern SDRAM_HandleTypeDef hsdram1;
  sdram_init_sequence(&hsdram1);
  //sdram_test();
}
