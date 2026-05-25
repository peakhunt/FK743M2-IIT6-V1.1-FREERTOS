#include <stdint.h>
#include <stdbool.h>
#include "app_stm32.h"
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "task.h"
#include "semphr.h"
#include "main.h"

#include "lvgl.h"
#include "src/drivers/display/st_ltdc/lv_st_ltdc.h"

#include "demos/music/lv_demo_music.h"
#include "demos/widgets/lv_demo_widgets.h"
#include "demos/benchmark/lv_demo_benchmark.h"

#include "ft5406.h"

extern LTDC_HandleTypeDef hltdc;
extern DMA2D_HandleTypeDef hdma2d;

static uint8_t* const frame_buffer0 = (uint8_t*)0xC0000000; 
static uint8_t* const frame_buffer1 = (uint8_t*)0xC0200000; 

static SemaphoreHandle_t ltdc_vsync_sem = NULL;

#define LCD_WIDTH           800
#define LCD_HEIGHT          480
#define LCD_COLOR_DEPTH     4

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
  //
  // I don't know why. with LVGL v9.5, DMA2D cuases nothing but all kinds of trouble.
  // and it works much smoother without DMA2D.
  //
  HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_TRANSFERCOMPLETE_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_TRANSFERERROR_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_CLUTLOADINGCPLT_CB_ID, lvgl_dma2d_transfer_complete);
  //HAL_DMA2D_RegisterCallback(&hdma2d, HAL_DMA2D_LINEEVENT_CB_ID, lvgl_dma2d_transfer_complete);

  lv_init();
  lv_tick_set_cb(xTaskGetTickCount);

  lv_display_t* disp = lv_st_ltdc_create_direct(frame_buffer0, frame_buffer1, LTDC_LAYER_1);
  (void)disp;

  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, lvgl_app_touch_read_cb);

  lv_timer_t * read_timer = lv_indev_get_read_timer(indev);
  if (read_timer != NULL)
  {
    lv_timer_set_period(read_timer, 17);
  }

  //lv_demo_music()
  lv_demo_widgets();
  //lv_demo_benchmark();

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
