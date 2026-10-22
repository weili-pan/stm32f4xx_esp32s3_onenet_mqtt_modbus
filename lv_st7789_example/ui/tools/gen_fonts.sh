#!/usr/bin/env bash
# 生成 ui/fonts/ 下的三个中文点阵字体。
#
# 为什么不用内置的 lv_font_simsun_16_cjk：它只有 1166 个常用字，本项目界面要用的
# 「环 监 测 传 器 湿 设 备 线 连 绪」等 11 个字它没有，会在屏幕上显示成空白。
#
# 依赖：npm install -g lv_font_conv（本机已装，v1.5.3）
# 用法：bash ui/tools/gen_fonts.sh   （在 lv_st7789_example/ 目录下执行）

set -euo pipefail

export PATH="/d/nodejs:$PATH"

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/fonts"
FONTS="/c/Windows/Fonts"

# ---- 界面里会用到的字符（改了界面文案就要把新字加进来） ----
# 必需：环 境 监 测 传 感 器 温 度 湿 设 备 在 线 已 连 接 就 绪 未 离
# 预留：无 有 正 常 错 误 报 警 升 级 固 件 待 机 关 断 号 数 体 屏 幕 管 理 级
CJK="0x4EF6,0x4F20,0x4F53,0x5173,0x5347,0x53F7,0x56FA,0x5668,0x5728,0x5883,\
0x5907,0x5C31,0x5C4F,0x5DF2,0x5E38,0x5E55,0x5EA6,0x5F85,0x611F,0x62A5,\
0x63A5,0x6570,0x65AD,0x65E0,0x6709,0x672A,0x673A,0x6B63,0x6D4B,0x6E29,\
0x6E7F,0x73AF,0x7406,0x76D1,0x79BB,0x7BA1,0x7EA7,0x7EBF,0x7EEA,0x8BBE,\
0x8B66,0x8BEF,0x8FDE,0x9519"

# ASCII 可见字符 + 不换行空格不用 / 「·」U+00B7 分隔点 / 「℃」U+2103 摄氏度
TEXT="0x20-0x7E,0xB7,0x2103"

# 大号数字只需要 空格 - . 0-9 % R H
NUM="0x20,0x25,0x2D,0x2E,0x30-0x39,0x48,0x52"

gen() {  # gen <字号> <bpp> <源字体> <符号范围> <字体名> <输出文件>
  echo ">> $5  (size=$1 bpp=$2)"
  lv_font_conv \
    --font "$3" \
    --size "$1" --bpp "$2" \
    -r "$4" \
    --format lvgl \
    --lv-include lvgl.h \
    --lv-font-name "$5" \
    -o "$OUT/$6" \
    --force-fast-kern-format
}

# 标题 + 卡片徽标：等线 Bold
gen 15 4 "$FONTS/Dengb.ttf" "$TEXT,$CJK" ui_font_bold_15 ui_font_bold_15.c
# 正文：标签、页脚、单位。等线 Regular
gen 12 4 "$FONTS/Deng.ttf"  "$TEXT,$CJK" ui_font_reg_12  ui_font_reg_12.c
# 大号读数数字：等线 Bold
gen 26 4 "$FONTS/Dengb.ttf" "$NUM"       ui_font_num_26  ui_font_num_26.c

echo "OK -> $OUT"
ls -l "$OUT"
