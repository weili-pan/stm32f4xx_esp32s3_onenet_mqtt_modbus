#ifndef __LCD_ST7789_H
#define __LCD_ST7789_H

#include "main.h"

#define LCD_Width     240
#define LCD_Height    320

#define Direction_H          0
#define Direction_H_Flip     1
#define Direction_V          2
#define Direction_V_Flip     3

/*----------------------------------------------- SPI 总线 -------------------------------------------
 * LCD 挂在 SPI2 上，SCK / MOSI 由 Core/Src/spi.c 里的 HAL_SPI_MspInit 配好：
 *     PB10 -> SPI2_SCK
 *     PC3  -> SPI2_MOSI
 * 本组件不再自带 SPI 初始化（原来那份 hspi3 + HAL_SPI_MspInit 会和 Core/Src/spi.c 撞重复符号）。
 * ST7789 只写不读，SPI_LCD_Init() 里会把 hspi2 切换成 1LINE TX。
 */

/*----------------------------------------------- 控制引脚 -------------------------------------------
 * 下面四个引脚在 stm_modbus.ioc 里并不存在，这里是占位值。
 * 实际接线确认之后改这四个宏即可，其余代码不用动。
 */
#define LCD_CS_PORT              GPIOB
#define LCD_CS_PIN               GPIO_PIN_0     /* TODO 占位，待确认 */
#define GPIO_LCD_CS_CLK          __HAL_RCC_GPIOB_CLK_ENABLE()

#define LCD_DC_PORT              GPIOB
#define LCD_DC_PIN               GPIO_PIN_12    /* TODO 占位，待确认 */
#define GPIO_LCD_DC_CLK          __HAL_RCC_GPIOB_CLK_ENABLE()

#define LCD_RST_PORT             GPIOB
#define LCD_RST_PIN              GPIO_PIN_11    /* TODO 占位，待确认 */
#define GPIO_LCD_RST_CLK         __HAL_RCC_GPIOB_CLK_ENABLE()

#define LCD_Backlight_PORT       GPIOB
#define LCD_Backlight_PIN        GPIO_PIN_13    /* TODO 占位，待确认 */
#define GPIO_LCD_Backlight_CLK   __HAL_RCC_GPIOB_CLK_ENABLE()

/*----------------------------------------------- 控制宏 -------------------------------------------*/

#define LCD_CS_H            LCD_CS_PORT->BSRR = LCD_CS_PIN
#define LCD_CS_L            LCD_CS_PORT->BSRR = (uint32_t)LCD_CS_PIN << 16U

#define LCD_DC_Command      HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_RESET)
#define LCD_DC_Data         HAL_GPIO_WritePin(LCD_DC_PORT, LCD_DC_PIN, GPIO_PIN_SET)

#define LCD_Backlight_ON    HAL_GPIO_WritePin(LCD_Backlight_PORT, LCD_Backlight_PIN, GPIO_PIN_SET)
#define LCD_Backlight_OFF   HAL_GPIO_WritePin(LCD_Backlight_PORT, LCD_Backlight_PIN, GPIO_PIN_RESET)

/* ST7789 复位脚低有效（低 = 复位） */
#define LCD_RST_H           HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_SET)
#define LCD_RST_L           HAL_GPIO_WritePin(LCD_RST_PORT, LCD_RST_PIN, GPIO_PIN_RESET)

/*----------------------------------------------- 像素字节序 -----------------------------------------
 * 画像素时 SPI2 是 16bit 数据帧（CR1.DFF=1）且 FirstBit=MSB，DR 里的高字节会先上总线，
 * 正好是 ST7789 要的顺序，所以默认不交换。
 *
 * 如果点上去发现整屏颜色不对（比如红色变成蓝色、或者明显花屏），说明这块屏/走线按老代码
 * 需要交换高低字节，把下面改成 1 即可。
 *   0 = 不交换（按 16bit 帧 MSB first 直接发，理论正确）
 *   1 = 交换高低字节（旧版 lcd_st7789.c 里的做法）
 */
#define LCD_COLOR_SWAP_PIXEL    0

/*----------------------------------------------- 颜色顺序（RGB / BGR）----------------------------------
 * 位置：lcd_st7789.c 的 LCD_SetDirection() 里给 MADCTL(0x36) 的四个值 OR 上这个掩码。
 *
 * 现象对照：
 *   · 整屏“红↔蓝互掉、绿色不变” → 屏是 BGR 顺序，把下面改成 0x08（当前值）
 *   · 颜色恢复正常               → 保持 0x08
 *   · 若改成 0x08 后红蓝又反了   → 说明这块屏是 RGB，改回 0x00
 *   · 若整屏像“负片”（白↔黑全反）→ 那不是这里，是初始化里的 0x21(INVON)，改成 0x20
 *
 * 注意：这跟 LCD_COLOR_SWAP_PIXEL（高低字节序）是两回事。字节序错了会花屏/整体色偏，
 * 不会是干净的红蓝互换。
 *   0x00 = RGB 顺序（MADCTL bit3 = 0）
 *   0x08 = BGR 顺序（MADCTL bit3 = 1）
 */
#define LCD_MADCTL_BGR          0x08

/*----------------------------------------------- 上电自检 ------------------------------------------------
 * 置 1 时，SPI_LCD_Init() 末尾会绕过 LVGL，直接全屏依次刷 红/绿/蓝/白/黑 各 400ms。
 * 用来区分白屏的三类原因：
 *   · 颜色能依次变化          → SPI/CS/DC/RST 通路正常，问题在 LVGL 或极性
 *   · 红变成青色（红↔青互换） → 通路正常，只是 0x21(INVON) 极性反了 → 把初始化里的 0x21 改 0x20
 *   · 一直白屏                → 命令没到屏上（多半 CS/DC/RST 接线与 macros 不一致）
 * 调试完改回 0。
 */
#define LCD_BRINGUP_TEST   0

/*----------------------------------------------- API ----------------------------------------------*/

void SPI_LCD_Init(void);
void LCD_Clear(void);
void LCD_SetAddress(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p);
void LCD_FillScreen(uint16_t color);
void LCD_BringUpTest(void);

#endif
