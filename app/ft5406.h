#pragma once

#include <stdint.h>
#include <stdbool.h>

extern void ft5406_init(void);
extern void ft5406_get(int16_t* x, int16_t* y, bool* pressed);
