#ifndef __RS485_H
#define __RS485_H

#include "main.h"

/*----------------------------------------------- 硬件连接 -------------------------------------------
 * USART2 没有出现在 stm_modbus.ioc 里，所以这一份代码自己把时钟、GPIO、UART 全配了，
 * 不依赖 CubeMX 生成的 usart.c（那里只有 USART1）。
 *
 * 两种 485 收发器都支持，用 RS485_DE_ENABLE 选：
 *   1 = 有独立 DE/RE 控制脚的收发器（MAX485/MAX3485 一类）
 *         PA2(USART2_TX) -> DI,  PA3(USART2_RX) <- RO,  PD4 -> DE/RE
 *   0 = 自动换向模块（TTL 侧只有 TX/RX/GND/VCC 四根线，没有 DE/RE）
 *         PA2(USART2_TX) -> 模块 RXD,  PA3(USART2_RX) <- 模块 TXD,
 *         GND 共地, VCC 供电
 *       方向由模块自己按 TX 数据切换，MCU 不用控制任何脚。
 */
#define RS485_DE_ENABLE                0

#define RS485_UART                     USART2
#define RS485_UART_IRQn                USART2_IRQn
#define RS485_UART_CLK_ENABLE()        __HAL_RCC_USART2_CLK_ENABLE()

#define RS485_GPIO_PORT                GPIOA
#define RS485_GPIO_CLK_ENABLE()        __HAL_RCC_GPIOA_CLK_ENABLE()
#define RS485_TX_PIN                   GPIO_PIN_2      /* USART2_TX -> 收发器 DI / 模块 RXD */
#define RS485_RX_PIN                   GPIO_PIN_3      /* USART2_RX <- 收发器 RO / 模块 TXD */
#define RS485_GPIO_AF                  GPIO_AF7_USART2

#define RS485_DE_PORT                  GPIOD
#define RS485_DE_PIN                   GPIO_PIN_4      /* 仅 RS485_DE_ENABLE=1 时使用 */
#define RS485_DE_CLK_ENABLE()          __HAL_RCC_GPIOD_CLK_ENABLE()

/*----------------------------------------------- 帧参数 -------------------------------------------
 * Modbus RTU 靠「3.5 个字符时间」的静默间隔分帧。
 * 9600 8N1 下一个字符 11bit，3.5 字符 ≈ 4.0ms，所以取 4ms。
 * 若以后把波特率提到 19200 以上，这个值可以降到 2。
 */
#define RS485_FRAME_GAP_MS             4U
#define RS485_TX_TIMEOUT_MS            100U
#define RS485_RX_RING_SIZE             256U    /* 必须是 2 的幂，便于取模 */

/*----------------------------------------------- API ----------------------------------------------*/

/** 配好 USART2（以及可选 DE 引脚），并打开 RX 中断。 */
void RS485_Init(uint32_t baudrate);

/** 阻塞发送一串字节（DE_ENABLE=1 时自动切换方向，发完等移位结束再收回）。 */
HAL_StatusTypeDef RS485_Send(const uint8_t *buf, uint16_t len);

/** 丢掉接收缓冲里的所有字节（发请求前调一下，免得读到上一轮的残包）。 */
void RS485_FlushRx(void);

/**
 * 取一帧完整的回包。
 * 只有「收到过字节」且「距最后一个字节已经静默 RS485_FRAME_GAP_MS 以上」时才返回；
 * 否则返回 0。out 至少要能放 max 字节，超长的部分会被丢掉。
 * @return 实际取到的字节数，0 表示还没有完整帧。
 */
uint16_t RS485_GetFrame(uint8_t *out, uint16_t max);

#endif
