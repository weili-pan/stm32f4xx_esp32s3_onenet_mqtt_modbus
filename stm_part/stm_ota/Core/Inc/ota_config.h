/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota_config.h
  * @brief   stm_ota Bootloader 的全部可调参数与引脚占位宏。
  *
  *          集中放这里的原因：Bootloader 的引脚/超时/地址散在多个 .c 里时，
  *          换板子要满仓库找。这个文件是唯一需要改的地方。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __OTA_CONFIG_H
#define __OTA_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ==========================================================================
 * 一、W25Q64 外部 SPI Flash 接在哪
 * ==========================================================================
 *
 * 接线：W25Q64 挂在 SPI1 —— PA5=SCK、PA6=MISO、PA7=MOSI，CS 用 PB1（软件控制）。
 * SPI2（PB10=SCK、PC3=MOSI，没有 MISO）是留给 ST7789 的写-only 总线。
 *
 * CubeMX 里 SPI1 的 MSP 只配了 SCK(PA5) 和 MOSI(PA7)——MISO 和 CS 是这里
 * 自己配的，所以下面这几个宏必须和实际接线一致。
 *
 * 注意 PA6 在 F407 上只对应 SPI1_MISO，SPI2 的 MISO 只能是 PB14/PC2；
 * 换总线时这三个值（句柄/引脚/AF）必须一起改。
 */

/* 用哪个 SPI 外设。W25Q64 在 SPI1 上。 */
#define W25Q64_SPI_HANDLE        hspi1

/* MISO 引脚（PA6 = SPI1_MISO） */
#define W25Q64_MISO_PORT         GPIOA
#define W25Q64_MISO_PIN          GPIO_PIN_6

/* CS 引脚（软件控制，推挽输出，空闲高）。 */
#define W25Q64_CS_PORT           GPIOB
#define W25Q64_CS_PIN            GPIO_PIN_1

/* 上面两个引脚所在的 GPIO 端口时钟使能宏。换端口时一起改。 */
#define W25Q64_MISO_CLK_ENABLE()  __HAL_RCC_GPIOA_CLK_ENABLE()
#define W25Q64_CS_CLK_ENABLE()    __HAL_RCC_GPIOB_CLK_ENABLE()

/* MISO 的复用功能编号。SPI1/SPI2 在 F407 上都是 AF5；若改用 SPI3 要改成 AF6。 */
#define W25Q64_MISO_AF           GPIO_AF5_SPI1

/* 单次 HAL SPI 调用的超时（ms）。4KB 读在 42MHz 下约 0.8ms、21MHz 下约 1.6ms，
 * 4KB 页编程约 3ms（芯片 tPP 典型值），所以 100ms 有充足余量，
 * 同时能在芯片掉线时及时退出。 */
#define W25Q64_SPI_TIMEOUT_MS    100U

/* JEDEC ID 期望值，用于自检「SPI 通了且芯片在位」。
 * W25Q64 = 0xEF4017，W25Q32 = 0xEF4016。 */
#define W25Q64_JEDEC_ID          0x00EF4017U

/* 等 W25Q64 内部擦/写完成（轮询状态寄存器 WIP 位）的超时（ms）。
 * 规格书给的最坏值：4KB 扇区擦除 400ms、256B 页编程 3ms。 */
#define W25Q64_BUSY_TIMEOUT_MS   3000U

/* ★ 引脚确认开关 ★
 * 上面 MISO/CS 几个宏已按实际接线（SPI1 + PB1）确认过，所以这里放开。
 * 如果之后改接线，先把这行注释掉，编译期会重新提醒。 */
#define W25Q64_PINS_CONFIRMED

/* 扇区大小（4KB）。W25Q64 支持 4KB 扇区擦除指令 0x20。 */
#define W25Q64_SECTOR_SIZE       4096U
#define W25Q64_PAGE_SIZE         256U
#define W25Q64_CAPACITY          (8U * 1024U * 1024U)

