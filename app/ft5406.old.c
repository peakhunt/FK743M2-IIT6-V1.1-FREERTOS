#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "semphr.h"
#include "main.h"
#include "ft5406.h"
#include "dwt.h"

//
// 0x70 for write
// 0x71 for read
//
#define I2C_SCL_HIGH()   (GPIOI->BSRR = GPIO_BSRR_BS11)
#define I2C_SCL_LOW()    (GPIOI->BSRR = GPIO_BSRR_BR11)
#define I2C_SDA_HIGH()   (GPIOI->BSRR = GPIO_BSRR_BS8)
#define I2C_SDA_LOW()    (GPIOI->BSRR = GPIO_BSRR_BR8)
#define I2C_SDA_READ()   ((GPIOI->IDR & GPIO_IDR_ID8) != 0)

static int16_t touch_cached_x = 0;
static int16_t touch_cached_y = 0;
static bool    touch_cached_pressed = false;

static SemaphoreHandle_t data_mutex = NULL;
static TaskHandle_t ft5406_task_handle = NULL;

// Quick software microsecond delay wrapper for 100kHz-400kHz I2C speeds
static inline void
i2c_delay(void)
{
  // 5us delay for 100KHz
  // 2.5us delay for 200KHz
  // 1.25us for 400Khz
  dwt_delay_us(2);
}

static inline void
i2c_sda_mode_output(void)
{
  GPIOI->MODER &= ~(3U << (8 * 2)); // Clear mode bits for pin 8
  GPIOI->MODER |=  (1U << (8 * 2)); // Set as General Purpose Output
}

static inline void
i2c_sda_mode_input(void)
{
  GPIOI->MODER &= ~(3U << (8 * 2)); // Set as Input mode (00)
}


void
i2c_start(void)
{
  i2c_sda_mode_output();

  I2C_SDA_HIGH();
  I2C_SCL_HIGH();
  i2c_delay();
  I2C_SDA_LOW();
  i2c_delay();
  I2C_SCL_LOW();
  i2c_delay();
}

void
i2c_stop(void)
{
  i2c_sda_mode_output();

  I2C_SDA_LOW();
  I2C_SCL_HIGH();
  i2c_delay();

  I2C_SDA_HIGH();
  i2c_delay();
}

bool
i2c_write_byte(uint8_t byte)
{
  i2c_sda_mode_output();
  for (int i = 0; i < 8; i++)
  {
    if (byte & 0x80)
      I2C_SDA_HIGH();
    else
      I2C_SDA_LOW();
    byte <<= 1;
    i2c_delay();
    I2C_SCL_HIGH();
    i2c_delay();
    I2C_SCL_LOW();
  }

  // Read ACK flag step
  i2c_sda_mode_input();

  i2c_delay();
  I2C_SCL_HIGH();
  i2c_delay();

  bool ack = (I2C_SDA_READ() == 0);

  I2C_SCL_LOW();
  i2c_delay();

  return ack;
}

uint8_t
i2c_read_byte(bool ack)
{
  uint8_t byte = 0;
  i2c_sda_mode_input();
  for (int i = 0; i < 8; i++)
  {
    byte <<= 1;
    I2C_SCL_HIGH();
    i2c_delay();

    if (I2C_SDA_READ())
      byte |= 0x01;

    I2C_SCL_LOW();
    i2c_delay();
  }
  // Send ACK/NACK step
  i2c_sda_mode_output();

  if (ack)
    I2C_SDA_LOW();
  else
    I2C_SDA_HIGH();

  i2c_delay();
  I2C_SCL_HIGH();
  i2c_delay();
  I2C_SCL_LOW();
  i2c_delay();
  return byte;
}

