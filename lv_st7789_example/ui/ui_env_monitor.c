/**
 * @file ui_env_monitor.c
 * @brief 环境监测主界面，竖屏 240x320。
 *
 * 版式来源：download.png（原图是 320x240 横屏，屏是 240x320 竖屏），
 * 所以按竖屏重排——顶部栏照搬，两张卡片改为上下堆叠，底部状态条照搬。
 * 颜色全部是原图采样值，见下面的 COL_* 宏。
 */
#include "ui_env_monitor.h"

#include "ui_fonts.h"

/* ============================ 设计常量 ============================ */

/* 原图采样出来的配色 */
#define COL_PAGE        0x0B1220U   /* 页面底色 */
#define COL_HEADER      0x13233AU   /* 顶部栏 */
#define COL_CARD        0x14243AU   /* 卡片填充 */
#define COL_CARD_BORDER 0x294461U   /* 卡片 / 状态条描边 */
#define COL_BADGE       0x24547AU   /* 卡片里的徽标药丸 */
#define COL_DIVIDER     0x243A56U   /* 标题栏下的细分隔线 */
#define COL_FOOTER      0x101D2EU   /* 底部状态条填充 */
#define COL_ORANGE      0xFFB86BU   /* 温度 */
#define COL_CYAN        0x61D5FFU   /* 湿度 */
#define COL_DOT_ON      0x27D17FU   /* 亮着的状态圆点 */
#define COL_DOT_OFF     0x3A4A5CU   /* 灭掉的状态圆点 */
#define COL_GREEN_TXT   0x7FF0B3U   /* 「设备在线」 */
#define COL_CONN_ON     0x61B791U   /* 「已连接」 */
#define COL_CONN_OFF    0x6C7F95U
#define COL_TITLE_TXT   0xF4F7FBU
#define COL_SUBTITLE    0x849DB7U
#define COL_LABEL       0x75889EU   /* 「温度」「湿度」标签 */
#define COL_FOOTER_TXT  0x93A4B6U   /* 状态条里的灰字 */
#define COL_BADGE_TXT   0xD8E9FBU

/* 版面尺寸（屏 240x320） */
#define SCR_W           240
#define SCR_H           320
#define MARGIN_X        12
#define CONTENT_W       (SCR_W - 2 * MARGIN_X)      /* 216 */

#define HDR_H           40
#define DIV_Y           40
#define CARD_H          104
#define CARD1_Y         46
#define CARD2_Y         158
#define FOOTER_Y        272
#define FOOTER_H        36

/* 卡片内部（相对卡片左上角）*/
#define BADGE_X         8
#define BADGE_Y         6
#define BADGE_W         (CONTENT_W - 2 * BADGE_X)   /* 200 */
#define BADGE_H         24
#define ROW_H           32
#define ROW1_Y          32
#define ROW2_Y          66
#define VALUE_BOX_W     100   /* 「数字 + 单位」的定宽容器，内容在其中右对齐 */

/* ============================ 运行时句柄 ============================ */

typedef struct {
    lv_obj_t *temp;
    lv_obj_t *humi;
} card_t;

static card_t s_card[2];
static lv_obj_t *s_wifi;
static lv_obj_t *s_conn;
static lv_obj_t *s_ota_dot;
static lv_obj_t *s_online;   /* 底部「设备在线 / 设备离线」文字 */
static lv_obj_t *s_online_dot;
static lv_obj_t *s_rssi;     /* 底部「WiFi -55 dBm」 */

/* ============================ 小工具 ============================ */

