/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "i2c.h"
#include "rtc.h"
#include "spi.h"
#include "usart.h"
#include "wwdg.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "lvgl.h"
#include "lv_port_disp.h"
#include "ui_env_monitor.h"
#include "modbus_master.h"
#include "esp_link.h"
#include "key.h"
#include "led.h"
#include "lcd_st7789.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/*
 * WWDG 这一版不能用：CubeMX 生成的是 Prescaler=DIV1 / Window=64 / Counter=64，
 * PCLK1=42MHz 时计数时钟 42e6/4096 = 10.25kHz，
 * 而 Window=64 意味着刷新窗口只有一个计数周期 —— 也就是 ~97us 内必须喂一次狗，
 * 否则立刻复位。所以下面的 MX_WWDG_Init() 被注释掉了（不初始化就不会开 WWDG 时钟，
 * 看门狗处于停用状态）。
 *
 * 想真正用起来的话：在 CubeMX 里把 WWDG 从 stm_modbus.ioc 里删掉，或者重新配成
 * Prescaler=DIV8 + 合理的 Window（约 25ms 刷新的窗口期）再在主循环里 HAL_WWDG_Refresh()。
 */
#define UI_REFRESH_MS   500U    /* 界面读数刷新的间隔 */
#define KEY_DEBOUNCE_MS 20U     /* 按键去抖：两次同电平的间隔 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static uint32_t s_ui_tick;
static bool     s_backlight_on  = true;   /* 背光状态（初始亮） */
static uint32_t s_key_tick;               /* 上次采样 PA15 的时刻 */
static bool     s_key_last;               /* 上次采到的电平（KEY_ON=按下） */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* 把 Modbus 主站收到的温湿度刷到两张大卡片上。
 * 用 lv_snprintf 而不是 libc 的 snprintf：LVGL 自带的格式化不依赖浮点 printf，
 * 而本次只用到 %d，正好避开 nano.specs 下要额外加 -u _printf_float 的麻烦。 */
static void ui_update_from_modbus(void)
{
    char tbuf[16];
    char hbuf[16];

    for (uint8_t i = 0U; i < MB_SENSOR_COUNT; i++)
    {
        const mb_sensor_t *s = Modbus_GetSensor(i);

        if ((s == NULL) || (!s->online))
        {
            ui_env_set_values(i, NULL, NULL);       /* 显示成 "--" */
            continue;
        }

        int t10 = (int)(s->temp * 10.0f + ((s->temp >= 0.0f) ? 0.5f : -0.5f));
        int h10 = (int)(s->humi * 10.0f + 0.5f);

        if (t10 < 0)
        {
            t10 = -t10;
            lv_snprintf(tbuf, sizeof tbuf, "-%d.%d", t10 / 10, t10 % 10);
        }
        else
        {
            lv_snprintf(tbuf, sizeof tbuf, "%d.%d", t10 / 10, t10 % 10);
        }
        lv_snprintf(hbuf, sizeof hbuf, "%d", h10 / 10);

        ui_env_set_values(i, tbuf, hbuf);
    }
}

/*
 * 板上 KEY（PA15，低有效）非阻塞去抖：每 KEY_DEBOUNCE_MS 采一次，连续两次电平一致
 * 才认一次边沿。检测到「按下（下降沿）」就翻转背光。
 * 只关背光，不动 LVGL/Modbus —— 所以关屏后界面照样在后台更新，再按一下立刻能看。
 */
static void screen_key_poll(void)
{
    if ((HAL_GetTick() - s_key_tick) < KEY_DEBOUNCE_MS)
    {
        return;
    }
    s_key_tick = HAL_GetTick();

    const bool pressed = (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN) == GPIO_PIN_RESET);

    if (pressed && !s_key_last)     /* 下降沿：按下瞬间 */
    {
        s_backlight_on = !s_backlight_on;
        if (s_backlight_on) { LCD_Backlight_ON; }
        else                { LCD_Backlight_OFF; }
    }
    s_key_last = pressed;
}

/* 板载 LED1(PC13，低电平点亮)。
 * 不用 led.h 的 LED1_ON —— 那个宏走的是 F4 上不存在的 BRR 寄存器（厂商库对 F4 的笔误，
 * 因为从没被调用过所以一直没编译暴露），这里直接走 HAL。 */
static void link_led(bool on)
{
    HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

/*
 * 把 ESP32 经 USART1 发来的链接状态刷到界面。
 * 链路在线时用真实值；超时（EspLink_GetStatus 返回 false）则回落成「未连接 / --」，
 * 免得 ESP32 掉线后界面还一直显示旧数据。
 *
 * 顺便驱动 LED1：亮 = 收到 ESP32 的状态帧，灭 = 超时没收到。
 * 这样万一界面没动，一眼就能分清是「帧没到 STM32」还是「帧到了但界面没刷」。
 */
static void ui_update_from_link(void)
{
    esp_link_status_t st;
    const bool up = EspLink_GetStatus(&st);

    link_led(up);

    if (!up)
    {
        ui_env_set_link(false);
        ui_env_set_rssi(false, 0);
        ui_env_set_device_online(0);     /* ESP 掉线 → 「设备在线」也回落成离线 */
        ui_env_set_ota_ready(false);
        return;
    }

    ui_env_set_link(st.wifi != 0U);
    ui_env_set_rssi(st.wifi != 0U, st.rssi);
    ui_env_set_device_online(st.devices);
    ui_env_set_ota_ready(st.ota_ready != 0U);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  MX_RTC_Init();
  MX_SPI1_Init();
  MX_SPI2_Init();
  MX_USART1_UART_Init();
  /* MX_WWDG_Init(); */   /* 见 USER CODE BEGIN PD 里的说明：这一版的 WWDG 参数 97us 就会复位，不能开 */
  /* USER CODE BEGIN 2 */

  /* LVGL：先建驱动，再建界面。lv_port_disp_init() 里会 SPI_LCD_Init() 把屏点起来，
   * LVGL 的 tick 直接取 HAL_GetTick()（lv_conf.h 里 LV_TICK_CUSTOM=1），不用喂 lv_tick_inc()。 */
  lv_init();
  lv_port_disp_init();
  ui_env_monitor_create();

  Modbus_Init();          /* 内部会配好 USART2 + DE 引脚并开 RX 中断 */

  /* 板上 KEY(PA15) 关/开背光；LED1(PC13) 当 ESP32 链路指示灯 */
  KEY_Init();
  LED_Init();
  link_led(false);        /* 上电先灭，等第一帧状态再点亮 */
  s_key_last = (HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN) == GPIO_PIN_RESET);
  s_key_tick = HAL_GetTick();
  EspLink_Init();

  ui_update_from_link();  /* 先按「未连接」刷一次，避免上电瞬间显示默认亮灯 */

  s_ui_tick = HAL_GetTick();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    lv_timer_handler();     /* 渲染 + 刷屏（LCD_Fill 是阻塞的，整屏约几十 ms） */
    Modbus_Poll();          /* 非阻塞状态机，不会卡住上面那个 */
    EspLink_Poll();         /* 非阻塞帧解析，收 ESP32 的状态帧 */
    screen_key_poll();      /* 非阻塞按键去抖，切换背光 */

    if ((HAL_GetTick() - s_ui_tick) >= UI_REFRESH_MS)
    {
      s_ui_tick = HAL_GetTick();
      ui_update_from_modbus();
      ui_update_from_link();
    }

    HAL_Delay(5);
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM1 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM1)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
