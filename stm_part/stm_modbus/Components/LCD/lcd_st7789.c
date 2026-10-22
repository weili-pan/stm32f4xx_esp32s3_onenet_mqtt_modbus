#include "lcd_st7789.h"
#include "spi.h"

/* LCD 挂在 SPI2 上（PB10=SCK / PC3=MOSI），句柄由 Core/Src/spi.c 提供。
 * 这里直接用全局句柄，不再自己定义 hspi3，也不再实现 HAL_SPI_MspInit
 * （否则和 Core/Src/spi.c 里的同名函数撞重复符号，链不过）。 */
#define LCD_SPI hspi2

struct {
    uint16_t Width;
    uint16_t Height;
    uint8_t  X_Offset;
    uint8_t  Y_Offset;
} LCD;

/*----------------------------------------------- 静态函数声明 -------------------------------------------*/

static void LCD_GPIO_Init(void);
static void LCD_SPI_ConfigTxOnly(void);
static void LCD_Reset(void);
static void LCD_WriteCommand(uint8_t cmd);
static void LCD_WriteData_8bit(uint8_t data);
static void LCD_WriteData_16bit(uint16_t data);
static void LCD_SetDirection(uint8_t direction);

/*----------------------------------------------- GPIO 初始化 -------------------------------------------
 * 只配 LCD 自己的控制脚。SCK / MOSI 归 HAL_SPI_MspInit（Core/Src/spi.c）管，这里不碰。
 */