/* 去掉主题给容器加的圆角/内边距/滚动条，回到「干净的矩形」再按需设样式。 */
static lv_obj_t *plain_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void fill(lv_obj_t *o, uint32_t color)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static void outline(lv_obj_t *o, uint32_t color, lv_coord_t width)
{
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(o, width, 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *dot(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, uint32_t color)
{
    lv_obj_t *d = plain_box(parent, x, y, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    fill(d, color);
    return d;
}

/* ============================ 各区块 ============================ */

static void build_header(lv_obj_t *scr)
{
    lv_obj_t *band = plain_box(scr, 0, 0, SCR_W, HDR_H);
    fill(band, COL_HEADER);

    lv_obj_t *title = label(band, "环境监测", &ui_font_bold_15, COL_TITLE_TXT);
    lv_obj_set_pos(title, 12, 2);

    lv_obj_t *sub = label(band, "STM32F407 · OneNET MQTT", &ui_font_reg_12, COL_SUBTITLE);
    lv_obj_set_pos(sub, 12, 21);

    /* 右上角：WiFi 图标 + 「已连接」。图标直接用 Montserrat 里的符号字体。 */
    s_conn = label(band, "已连接", &ui_font_reg_12, COL_CONN_ON);
    lv_obj_align(s_conn, LV_ALIGN_TOP_RIGHT, -12, 10);

    s_wifi = label(band, LV_SYMBOL_WIFI, &lv_font_montserrat_20, COL_DOT_ON);
    lv_obj_align_to(s_wifi, s_conn, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    /* 标题栏下的细分隔线 */
    lv_obj_t *line = plain_box(scr, MARGIN_X, DIV_Y, CONTENT_W, 1);
    fill(line, COL_DIVIDER);
}

static void build_card(lv_obj_t *scr, uint8_t idx, lv_coord_t y, const char *name)
{
    lv_obj_t *card = plain_box(scr, MARGIN_X, y, CONTENT_W, CARD_H);
    lv_obj_set_style_radius(card, 8, 0);
    fill(card, COL_CARD);
    outline(card, COL_CARD_BORDER, 1);

    /* 徽标药丸：传感器 01 / 传感器 02 */
    lv_obj_t *badge = plain_box(card, BADGE_X, BADGE_Y, BADGE_W, BADGE_H);
    lv_obj_set_style_radius(badge, 5, 0);
    fill(badge, COL_BADGE);
    lv_obj_t *name_lbl = label(badge, name, &ui_font_bold_15, COL_BADGE_TXT);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 10, 0);

    /* 两行读数：左标签、右侧「大号数字 + 小单位」 */
    const char *row_name[2] = {"温度", "湿度"};
    const char *row_unit[2] = {"℃", "%RH"};
    const uint32_t row_color[2] = {COL_ORANGE, COL_CYAN};
    const lv_coord_t row_y[2] = {ROW1_Y, ROW2_Y};
    const char *row_val[2] = {"--", "--"};

    /*
     * 「大号数字 + 小单位」作为一个整体靠右对齐。没有手工算 x 偏移，是因为两行
     * 单位宽度不同（℃ 12px / %RH 21px），写死偏移会让数字压到单位上；也不用
     * LV_SIZE_CONTENT，因为 flex 容器自撑宽会算窄，子对象会被裁掉左边的数字
     * （实测 26.5 只剩 6.5）。这里给固定宽度 + 主轴 END，内容自己往右靠。
     */
    for (uint8_t r = 0; r < 2; r++) {
        lv_obj_t *row = plain_box(card, 0, row_y[r], CONTENT_W, ROW_H);

        lv_obj_t *lb = label(row, row_name[r], &ui_font_reg_12, COL_LABEL);
        lv_obj_align(lb, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *grp = plain_box(row, 0, 0, VALUE_BOX_W, ROW_H);
        lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(grp, 6, 0);
        lv_obj_align(grp, LV_ALIGN_RIGHT_MID, -16, 0);

        lv_obj_t *val = label(grp, row_val[r], &ui_font_num_26, row_color[r]);
        lv_obj_t *unit = label(grp, row_unit[r], &ui_font_reg_12, row_color[r]);
        /* 单位往下压，让它的底边跟大号数字的基线对齐（原图就是这样）。
         * 需要偏移是因为两个字体在框里的落位反着：num_26 的字形贴着框底，
         * reg_12 的字形贴着框顶，直接居中对齐单位会高 6px。 */
        lv_obj_set_style_translate_y(unit, 6, 0);

        if (r == 0) { s_card[idx].temp = val; } else { s_card[idx].humi = val; }
    }
}

static void build_footer(lv_obj_t *scr)
{
    lv_obj_t *bar = plain_box(scr, MARGIN_X, FOOTER_Y, CONTENT_W, FOOTER_H);
    lv_obj_set_style_radius(bar, 8, 0);
    fill(bar, COL_FOOTER);
    outline(bar, COL_CARD_BORDER, 1);

    /*
     * 小圆点跟在文字后面（原图是「设备在线 ●   WiFi -55 dBm   OTA 就绪 ●」）。
     * 216px 比原图窄得多，所以内边距压到 6px、段间距 5px，各段 x 是按实测字宽
     * （48 / 71 / 50）排的。OTA 那段用右边距定位：RSSI 要是显示成 -100 变宽，
     * 只会挤中间的空隙，不会顶掉右边那个点。
     */
    lv_obj_t *online = label(bar, "设备在线", &ui_font_reg_12, COL_GREEN_TXT);
    lv_obj_align(online, LV_ALIGN_LEFT_MID, 6, 0);
    s_online = online;

    s_online_dot = dot(bar, 59, 14, COL_DOT_ON);

    lv_obj_t *rssi = label(bar, "WiFi -55 dBm", &ui_font_reg_12, COL_FOOTER_TXT);
    lv_obj_align(rssi, LV_ALIGN_LEFT_MID, 72, 0);
    s_rssi = rssi;

    lv_obj_t *ota = label(bar, "OTA 就绪", &ui_font_reg_12, COL_FOOTER_TXT);
    lv_obj_align(ota, LV_ALIGN_RIGHT_MID, -18, 0);

    s_ota_dot = dot(bar, 203, 14, COL_DOT_ON);
}

/* ============================ 对外 ============================ */

void ui_env_monitor_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    fill(scr, COL_PAGE);

    build_header(scr);
    build_card(scr, 0, CARD1_Y, "传感器 01");
    build_card(scr, 1, CARD2_Y, "传感器 02");
    build_footer(scr);
}

void ui_env_set_values(uint8_t idx, const char *temp, const char *humi)
{
    if (idx > 1U) { return; }
    lv_label_set_text(s_card[idx].temp, (temp != NULL) ? temp : "--");
    lv_label_set_text(s_card[idx].humi, (humi != NULL) ? humi : "--");
}

/* 顶部「已连接 / 未连接」+ WiFi 图标：文字和颜色一起切。
 * 两串都是 3 个汉字、等宽，切换不会挤动左边对齐的 WiFi 图标。 */
void ui_env_set_link(bool connected)
{
    lv_label_set_text(s_conn, connected ? "已连接" : "未连接");
    lv_obj_set_style_text_color(s_conn, lv_color_hex(connected ? COL_CONN_ON : COL_CONN_OFF), 0);
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(connected ? COL_DOT_ON : COL_DOT_OFF), 0);
}

void ui_env_set_ota_ready(bool ready)
{
    lv_obj_set_style_bg_color(s_ota_dot, lv_color_hex(ready ? COL_DOT_ON : COL_DOT_OFF), 0);
}

/* 底部「设备在线 / 设备离线」+ 圆点。
 * 两串都是 4 个汉字、等宽，所以切换时不会挤动右边的圆点和 RSSI（它们按实测字宽定死）。 */
void ui_env_set_device_online(uint8_t count)
{
    const bool on = (count > 0U);

    lv_label_set_text(s_online, on ? "设备在线" : "设备离线");
    lv_obj_set_style_text_color(s_online, lv_color_hex(on ? COL_GREEN_TXT : COL_CONN_OFF), 0);
    lv_obj_set_style_bg_color(s_online_dot, lv_color_hex(on ? COL_DOT_ON : COL_DOT_OFF), 0);
}

/* 底部「WiFi -55 dBm」。valid=0（链路没上来）显示成「WiFi -- dBm」。 */
void ui_env_set_rssi(bool valid, int dbm)
{
    if (valid)
    {
        lv_label_set_text_fmt(s_rssi, "WiFi %d dBm", dbm);
    }
    else
    {
        lv_label_set_text(s_rssi, "WiFi -- dBm");
    }
}
