
/**
 * @file main
 *
 */

/*********************
 *      INCLUDES
 *********************/
#define _DEFAULT_SOURCE /* needed for usleep() */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define SDL_MAIN_HANDLED /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#include "lv_drivers/sdl/sdl.h"
#include "ui/ui_env_monitor.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void hal_init(void);
static void hal_init_headless(void);
static void dump_ppm(const char *path);
static void dump_layout(lv_obj_t *o, int depth);
static void headless_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *      VARIABLES
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

int main(int argc, char **argv)
{
  const bool dump = (argc > 1) && (strcmp(argv[1], "--dump") == 0);

  /*Initialize LVGL*/
  lv_init();

  /* `--dump out.ppm`：不开窗口，把界面渲染一次存成 PPM 就退出。
   * 用于没有显示器/不方便看窗口时核对版式。 */
  if(dump) {
      hal_init_headless();
  }
  else {
      /*Initialize the HAL (display, input devices, tick) for LVGL*/
      hal_init();
  }

  /* 环境监测主界面。数值这里先写死，接上 Modbus 后由采样任务调用
   * ui_env_set_values() 刷新。 */
  ui_env_monitor_create();
  ui_env_set_values(0, "26.5", "58");
  ui_env_set_values(1, "24.8", "62");

  if(dump) {
      lv_obj_update_layout(lv_scr_act());
      dump_layout(lv_scr_act(), 0);
      dump_ppm((argc > 2) ? argv[2] : "ui_dump.ppm");
      return 0;
  }

  while(1) {
      /* Periodically call the lv_task handler.
       * It could be done in a timer interrupt or an OS task too.*/
      lv_timer_handler();
      usleep(5 * 1000);
  }

  return 0;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * Initialize the Hardware Abstraction Layer (HAL) for the LVGL graphics
 * library
 */static void hal_init(void)
{
  /* Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/
  sdl_init();

  /*Create a display buffer*/
  static lv_disp_draw_buf_t disp_buf1;
  static lv_color_t buf1_1[SDL_HOR_RES * 100];
  lv_disp_draw_buf_init(&disp_buf1, buf1_1, NULL, SDL_HOR_RES * 100);

  /*Create a display*/
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv); /*Basic initialization*/
  disp_drv.draw_buf = &disp_buf1;
  disp_drv.flush_cb = sdl_display_flush;
  disp_drv.hor_res = SDL_HOR_RES;
  disp_drv.ver_res = SDL_VER_RES;

  lv_disp_t * disp = lv_disp_drv_register(&disp_drv);

  lv_theme_t * th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), LV_THEME_DEFAULT_DARK, LV_FONT_DEFAULT);
  lv_disp_set_theme(disp, th);

  lv_group_t * g = lv_group_create();
  lv_group_set_default(g);

  /* Add the mouse as input device
   * Use the 'mouse' driver which reads the PC's mouse*/
  static lv_indev_drv_t indev_drv_1;
  lv_indev_drv_init(&indev_drv_1); /*Basic initialization*/
  indev_drv_1.type = LV_INDEV_TYPE_POINTER;

  /*This function will be called periodically (by the library) to get the mouse position and state*/
  indev_drv_1.read_cb = sdl_mouse_read;
  lv_indev_t *mouse_indev = lv_indev_drv_register(&indev_drv_1);

  static lv_indev_drv_t indev_drv_2;
  lv_indev_drv_init(&indev_drv_2); /*Basic initialization*/
  indev_drv_2.type = LV_INDEV_TYPE_KEYPAD;
  indev_drv_2.read_cb = sdl_keyboard_read;
  lv_indev_t *kb_indev = lv_indev_drv_register(&indev_drv_2);
  lv_indev_set_group(kb_indev, g);

  static lv_indev_drv_t indev_drv_3;
  lv_indev_drv_init(&indev_drv_3); /*Basic initialization*/
  indev_drv_3.type = LV_INDEV_TYPE_ENCODER;
  indev_drv_3.read_cb = sdl_mousewheel_read;
  lv_indev_t * enc_indev = lv_indev_drv_register(&indev_drv_3);
  lv_indev_set_group(enc_indev, g);

  /*Set a cursor for the mouse*/
  LV_IMG_DECLARE(mouse_cursor_icon); /*Declare the image file.*/
  lv_obj_t * cursor_obj = lv_img_create(lv_scr_act()); /*Create an image object for the cursor */
  lv_img_set_src(cursor_obj, &mouse_cursor_icon);           /*Set the image source*/
  lv_indev_set_cursor(mouse_indev, cursor_obj);             /*Connect the image  object to the driver*/
}

