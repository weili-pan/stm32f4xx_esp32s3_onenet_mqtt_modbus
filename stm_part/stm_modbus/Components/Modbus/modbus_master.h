#ifndef __MODBUS_MASTER_H
#define __MODBUS_MASTER_H

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------- 从站配置 -------------------------------------------
 * 界面上两张卡片 = 两个 Modbus 从站（传感器 01 / 02），STM32 作为主站轮流问它们要数据。
 *
 * 寄存器约定（接的传感器不一样就改这三个宏 + mb_parse() 里的取数方式）：
 *   功能码 0x04（读输入寄存器），从 0x0000 起读 2 个：
 *     reg0 = 温度 x10，有符号（比如 265 = 26.5℃）
 *     reg1 = 湿度 x10，无符号（比如 580 = 58.0%RH）
 */

#define MB_SENSOR_COUNT        2

#define MB_SLAVE_ADDR_1        0x01U
#define MB_SLAVE_ADDR_2        0x02U

#define MB_REG_FUNC            0x04U
#define MB_REG_START           0x0000U
#define MB_REG_COUNT           2U

#define MB_BAUDRATE            9600U    /* RS485 波特率，8N1 */
#define MB_TIMEOUT_MS          200U     /* 等一帧回包的最长时间 */
#define MB_MAX_RETRY           3U       /* 连续失败几次就把该从站标离线 */
#define MB_POLL_GAP_MS         20U      /* 两次请求之间留的最小间隔 */

typedef struct {
    float temp;     /* 摄氏度 */
    float humi;     /* %RH */
    bool  online;   /* 最近一次轮询是否有正常回包 */
} mb_sensor_t;

/** 初始化 RS485 和内部状态。 */
void Modbus_Init(void);

/** 主循环里反复调用：内部是个非阻塞状态机，不会卡住 LVGL。 */
void Modbus_Poll(void);

/** 取某个从站的最新数据，idx 越界返回 NULL。返回的指针一直有效，不用释放。 */
const mb_sensor_t *Modbus_GetSensor(uint8_t idx);

#endif