bool
ft5406_read_touch(int16_t *x, int16_t *y, bool *is_pressed)
{
  uint8_t buf[5]; // Storage for registers 0x02 to 0x06

  // 2. Stream the 5-byte data block over the bit-bang interface
  i2c_start();
  if (!i2c_write_byte(0x70)) { i2c_stop(); return false; } // Device Write Address
  if (!i2c_write_byte(0x02)) { i2c_stop(); return false; } // Point to TD_STATUS

  i2c_start();
  if (!i2c_write_byte(0x71)) { i2c_stop(); return false; } // Device Read Address

  buf[0] = i2c_read_byte(true);  // 0x02: TD_STATUS (Number of touch points)
  buf[1] = i2c_read_byte(true);  // 0x03: TOUCH1_XH (Event Flag [7:6] + X MSB [3:0])
  buf[2] = i2c_read_byte(true);  // 0x04: TOUCH1_XL (X LSB [7:0])
  buf[3] = i2c_read_byte(true);  // 0x05: TOUCH1_YH (Touch ID [7:4] + Y MSB [3:0])
  buf[4] = i2c_read_byte(false); // 0x06: TOUCH1_YL (Y LSB [7:0] - Send NACK)
  i2c_stop();

  // 3. Extract the Touch Count and Event Status Flags
  uint8_t touch_count = buf[0] & 0x0F;
  uint8_t event_flag  = (buf[1] >> 6) & 0x03;

  if(touch_count == 0)
  {
    *is_pressed = false;
    return true;
  }

  static int16_t last_x = 0, last_y = 0;
  int16_t raw_x, raw_y;

  raw_x = ((int16_t)(buf[1] & 0x0F) << 8) | buf[2];
  raw_y = ((int16_t)(buf[3] & 0x0F) << 8) | buf[4];

  switch(event_flag)
  {
  case 3: // reserved
    return false;

  case 1: // up
    *is_pressed = false;
    break;

  case 2: // contact
    {
      int16_t delta_x = abs(raw_x - last_x);
      int16_t delta_y = abs(raw_y - last_y);
      int16_t jitter_threshold = 20; // Number of noise pixels to ignore

      if (delta_x > jitter_threshold) last_x = raw_x;
      if (delta_y > jitter_threshold) last_y = raw_y;
      /* 
      // METHOD B: Exponential Moving Average (alternative if you want ultra-smooth drags)
      float alpha = 0.3; // 0.1 = very smooth but sluggish, 0.9 = responsive but jittery
      last_x = (int16_t)((raw_x * alpha) + (last_x * (1.0f - alpha)));
      last_y = (int16_t)((raw_y * alpha) + (last_y * (1.0f - alpha)));
      */
      *x = last_x;
      *y = last_y;
    }
  case 0: // down
    last_x = raw_x;
    last_y = raw_y;
    *x = last_x;
    *y = last_y;
    *is_pressed = true;
  }
  return true;
}

static void
ft5406_touch_untouch(void)
{
  int16_t raw_x = 0;
  int16_t raw_y = 0;
  bool verified_pressed = false;

  // 1. Let the I2C transfer pull the truth straight from the chip registers
  if (!ft5406_read_touch(&raw_x, &raw_y, &verified_pressed))
    return;

  // 2. Lock and assign based strictly on the register result
  if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE)
  {
    if(verified_pressed)
    {
      touch_cached_x = raw_y; // Retaining your axis swap
      touch_cached_y = raw_x;
    }
    touch_cached_pressed = verified_pressed;
    xSemaphoreGive(data_mutex);
  }
}

void
ft5406_get(int16_t* x, int16_t* y, bool* pressed)
{
  if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE)
  {
    *x = touch_cached_x;
    *y = touch_cached_y;
    *pressed = touch_cached_pressed;

    xSemaphoreGive(data_mutex);
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin & GPIO_PIN_3)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Use Task Notifications as a fast, light alternative to thread flags
    // Bitwise OR (0x01) sets bit 0, mirroring your previous flag logic
    xTaskNotifyFromISR(ft5406_task_handle, 
        0x01, 
        eSetBits, 
        &xHigherPriorityTaskWoken);

    // Forces context switch if the unblocked task has a higher priority
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void
ft5406_task(void* arg)
{
  (void)arg;
  uint32_t notified_value;

  // just to make sure
  i2c_start();
  i2c_write_byte(0x70);
  i2c_write_byte(0x00);
  i2c_write_byte(0x00);
  i2c_stop();

  // force active mode
  i2c_start();
  i2c_write_byte(0x70);
  i2c_write_byte(0xa5);
  i2c_write_byte(0x00);
  i2c_stop();

  // set fastest operational scanning rate
  i2c_start();
  i2c_write_byte(0x70);
  i2c_write_byte(0x88);
  i2c_write_byte(0x00);
  i2c_stop();

  while(true)
  {
    BaseType_t result = xTaskNotifyWait(0x00,              // Do not clear bits on entry
                                        0x01,              // Clear bit 0 on exit
                                        &notified_value,   // Stores flag value
                                        portMAX_DELAY);    // Wait forever

    // Verify the operation succeeded and bit 0 was actually set
    if (!(result == pdTRUE && (notified_value & 0x01)))
      continue;

    ft5406_touch_untouch();
  }
}

void
ft5406_init(void)
{
  HAL_GPIO_WritePin(CTP_NRST_GPIO_Port, CTP_NRST_Pin, GPIO_PIN_SET);

  data_mutex = xSemaphoreCreateMutex();

  // 2. Create the Task using native FreeRTOS API
  xTaskCreate(
    ft5406_task,              // Task function pointer
    "ft5406_task",            // Text name for debugging
    128,                      // Stack size in WORDS (512 bytes / 4 = 128 words)
    NULL,                     // Task parameter
    tskIDLE_PRIORITY + 4,     // Task priority (High)
    &ft5406_task_handle       // Pass the address to store the output handle
  );
}
