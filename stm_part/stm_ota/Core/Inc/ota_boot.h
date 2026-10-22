/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota_boot.h
  * @brief   stm_ota Bootloader 主流程入口。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __OTA_BOOT_H
#define __OTA_BOOT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在 main() 里、所有 MX_xxx_Init() 之后调用。
 *
 * 正常情况下**不会返回**：要么跳进 App，要么停在这里反复等 OTA。
 */
void ota_boot_run(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_BOOT_H */
