/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    w25q64.h
  * @brief   W25Q64（8MB SPI Flash）最小驱动，只做 Bootloader 需要的几件事。
  *
  *          接在哪个 SPI、MISO/CS 是哪个脚，全部由 ota_config.h 的宏决定。
  *          只支持 4KB 扇区擦除（指令 0x20），够用：
  *          固件从 W25Q64_OTA_FW_ADDR 起按 4KB 对齐分段写入。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __W25Q64_H
#define __W25Q64_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ota_config.h"

/**
 * @brief 配置 MISO/CS 引脚并读 JEDEC ID 自检。
 *
 * 依赖 MX_SPI2_Init()（或 ota_config.h 里指定的那个 SPI）已经跑过。
 *
 * @return true 表示读到的 ID 等于 W25Q64_JEDEC_ID（芯片在位且接线正确）
 */
bool w25q64_init(void);

/** 返回最近一次读到的 JEDEC ID，便于对不上时上报出来。 */
uint32_t w25q64_jedec_id(void);

/** 任意地址任意长度读（无对齐要求）。 */
bool w25q64_read(uint32_t addr, void *buf, uint32_t len);

/**
 * @brief 写数据，内部自动按 256 字节页边界切分。
 *
 * 注意：NOR Flash 只能把 1 写成 0，目标区域必须是**已擦除**的。
 * 本驱动不做「读-改-写」，调用方负责先擦。
 */
bool w25q64_write(uint32_t addr, const void *buf, uint32_t len);

/** 擦除 addr 所在的 4KB 扇区（忽略 addr 低 12 位）。 */
bool w25q64_sector_erase(uint32_t addr);

/**
 * @brief 流式回读并计算标准 CRC-32，用来验证「写进去的」和「期望的」一致。
 *
 * 分块读进一个 1KB 的栈外缓冲，所以不要求调用方准备 len 大小的内存，
 * 960KB 的固件也能直接校验。
 *
 * @param out 结果，可与云端下发的整包 crc32 直接比较
 */
bool w25q64_crc32_readback(uint32_t addr, uint32_t len, uint32_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __W25Q64_H */
