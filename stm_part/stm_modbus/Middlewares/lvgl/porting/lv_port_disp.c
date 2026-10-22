/**
 * @file lv_port_disp.c
 * @brief LVGL 显示端口：ST7789 240x320 竖屏，走 SPI2。
 *
 * 只做三件事：把屏初始化起来、给 LVGL 一块画布、把 flush 回调接到 LCD_Fill()。
 * 真正的 SPI 时序都在 Components/LCD/lcd_st7789.c 里。
 */

#if 1

/*********************
 *      INCLUDES
 *********************/
#include "lv_port_disp.h"
#include "lcd_st7789.h"

#include <stdbool.h>

/*********************
 *      DEFINES
 *********************/

/* 面板分辨率直接取 LCD 组件的宏，避免两处各写一遍对不上 */
#define MY_DISP_HOR_RES     LCD_Width       /* 240 */
#define MY_DISP_VER_RES     LCD_Height      /* 320 */

/* 画布给 20 行。240*20*2 = 9600 字节。
 * flush 是阻塞式的（LCD_Fill 里逐像素等 TXE），所以双缓冲没有意义，用一块就够。 */
#define DISP_BUF_ROWS       20

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void disp_init(void);
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    disp_init();

    static lv_disp_draw_buf_t draw_buf_dsc;
    static lv_color_t buf[DISP_BUF_ROWS * MY_DISP_HOR_RES];
    lv_disp_draw_buf_init(&draw_buf_dsc, buf, NULL, DISP_BUF_ROWS * MY_DISP_HOR_RES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res  = MY_DISP_HOR_RES;
    disp_drv.ver_res  = MY_DISP_VER_RES;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf_dsc;

    lv_disp_drv_register(&disp_drv);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void disp_init(void)
{
    SPI_LCD_Init();
}

volatile bool disp_flush_enabled = true;

void disp_enable_update(void)
{
    disp_flush_enabled = true;
}

void disp_disable_update(void)
{
    disp_flush_enabled = false;
}

/* 把 LVGL 渲染好的这块区域推到屏上。
 * LV_COLOR_DEPTH = 16，lv_color_t 就是一个 RGB565 的 u16，可以直接当像素缓冲用。 */
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    if(disp_flush_enabled) {
        LCD_Fill((uint16_t)area->x1, (uint16_t)area->y1,
                 (uint16_t)area->x2, (uint16_t)area->y2,
                 (uint16_t *)color_p);
    }

    /*IMPORTANT!!!
     *Inform the graphics library that you are ready with the flushing*/
    lv_disp_flush_ready(disp_drv);
}

#else /*Enable this file at the top*/

/*This dummy typedef exists purely to silence -Wpedantic.*/
typedef int keep_pedantic_happy;
#endif
