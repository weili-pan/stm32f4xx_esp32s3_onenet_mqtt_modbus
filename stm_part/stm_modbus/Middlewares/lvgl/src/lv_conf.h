/*
 * 这个文件原本是 lv_conf_template 的一份副本（所有字体都关着）。
 * 麻烦在于 src/lv_conf_internal.h 里那句 __has_include("lv_conf.h") 会先命中「同目录」的
 * 这一份，于是 Middlewares/lvgl/lv_conf.h（那份真正配好的）永远轮不到。
 *
 * 这里直接转过去，保证工程里只有一份生效的配置。真正的配置在上一层：
 *   Middlewares/lvgl/lv_conf.h
 */
#include "../lv_conf.h"
