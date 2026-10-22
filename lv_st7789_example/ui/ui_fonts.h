/**
 * @file ui_fonts.h
 * @brief 界面用的三个中文字体（由 ui/tools/gen_fonts.sh 生成）。
 *
 * 为什么不用 LVGL 内置的 lv_font_simsun_16_cjk：它只覆盖 1166 个常用字，
 * 「环 监 测 传 器 湿 设 备 线 连 绪」这些本界面要用的字它没有，会显示成空白。
 */
#ifndef UI_FONTS_H
#define UI_FONTS_H

#include "lvgl.h"

LV_FONT_DECLARE(ui_font_bold_15);   /* 标题「环境监测」、卡片徽标 */
LV_FONT_DECLARE(ui_font_reg_12);    /* 副标题、标签、页脚、单位 */
LV_FONT_DECLARE(ui_font_num_26);    /* 大号读数数字 */

#endif /* UI_FONTS_H */