/* ==========================================================================
 * 二、W25Q64 里的存储布局
 * ==========================================================================
 *
 *   偏移 0x000000（扇区 0）: OTA 头
 *   偏移 0x001000 起       : 固件原始二进制（与云端 .bin 逐字节一致）
 */
#define W25Q64_OTA_HDR_ADDR      0x000000U
#define W25Q64_OTA_FW_ADDR       0x001000U   /* 固件起始偏移 */

/* ==========================================================================
 * 三、STM32 内部 Flash 分区
 * ==========================================================================
 *
 * STM32F407ZG 的 1MB Flash 扇区分布：
 *   扇区 0..3 : 0x08000000 起，各 16KB   -> 共 64KB，给 Bootloader
 *   扇区 4    : 0x08010000，64KB
 *   扇区 5..11: 0x08020000 起，各 128KB  -> 共 960KB，给 App
 *
 * App 区从这里开始。App 自己的链接脚本 ORIGIN 必须也是 0x08010000，
 * 并在启动时把 SCB->VTOR 设成这个值。
 */
#define OTA_APP_ADDR             0x08010000U
#define OTA_APP_SIZE             (960U * 1024U)
#define OTA_APP_MAX_SIZE         OTA_APP_SIZE       /* 别名，读代码时更顺 */
#define OTA_FLASH_END            (OTA_APP_ADDR + OTA_APP_SIZE)  /* 0x080F0000 */

/* ==========================================================================
 * 四、进下载模式的判定与时序
 * ========================================================================== */

/* 把这个魔数写进 RTC->BKP0R 再复位，Bootloader 上电就进下载模式。
 * 用备份寄存器而不是 Flash 扇区：不占空间、不需要擦除、复位后仍在。 */
#define OTA_BOOT_MAGIC           0x4F544131U   /* "OTA1" */

/* 进下载模式后，等 OTA_START 的总时长（ms）。超时就放弃、跳回 App。
 * 期间每 OTA_READY_REPEAT_MS 重发一次 ready 状态给 ESP32。
 *
 * 不要调小：每次 ready 都会被 ESP32 原样转发到 OneNET，而 OneNET 对属性上报有
 * 频率限制（同一属性 1 秒内多次上报会被限流/丢弃），500ms 一次会刷屏。5s 足够让
 * ESP32 侧知道 STM32 已经就绪（它本来就会同时重试 OTA_START）。 */
#define OTA_WAIT_START_MS        30000U
#define OTA_READY_REPEAT_MS      5000U

/* 两片之间的最大间隔（ms）。超了判定链路断开，丢弃本次半成品回到等待态。 */
#define OTA_DATA_GAP_TIMEOUT_MS  3000U

/* 台架调试用：>0 表示每次上电都先监听这么久（ms），不管有没有魔数。
 * 出货固件必须保持 0，否则白白等这段时间。 */
#define OTA_BOOT_WAIT_MS         0U

/* ==========================================================================
 * 五、传输缓冲
 * ========================================================================== */

/* 收到一个分片就立刻 ACK（不等落盘），然后攒够 OTA_STAGING_SIZE 才写 W25Q64
 * 一次。这样擦写耗时藏在「等下一片」的空档里，不会拖慢 ACK。
 * ESP32 侧 ACK 超时默认 500ms，务必保证 staging 大小是分片大小的整数倍，
 * 否则会出现「先擦后 ACK」的慢路径（代码里有兜底，但没必要走）。 */
#define OTA_STAGING_SIZE         (4U * 1024U)

/* 单帧 payload 上限。ESP32 侧是 CHUNK_SIZE + 64，CHUNK_SIZE 默认 1024、
 * 上限 4096，所以取 4096+64 = 4160 就足够；这里再留一点余量。 */
#define OTA_MAX_PAYLOAD          (4096U + 64U)