static void LCD_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_LCD_CS_CLK;
    GPIO_LCD_DC_CLK;
    GPIO_LCD_RST_CLK;
    GPIO_LCD_Backlight_CLK;

    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

    GPIO_InitStruct.Pin = LCD_CS_PIN;
    HAL_GPIO_Init(LCD_CS_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = LCD_DC_PIN;
    HAL_GPIO_Init(LCD_DC_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = LCD_RST_PIN;
    HAL_GPIO_Init(LCD_RST_PORT, &GPIO_InitStruct);

    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Pin   = LCD_Backlight_PIN;
    HAL_GPIO_Init(LCD_Backlight_PORT, &GPIO_InitStruct);

    LCD_DC_Data;
    LCD_CS_H;
    LCD_RST_H;
    LCD_Backlight_OFF;
}

/*----------------------------------------------- SPI 切成只发 -------------------------------------------
 * ST7789 只需要 MOSI 方向。SPI2 由 CubeMX 生成的是 2LINES，这里改成 1LINE(TX)，
 * 免得 RX 侧一直溢出。HAL_SPI_Init 第二次调用不会再跑 MspInit（State 已不是 RESET），
 * 只按新的 Direction 重写 CR1，所以 PB10/PC3 不会被重新初始化。
 */
static void LCD_SPI_ConfigTxOnly(void)
{
    LCD_SPI.Init.Direction = SPI_DIRECTION_1LINE;
    if (HAL_SPI_Init(&LCD_SPI) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_SPI_ENABLE(&LCD_SPI);
    SPI_1LINE_TX(&LCD_SPI);
}

/*----------------------------------------------- 硬复位 -------------------------------------------
 * ESP-IDF 那份是 panel_config.reset_gpio_num，由 esp_lcd_panel_reset() 驱动复位脚。
 * 这里手动做同样的时序：拉高 → 10ms → 拉低 ≥10ms → 拉高 → 等 ≥120ms 才能发命令。
 * 注意要在 LCD_GPIO_Init() 之后调用（RST 脚得先配成输出）。
 */
static void LCD_Reset(void)
{
    LCD_RST_H;
    HAL_Delay(10);
    LCD_RST_L;
    HAL_Delay(10);
    LCD_RST_H;
    HAL_Delay(120);
}

/*----------------------------------------------- 内部 SPI 写函数 -------------------------------------------*/

static void LCD_WriteCommand(uint8_t lcd_command)
{
    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_DC_Command;
    LCD_SPI.Instance->DR = lcd_command;
    while((LCD_SPI.Instance->SR & 0x0002) == 0);
    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_DC_Data;
}

static void LCD_WriteData_8bit(uint8_t lcd_data)
{
    LCD_SPI.Instance->DR = lcd_data;
    while((LCD_SPI.Instance->SR & 0x0002) == 0);
}

/* 8bit 数据帧下把一个 16bit 值拆成两次发（坐标用） */
static void LCD_WriteData_16bit(uint16_t lcd_data)
{
    LCD_SPI.Instance->DR = lcd_data >> 8;
    while((LCD_SPI.Instance->SR & 0x0002) == 0);
    LCD_SPI.Instance->DR = lcd_data;
    while((LCD_SPI.Instance->SR & 0x0002) == 0);
}

/*----------------------------------------------- 显示方向 -------------------------------------------*/

static void LCD_SetDirection(uint8_t direction)
{
    LCD_CS_L;

    if(direction == Direction_H)
    {
        LCD_WriteCommand(0x36);
        LCD_WriteData_8bit(0x70 | LCD_MADCTL_BGR);
        LCD.X_Offset = 0;
        LCD.Y_Offset = 0;
        LCD.Width    = LCD_Height;
        LCD.Height   = LCD_Width;
    }
    else if(direction == Direction_V)
    {
        LCD_WriteCommand(0x36);
        LCD_WriteData_8bit(0x00 | LCD_MADCTL_BGR);
        LCD.X_Offset = 0;
        LCD.Y_Offset = 0;
        LCD.Width    = LCD_Width;
        LCD.Height   = LCD_Height;
    }
    else if(direction == Direction_H_Flip)
    {
        LCD_WriteCommand(0x36);
        LCD_WriteData_8bit(0xA0 | LCD_MADCTL_BGR);
        LCD.X_Offset = 80;
        LCD.Y_Offset = 0;
        LCD.Width    = LCD_Height;
        LCD.Height   = LCD_Width;
    }
    else if(direction == Direction_V_Flip)
    {
        LCD_WriteCommand(0x36);
        LCD_WriteData_8bit(0xC0 | LCD_MADCTL_BGR);
        LCD.X_Offset = 0;
        LCD.Y_Offset = 80;
        LCD.Width    = LCD_Width;
        LCD.Height   = LCD_Height;
    }

    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;
}

/*----------------------------------------------- LCD 初始化 -------------------------------------------*/

void SPI_LCD_Init(void)
{
    LCD_GPIO_Init();
    LCD_SPI_ConfigTxOnly();
    LCD_Reset();   /* 硬复位，等屏就绪再发命令（取代原先的裸 HAL_Delay(10)） */

    LCD_CS_L;

    LCD_WriteCommand(0x36);
    LCD_WriteData_8bit(0x00 | LCD_MADCTL_BGR);

    LCD_WriteCommand(0x3A);
    LCD_WriteData_8bit(0x05);

    LCD_WriteCommand(0xB2);
    LCD_WriteData_8bit(0x0C);
    LCD_WriteData_8bit(0x0C);
    LCD_WriteData_8bit(0x00);
    LCD_WriteData_8bit(0x33);
    LCD_WriteData_8bit(0x33);

    LCD_WriteCommand(0xB7);
    LCD_WriteData_8bit(0x35);

    LCD_WriteCommand(0xBB);
    LCD_WriteData_8bit(0x19);

    LCD_WriteCommand(0xC0);
    LCD_WriteData_8bit(0x2C);

    LCD_WriteCommand(0xC2);
    LCD_WriteData_8bit(0x01);

    LCD_WriteCommand(0xC3);
    LCD_WriteData_8bit(0x12);

    LCD_WriteCommand(0xC4);
    LCD_WriteData_8bit(0x20);

    LCD_WriteCommand(0xC6);
    LCD_WriteData_8bit(0x0F);

    LCD_WriteCommand(0xD0);
    LCD_WriteData_8bit(0xA4);
    LCD_WriteData_8bit(0xA1);

    LCD_WriteCommand(0xE0);
    LCD_WriteData_8bit(0xD0);
    LCD_WriteData_8bit(0x04);
    LCD_WriteData_8bit(0x0D);
    LCD_WriteData_8bit(0x11);
    LCD_WriteData_8bit(0x13);
    LCD_WriteData_8bit(0x2B);
    LCD_WriteData_8bit(0x3F);
    LCD_WriteData_8bit(0x54);
    LCD_WriteData_8bit(0x4C);
    LCD_WriteData_8bit(0x18);
    LCD_WriteData_8bit(0x0D);
    LCD_WriteData_8bit(0x0B);
    LCD_WriteData_8bit(0x1F);
    LCD_WriteData_8bit(0x23);

    LCD_WriteCommand(0xE1);
    LCD_WriteData_8bit(0xD0);
    LCD_WriteData_8bit(0x04);
    LCD_WriteData_8bit(0x0C);
    LCD_WriteData_8bit(0x11);
    LCD_WriteData_8bit(0x13);
    LCD_WriteData_8bit(0x2C);
    LCD_WriteData_8bit(0x3F);
    LCD_WriteData_8bit(0x44);
    LCD_WriteData_8bit(0x51);
    LCD_WriteData_8bit(0x2F);
    LCD_WriteData_8bit(0x1F);
    LCD_WriteData_8bit(0x1F);
    LCD_WriteData_8bit(0x20);
    LCD_WriteData_8bit(0x23);

    LCD_WriteCommand(0x21);

    LCD_WriteCommand(0x11);
    HAL_Delay(120);

    LCD_WriteCommand(0x29);

    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;

    LCD_SetDirection(Direction_V);
    LCD_Clear();
    LCD_Backlight_ON;

#if LCD_BRINGUP_TEST
    LCD_BringUpTest();   /* 白屏排查用：直接刷颜色，绕过 LVGL。调好把 lcd_st7789.h 里的宏改回 0 */
#endif
}

/*----------------------------------------------- 设置显示窗口 -------------------------------------------*/

void LCD_SetAddress(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_CS_L;

    LCD_WriteCommand(0x2a);
    LCD_WriteData_16bit(x1 + LCD.X_Offset);
    LCD_WriteData_16bit(x2 + LCD.X_Offset);

    LCD_WriteCommand(0x2b);
    LCD_WriteData_16bit(y1 + LCD.Y_Offset);
    LCD_WriteData_16bit(y2 + LCD.Y_Offset);

    LCD_WriteCommand(0x2c);

    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;
}

/*----------------------------------------------- 清屏 -------------------------------------------*/

void LCD_Clear(void)
{
    uint32_t i;
    uint32_t size = (uint32_t)LCD.Width * LCD.Height;

    LCD_SetAddress(0, 0, LCD.Width - 1, LCD.Height - 1);

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 |= 0x0800;
    LCD_SPI.Instance->CR1 |= 0x0040;

    LCD_CS_L;

    for(i = 0; i < size; i++)
    {
        LCD_SPI.Instance->DR = 0x0000;
        while((LCD_SPI.Instance->SR & 0x0002) == 0);
    }
    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 &= 0xF7FF;
    LCD_SPI.Instance->CR1 |= 0x0040;
}

/*----------------------------------------------- 区域像素填充（供 LVGL 调用）-------------------------------------------*/

void LCD_Fill(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t *color_p)
{
    uint32_t i;
    uint32_t size = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);

    LCD_SetAddress(x1, y1, x2, y2);

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 |= 0x0800;
    LCD_SPI.Instance->CR1 |= 0x0040;

    LCD_CS_L;

    for(i = 0; i < size; i++)
    {
        uint16_t pixel = color_p[i];
#if LCD_COLOR_SWAP_PIXEL
        pixel = (uint16_t)((pixel >> 8) | (pixel << 8));
#endif
        LCD_SPI.Instance->DR = pixel;
        while((LCD_SPI.Instance->SR & 0x0002) == 0);
    }
    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 &= 0xF7FF;
    LCD_SPI.Instance->CR1 |= 0x0040;
}

/*----------------------------------------------- 全屏单色（自检用）-----------------------------------------*/

void LCD_FillScreen(uint16_t color)
{
    uint32_t i;
    uint32_t size = (uint32_t)LCD.Width * LCD.Height;

    LCD_SetAddress(0, 0, LCD.Width - 1, LCD.Height - 1);

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 |= 0x0800;
    LCD_SPI.Instance->CR1 |= 0x0040;

    LCD_CS_L;

    for(i = 0; i < size; i++)
    {
        LCD_SPI.Instance->DR = color;
        while((LCD_SPI.Instance->SR & 0x0002) == 0);
    }
    while((LCD_SPI.Instance->SR & 0x0080) != RESET);
    LCD_CS_H;

    LCD_SPI.Instance->CR1 &= 0xFFBF;
    LCD_SPI.Instance->CR1 &= 0xF7FF;
    LCD_SPI.Instance->CR1 |= 0x0040;
}

/*----------------------------------------------- 上电自检 -------------------------------------------*/

void LCD_BringUpTest(void)
{
    static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 }; /* 红 绿 蓝 白 黑 */
    uint32_t k;

    for(k = 0; k < sizeof(colors) / sizeof(colors[0]); k++)
    {
        LCD_FillScreen(colors[k]);
        HAL_Delay(400);
    }
}
