#include <stdint.h>
#include <stdbool.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "semphr.h"
#include "main.h"

#include "lvgl.h"
#include "demos/widgets/lv_demo_widgets.h" 
#include "ft5406.h"

extern LTDC_HandleTypeDef hltdc;
extern DMA2D_HandleTypeDef hdma2d;

static uint8_t* const frame_buffer0 = (uint8_t*)0xC0000000; 
static uint8_t* const frame_buffer1 = (uint8_t*)0xC0200000; 

static SemaphoreHandle_t ltdc_vsync_sem = NULL;

#define LCD_WIDTH           800
#define LCD_HEIGHT          480
#define LCD_COLOR_DEPTH     4

/////////////////////////////////////////////////////////////////////////
//
// ltdc frame 
//
/////////////////////////////////////////////////////////////////////////
void
HAL_LTDC_ReloadEventCallback(LTDC_HandleTypeDef *hltdc)
{
  if (hltdc->Instance == LTDC)
  {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Release binary semaphore from the ISR
    xSemaphoreGiveFromISR(ltdc_vsync_sem, &xHigherPriorityTaskWoken);

    // Context switch if the unblocked task has a higher priority
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

static void
lvgl_app_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
  uint32_t target_address = (uint32_t)px_map;

  // does it really necessary? isn't it supposed to be a singleshot?
  bool flush_is_last = lv_display_flush_is_last(disp);
  if(flush_is_last)
  {
#if 1
    SCB_CleanDCache();
#else
    uint32_t fb_size = 4 * 800 * 480;
    SCB_CleanDCache_by_Addr((uint32_t *)target_address, fb_size);
#endif
  }

  HAL_LTDC_SetAddress_NoReload(&hltdc, target_address, LTDC_LAYER_1);
  HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);
}

static void
lvgl_app_flush_wait_cb(lv_display_t * disp)
{
  (void)disp;

  // Native block on the binary semaphore with a 30ms timeout
  //xSemaphoreTake(ltdc_vsync_sem, pdMS_TO_TICKS(30));
  xSemaphoreTake(ltdc_vsync_sem, portMAX_DELAY);
}

static void
lvgl_app_touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
  int16_t x, y;
  bool pressed;

  ft5406_get(&x, &y, &pressed);

  if(pressed)
  {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void
lvgl_dma2d_transfer_complete(DMA2D_HandleTypeDef *h_dma2d)
{
  (void)h_dma2d; // Unused parameter
#if LV_USE_DRAW_DMA2D_INTERRUPT
  extern void lv_draw_dma2d_transfer_complete_interrupt_handler(void);

  // Call the mandatory LVGL v9.5 internal interrupt engine handler
  // This executes the native thread synchronization unblocking routine
  lv_draw_dma2d_transfer_complete_interrupt_handler();
#endif
}

/////////////////////////////////////////////////////////////////////////
//
// lvgl task
//
/////////////////////////////////////////////////////////////////////////
static void
lvgl_task(void* arg)
{
  HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_TRANSFERCOMPLETE_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_TRANSFERERROR_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_CLUTLOADINGCPLT_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_LINEEVENT_CB_ID, lvgl_dma2d_transfer_complete);

  lv_init();
  lv_tick_set_cb(xTaskGetTickCount);

  lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);

  uint32_t buffer_size_in_bytes = LCD_WIDTH * LCD_HEIGHT * LCD_COLOR_DEPTH;

  lv_display_set_flush_cb(disp, lvgl_app_flush_cb);
  lv_display_set_flush_wait_cb(disp, lvgl_app_flush_wait_cb);

  lv_display_set_buffers(disp,
      (void*)frame_buffer0,
      (void*)frame_buffer1,
      buffer_size_in_bytes,
      LV_DISPLAY_RENDER_MODE_DIRECT);

  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, lvgl_app_touch_read_cb);

  lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
  if (read_timer != NULL)
  {
    lv_timer_set_period(read_timer, 10);
  }

  lv_demo_widgets();

  while(true)
  {
    uint32_t next_call = lv_timer_handler();
    if (next_call < 1)
    {
      next_call = 1;
    }
    vTaskDelay(pdMS_TO_TICKS(next_call));
  }
}

/////////////////////////////////////////////////////////////////////////
//
//  external interfaces
//
/////////////////////////////////////////////////////////////////////////
void
lvgl_app_init(void)
{
  ltdc_vsync_sem = xSemaphoreCreateBinary();

  // 1024 * 16 bytes = 16384 bytes. Divided by 4 = 4096 words.
  xTaskCreate(
    lvgl_task,
    "lvgl_task",
    4096,
    NULL,
    tskIDLE_PRIORITY + 2,
    NULL
  );
}
