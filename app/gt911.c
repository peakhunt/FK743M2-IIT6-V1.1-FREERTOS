#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "semphr.h"
#include "main.h"
#include "dwt.h"

//
// GT911 Device Address Options:
// Standard default: 0xBA (Write) / 0xBB (Read)
// Alternating fallback: 0x28 (Write) / 0x29 (Read) depending on INT pin strapping at boot.
//
#define GT911_ADDR_WRITE  0x28
#define GT911_ADDR_READ   0x29
//#define GT911_ADDR_WRITE  0xBA
//#define GT911_ADDR_READ   0xBB

#define I2C_SCL_HIGH()   (GPIOE->BSRR = GPIO_BSRR_BS5)
#define I2C_SCL_LOW()    (GPIOE->BSRR = GPIO_BSRR_BR5)
#define I2C_SDA_HIGH()   (GPIOE->BSRR = GPIO_BSRR_BS6)
#define I2C_SDA_LOW()    (GPIOE->BSRR = GPIO_BSRR_BR6)
#define I2C_SDA_READ()   ((GPIOE->IDR & GPIO_IDR_ID6) != 0)


static int16_t touch_cached_x = 0;
static int16_t touch_cached_y = 0;
static bool    touch_cached_pressed = false;

static SemaphoreHandle_t data_mutex = NULL;
static TaskHandle_t gt911_task_handle = NULL;

static inline void
i2c_delay(void)
{
  dwt_delay_us(2);
}

static inline void
i2c_sda_mode_output(void)
{
  GPIOE->MODER &= ~(3U << (6 * 2)); 
  GPIOE->MODER |=  (1U << (6 * 2)); 
  __DSB();
  __ISB();
}

static inline void
i2c_sda_mode_input(void)
{
  GPIOE->MODER &= ~(3U << (6 * 2)); 
  __DSB();
  __ISB();
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
gt911_read_touch(int16_t *x, int16_t *y, bool *is_pressed)
{
  uint8_t status = 0;
  uint8_t read_buffer[4]; 

  // 1. Read GT911 Status Register (16-bit addr: 0x814E)
  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_WRITE)) { i2c_stop(); return false; }
  if (!i2c_write_byte(0x81))             { i2c_stop(); return false; } 
  if (!i2c_write_byte(0x4E))             { i2c_stop(); return false; } 
  i2c_stop(); // XXX care

  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_READ))   { i2c_stop(); return false; }
  status = i2c_read_byte(false); 
  i2c_stop();

  uint8_t touch_count = status & 0x0F;
  bool buffer_ready   = (status & 0x80) != 0;

  // If buffer isn't ready or no points are detected, clear status and exit
  if (!buffer_ready || touch_count == 0)
  {
    *is_pressed = false;
    return true;
  }

  // 2. Read Coordinates First (16-bit starting addr: 0x8150)
  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_WRITE)) { i2c_stop(); return false; }
  if (!i2c_write_byte(0x81))             { i2c_stop(); return false; }
  if (!i2c_write_byte(0x50))             { i2c_stop(); return false; }
  i2c_stop();

  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_READ))   { i2c_stop(); return false; }
  read_buffer[0] = i2c_read_byte(true);  // 0x8150: X LSB
  read_buffer[1] = i2c_read_byte(true);  // 0x8151: X MSB
  read_buffer[2] = i2c_read_byte(true);  // 0x8152: Y LSB
  read_buffer[3] = i2c_read_byte(false); // 0x8153: Y MSB (Send NACK)
  i2c_stop();

  // 4. Parse Native Little-Endian coordinates
  *x = ((int16_t)read_buffer[1] << 8) | read_buffer[0];
  *y = ((int16_t)read_buffer[3] << 8) | read_buffer[2];
  *is_pressed = true;

  return true;
}

static void
gt911_touch_untouch(void)
{
  int16_t raw_x = 0;
  int16_t raw_y = 0;
  bool verified_pressed = false;

  if (!gt911_read_touch(&raw_x, &raw_y, &verified_pressed))
    return;

  if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE)
  {
    if(verified_pressed)
    {
      // Retaining your original display structural axis configuration layout swaps
      touch_cached_x = raw_y; 
      touch_cached_y = raw_x;
    }
    touch_cached_pressed = verified_pressed;
    xSemaphoreGive(data_mutex);
  }
}

