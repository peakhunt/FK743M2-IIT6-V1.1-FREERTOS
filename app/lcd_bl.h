#pragma once

#include <stdint.h>

extern void lcd_bl_init(void);
extern void lcd_bl_set_brightness(uint8_t brightness);
extern uint8_t lcd_bl_get_brightness(void);
