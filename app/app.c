#include "app_stm32.h"
#include "app_common.h"

#include "shell.h"
#include "led.h"
#include "lcd_bl.h"
#include "sdram.h"
#include "lcd_app.h"
#include "lvgl_app.h"
#include "ft5406.h"
#include "dwt.h"

void
app_init(void)
{
  sdram_init();
  dwt_init();

  led_init();
  lcd_bl_init();
  ft5406_init();
  lvgl_app_init();
}