void
gt911_get(int16_t* x, int16_t* y, bool* pressed)
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
  if (GPIO_Pin & GPIO_PIN_4)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(gt911_task_handle, 0x01, eSetBits, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void
gt911_read_coord_clear(void)
{
  i2c_start();
  if (i2c_write_byte(GT911_ADDR_WRITE))
  {
    i2c_write_byte(0x81);
    i2c_write_byte(0x4E);
    i2c_write_byte(0x00); 
  }
  i2c_stop();
}

static uint8_t live_cfg[186];

bool
gt911_read_modify_write_config(void)
{
  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_WRITE)) { i2c_stop(); return false; }
  i2c_write_byte(0x80); // Address High
  i2c_write_byte(0x47); // Address Low (0x8047)
  i2c_stop(); // XXX care

  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_READ))   { i2c_stop(); return false; }

  for (int i = 0; i < 184; i++)
  {
    live_cfg[i] = i2c_read_byte(i < 183); // ACK all elements except the 184th
  }
  i2c_stop();

  if(live_cfg[1] == 0 || live_cfg[3] == 0 || live_cfg[5] == 0)
  {
    return false;
  }

  live_cfg[0] += 1;

  //live_cfg[6] &= ~0x03;
  //live_cfg[6] |= 0x01;     // 0x804d, falling edge irq
  live_cfg[6] = 0x01;     // 0x804d, falling edge irq

  // Force your 480x272 display geometry limits into the active matrix
#if 0 // why? x2y doesn't work and only this works. why?
  live_cfg[1] = 0xE0; // 0x8048: X Output Resolution LSB
  live_cfg[2] = 0x01; // 0x8049: X Output Resolution MSB (0x01E0 = 480)
  live_cfg[3] = 0x10; // 0x804A: Y Output Resolution LSB
  live_cfg[4] = 0x01; // 0x804B: Y Output Resolution MSB (0x0110 = 272)
#else
  live_cfg[3] = 0xE0; // 0x8048: X Output Resolution LSB
  live_cfg[4] = 0x01; // 0x8049: X Output Resolution MSB (0x01E0 = 480)
  live_cfg[1] = 0x10; // 0x804A: Y Output Resolution LSB
  live_cfg[2] = 0x01; // 0x804B: Y Output Resolution MSB (0x0110 = 272)
#endif

  live_cfg[5] = 0x01;   // 0x804c, num touch count

  uint8_t sum = 0;
  for (int i = 0; i < 184; i++)
  {
    sum += live_cfg[i];
  }

  // Index 184 maps to Register 0x80FF (The Checksum Location)
  live_cfg[184] = (~sum) + 1; 

  // Index 185 maps to Register 0x8100 (The Config_Fresh Refresh Flag)
  live_cfg[185] = 0x01; 

  i2c_start();
  if (!i2c_write_byte(GT911_ADDR_WRITE)) { i2c_stop(); return false; }
  i2c_write_byte(0x80); 
  i2c_write_byte(0x47); 

  // Stream out the full array matching the exact footprint requirements
  for (int i = 0; i < 186; i++)
  {
    if (!i2c_write_byte(live_cfg[i]))
    {
      i2c_stop();
      return false;
    }
  }
  i2c_stop();
  return true;
}

static void
gt911_ctp_nint_to_output(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  HAL_NVIC_DisableIRQ(CTP_NINT_EXTI_IRQn);

  HAL_GPIO_WritePin(CTP_NINT_GPIO_Port, CTP_NINT_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = CTP_NINT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP; 
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

  HAL_GPIO_Init(CTP_NINT_GPIO_Port, &GPIO_InitStruct);
}

static void
gt911_ctp_nint_to_exti(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  GPIO_InitStruct.Pin = CTP_NINT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;

  HAL_GPIO_Init(CTP_NINT_GPIO_Port, &GPIO_InitStruct);

  __HAL_GPIO_EXTI_CLEAR_IT(CTP_NINT_Pin);
  HAL_NVIC_EnableIRQ(CTP_NINT_EXTI_IRQn);
}

static void
gt911_reset_and_read_config(void)
{
  bool done = false;

  while(!done)
  {
    gt911_ctp_nint_to_output();

    HAL_GPIO_WritePin(CTP_NRST_GPIO_Port, CTP_NRST_Pin, GPIO_PIN_RESET);
    osDelay(20);
    HAL_GPIO_WritePin(CTP_NINT_GPIO_Port, CTP_NINT_Pin, GPIO_PIN_SET);
    osDelay(2);
    HAL_GPIO_WritePin(CTP_NRST_GPIO_Port, CTP_NRST_Pin, GPIO_PIN_SET);
    osDelay(6);

    HAL_GPIO_WritePin(CTP_NINT_GPIO_Port, CTP_NINT_Pin, GPIO_PIN_RESET);
    osDelay(50);
    gt911_ctp_nint_to_exti();

    done = gt911_read_modify_write_config();
  }
}

static void
gt911_task(void* arg)
{
  (void)arg;
  uint32_t notified_value;


  gt911_reset_and_read_config();

  while(true)
  {
    BaseType_t result = xTaskNotifyWait(0x00, 0x01, &notified_value, portMAX_DELAY);

    if (!(result == pdTRUE && (notified_value & 0x01)))
      continue;

    gt911_touch_untouch();
    gt911_read_coord_clear();
  }
}

void
gt911_init(void)
{
  data_mutex = xSemaphoreCreateMutex();

  xTaskCreate(
    gt911_task,              
    "gt911_task",            
    256,                      
    NULL,                     
    tskIDLE_PRIORITY + 4,     
    &gt911_task_handle       
  );
}