/* 串口接收：DMA 环形缓冲（硬件）+ 软件环形缓冲（ISR 搬到主循环用）。
 * DMA 缓冲不参与「攒够一帧」的逻辑，只是给软件环做搬运源，所以两倍于
 * 软件环即可。软件环要能装下主循环一次停顿期间到达的最多字节数：
 * 主循环最长停顿是 W25Q64 扇区擦除（最坏 400ms），但那时 ESP32 正在等
 * ACK 不会发数据，故 4KB 绰绰有余（单帧最大 7+4160+2 = 4169，也放得下）。 */
#define OTA_RX_DMA_BUF_SIZE      4096U
#define OTA_RX_RING_SIZE         8192U       /* 必须是 2 的幂 */

/* 帧协议固定长度，与 ESP32 侧 connect_stm.h 保持一致，不要改。 */
#define OTA_SOF_0                0xAAU
#define OTA_SOF_1                0x55U
#define OTA_HEADER_LEN           7U          /* SOF(2)+TYPE(1)+SEQ(2)+LEN(2) */
#define OTA_CRC_LEN              2U

/* 上行 JSON 状态串的长度上限。ESP32 侧 main.c 要求 < 384 字节才转发。 */
#define OTA_STATUS_JSON_MAX      160U

/* ==========================================================================
 * 六、协议常量（与 ESP32 侧 connect_stm.h 一一对应，改了就是断链）
 * ========================================================================== */

/* 帧类型：0x01-0x0F ESP32S3 -> STM32；0x10-0x7F STM32 主动上报；
 *         0x80-0xFF STM32 应答 */
#define OTA_FRAME_OTA_START      0x01U
#define OTA_FRAME_OTA_DATA       0x02U
#define OTA_FRAME_OTA_FINISH     0x03U
#define OTA_FRAME_OTA_ABORT      0x04U
#define OTA_FRAME_STM_DATA       0x10U
#define OTA_FRAME_STM_STATUS     0x11U
#define OTA_FRAME_ACK            0x81U
#define OTA_FRAME_CRC_RESULT     0x82U

/* ACK 状态码 */
#define OTA_ACK_OK               0x00U
#define OTA_ACK_CRC_ERROR        0x01U
#define OTA_ACK_FLASH_ERR        0x02U
#define OTA_ACK_OUT_OF_SEQ       0x03U

/* OTA_ABORT 原因码（目前只记日志，不做差异化处理） */
#define OTA_ABORT_DOWNLOAD_FAIL  0x01U
#define OTA_ABORT_SIZE_MISMATCH  0x02U
#define OTA_ABORT_CRC_MISMATCH   0x03U
#define OTA_ABORT_TIMEOUT        0x04U

/* CRC_RESULT 状态码，复用 ACK_OK */
#define OTA_CRC_RESULT_OK        0x00U
#define OTA_CRC_RESULT_FAIL      0x01U

/* OTA_START payload 的前 11 个字节（小端 packed）。
 * version_len 个字节的版本号紧跟在后面，无终止符。 */
#define OTA_VERSION_MAX          32U

/* ==========================================================================
 * 七、W25Q64 上 OTA 头的状态机
 * ==========================================================================
 *
 * 掉电安全性靠这个状态：
 *   EMPTY     没在收，也没有待搬运的固件
 *   RECEIVING 正在收（中途掉电 -> 上电后重收，不会碰 App 区）
 *   READY     整包 CRC32 已校验通过，等搬运
 *   CONSUMED  已搬运进 App 区
 * 搬运阶段只认 READY，所以绝不会出现「半个 App」。 */
#define OTA_HDR_STATE_EMPTY      0x00U
#define OTA_HDR_STATE_RECEIVING  0x01U
#define OTA_HDR_STATE_READY      0x02U
#define OTA_HDR_STATE_CONSUMED   0x03U

#define OTA_HDR_MAGIC            0x4F544131U   /* "OTA1"，与 OTA_BOOT_MAGIC 同值但用途不同 */

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CONFIG_H */