/**
 * 无窗口模式：只注册一个显示屏，flush 回调什么都不做。
 * 屏幕内容交给 dump_ppm() 里的 lv_snapshot_take() 自己渲染，所以这里不需要窗口。
 */
static void hal_init_headless(void)
{
  static lv_disp_draw_buf_t disp_buf;
  static lv_color_t buf[SDL_HOR_RES * 40];

  lv_disp_draw_buf_init(&disp_buf, buf, NULL, SDL_HOR_RES * 40);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.draw_buf = &disp_buf;
  disp_drv.flush_cb = headless_flush;
  disp_drv.hor_res = SDL_HOR_RES;
  disp_drv.ver_res = SDL_VER_RES;
  lv_disp_drv_register(&disp_drv);
}

static void headless_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
  (void)area;
  (void)color_p;
  lv_disp_flush_ready(drv);
}

/**
 * 把当前屏幕内容写成 PPM(P6)。LVGL 的快照缓冲是 32bpp 的 BGRX，
 * 这里逐像素转成 RGB 再落盘，方便转成 PNG 查看。
 *
 * 注意用 lv_snapshot_take_to_buf 而不是 lv_snapshot_take：后者从 LVGL 的内存池
 * 里要 240*320*4 = 300KB，而 LV_MEM_SIZE 只有 128KB，会直接命中 MALLOC 断言卡死。
 */
static void dump_ppm(const char *path)
{
  static uint8_t snap_buf[SDL_HOR_RES * SDL_VER_RES * 4U + 8192U];
  lv_img_dsc_t snap;

  if(lv_snapshot_take_to_buf(lv_scr_act(), LV_IMG_CF_TRUE_COLOR, &snap,
                             snap_buf, sizeof(snap_buf)) != LV_RES_OK) {
      fprintf(stderr, "snapshot failed (buffer too small?)\n");
      return;
  }

  FILE *f = fopen(path, "wb");
  if(f == NULL) {
      fprintf(stderr, "cannot open %s\n", path);
      return;
  }

  const uint32_t w = snap.header.w;
  const uint32_t h = snap.header.h;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  for(uint32_t y = 0; y < h; y++) {
      const uint8_t *row = snap.data + (size_t)y * w * 4U;
      for(uint32_t x = 0; x < w; x++) {
          const uint8_t *px = row + x * 4U;   /* B G R X */
          fputc(px[2], f);
          fputc(px[1], f);
          fputc(px[0], f);
      }
  }

  fclose(f);
  printf("dumped %ux%u -> %s\n", w, h, path);
}

static void dump_layout(lv_obj_t *parent, int depth)
{
  uint32_t n = lv_obj_get_child_cnt(parent);
  for(uint32_t i = 0; i < n; i++) {
      lv_obj_t *c = lv_obj_get_child(parent, i);
      const char *txt = lv_obj_check_type(c, &lv_label_class) ? lv_label_get_text(c) : "";
      printf("%*sx=%3d y=%3d w=%3d h=%3d |%s|\n", depth * 2 + 1, "",
             lv_obj_get_x(c), lv_obj_get_y(c),
             lv_obj_get_width(c), lv_obj_get_height(c), txt);
      dump_layout(c, depth + 1);
  }
}
