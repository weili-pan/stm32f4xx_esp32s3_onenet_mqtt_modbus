/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    w25q64.c
  * @brief   W25Q64（8MB SPI Flash）最小驱动。
  ******************************************************************************
  */
/* USER CODE END Header */

#include "w25q64.h"

#include <string.h>

#include "main.h"
#include "spi.h"
#include "stm32f4xx_hal.h"
#include "ota_proto.h"     /* 复用 ota_crc32_update，避免同一份算法写两遍 */

/* 引脚还是占位值时的编译期提醒。改好 ota_config.h 里的接线并放开
 * W25Q64_PINS_CONFIRMED 之后，这条警告就会消失。 */
#ifndef W25Q64_PINS_CONFIRMED
#warning "W25Q64 的 MISO/CS 引脚仍是 ota_config.h 里的占位值，请按实际接线修改后再烧录。"
#endif

/* ---------------------------------------------------------------- 指令 */

#define CMD_WRITE_ENABLE   0x06U
#define CMD_READ_STATUS1   0x05U
#define CMD_READ_DATA      0x03U
#define CMD_PAGE_PROGRAM   0x02U
#define CMD_SECTOR_ERASE   0x20U   /* 4KB */
#define CMD_JEDEC_ID       0x9FU

#define STATUS1_WIP        0x01U   /* write in progress */

/* ---------------------------------------------------------------- 底层 */

static inline void cs_low(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_PORT, W25Q64_CS_PIN, GPIO_PIN_RESET);
}

static inline void cs_high(void)
{
    HAL_GPIO_WritePin(W25Q64_CS_PORT, W25Q64_CS_PIN, GPIO_PIN_SET);
}

static uint32_t s_jedec_id;

static bool xfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    if (tx_len > 0U &&
        HAL_SPI_Transmit(&W25Q64_SPI_HANDLE, (uint8_t *)tx, (uint16_t)tx_len,
                         W25Q64_SPI_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    if (rx_len > 0U &&
        HAL_SPI_Receive(&W25Q64_SPI_HANDLE, rx, (uint16_t)rx_len,
                        W25Q64_SPI_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    return true;
}

static void cmd_only(uint8_t cmd)
{
    cs_low();
    (void)xfer(&cmd, 1U, NULL, 0U);
    cs_high();
}

/* 轮询状态寄存器的 WIP 位，直到芯片内部操作结束。
 * 每轮都重新发一次 0x05 并在结束时拉高 CS，是最保守也最通用的写法
 * （不依赖「CS 保持低、连续读状态寄存器」这种芯片相关的便利行为）。
 * 一轮约 4µs，一页编程 0.7~3ms 只需几百轮，不需要额外延时。 */
static bool wait_ready(void)
{
    const uint32_t start = HAL_GetTick();
    uint8_t cmd = CMD_READ_STATUS1;
    uint8_t st = 0xFFU;

    for (;;) {
        cs_low();
        const bool ok = xfer(&cmd, 1U, NULL, 0U) && xfer(NULL, 0U, &st, 1U);
        cs_high();
        if (!ok) {
            return false;
        }
        if ((st & STATUS1_WIP) == 0U) {
            return true;
        }
        if ((HAL_GetTick() - start) > W25Q64_BUSY_TIMEOUT_MS) {
            return false;
        }
    }
}

/* ---------------------------------------------------------------- 对外 */

uint32_t w25q64_jedec_id(void)
{
    return s_jedec_id;
}

bool w25q64_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* CS 先拉高再配成输出，避免初始化过程中出现低电平把芯片选中 */
    W25Q64_CS_CLK_ENABLE();
    HAL_GPIO_WritePin(W25Q64_CS_PORT, W25Q64_CS_PIN, GPIO_PIN_SET);
    gpio.Pin = W25Q64_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(W25Q64_CS_PORT, &gpio);

    /* MISO 走 SPI 复用。CubeMX 只配了 SCK/MOSI，这里补上。
     * Pull-up：CS 有效期间 MISO 不会浮空，读 ID 更稳。 */
    W25Q64_MISO_CLK_ENABLE();
    gpio.Pin = W25Q64_MISO_PIN;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = W25Q64_MISO_AF;
    HAL_GPIO_Init(W25Q64_MISO_PORT, &gpio);

    /* 读 JEDEC ID：0x9F 之后回 3 字节 */
    {
        uint8_t cmd = CMD_JEDEC_ID;
        uint8_t id[3] = {0xFFU, 0xFFU, 0xFFU};

        cs_low();
        const bool ok = xfer(&cmd, 1U, NULL, 0U) && xfer(NULL, 0U, id, 3U);
        cs_high();
        if (!ok) {
            return false;
        }
        s_jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2];
    }

    return s_jedec_id == W25Q64_JEDEC_ID;
}

bool w25q64_read(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t hdr[4];

    if (len == 0U) {
        return true;
    }
    if (buf == NULL || addr + len > W25Q64_CAPACITY) {
        return false;
    }

    hdr[0] = CMD_READ_DATA;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)addr;

    cs_low();
    const bool ok = xfer(hdr, sizeof(hdr), NULL, 0U) &&
                    xfer(NULL, 0U, (uint8_t *)buf, len);
    cs_high();
    return ok;
}

/* 写前必须先发 WREN。每页编程都要重发一次——CS 拉高后 WEL 位会被清掉。 */
static void write_enable(void)
{
    cmd_only(CMD_WRITE_ENABLE);
}

bool w25q64_write(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t hdr[4];

    if (len == 0U) {
        return true;
    }
    if (buf == NULL || addr + len > W25Q64_CAPACITY) {
        return false;
    }

    while (len > 0U) {
        /* 一页不能跨 256 字节边界，所以每轮只写到本页末尾 */
        const uint32_t page_left = W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE);
        const uint32_t n = (len < page_left) ? len : page_left;

        hdr[0] = CMD_PAGE_PROGRAM;
        hdr[1] = (uint8_t)(addr >> 16);
        hdr[2] = (uint8_t)(addr >> 8);
        hdr[3] = (uint8_t)addr;

        write_enable();
        cs_low();
        const bool ok = xfer(hdr, sizeof(hdr), NULL, 0U) && xfer(p, n, NULL, 0U);
        cs_high();
        if (!ok || !wait_ready()) {
            return false;
        }

        addr += n;
        p += n;
        len -= n;
    }
    return true;
}

bool w25q64_sector_erase(uint32_t addr)
{
    uint8_t hdr[4];

    if (addr >= W25Q64_CAPACITY) {
        return false;
    }

    hdr[0] = CMD_SECTOR_ERASE;
    hdr[1] = (uint8_t)(addr >> 16);
    hdr[2] = (uint8_t)(addr >> 8);
    hdr[3] = (uint8_t)addr;

    write_enable();
    cs_low();
    const bool ok = xfer(hdr, sizeof(hdr), NULL, 0U);
    cs_high();

    /* 擦除指令一发出就返回，实际耗时靠轮询 WIP 等（最坏 400ms） */
    return ok && wait_ready();
}

bool w25q64_crc32_readback(uint32_t addr, uint32_t len, uint32_t *out)
{
    /* 1KB 缓冲：960KB 固件回读只要 940 轮，RAM 占用可忽略 */
    static uint8_t chunk[1024];
    uint32_t crc = 0U;

    if (out == NULL || addr + len > W25Q64_CAPACITY) {
        return false;
    }

    while (len > 0U) {
        const uint32_t n = (len < sizeof(chunk)) ? len : (uint32_t)sizeof(chunk);
        if (!w25q64_read(addr, chunk, n)) {
            return false;
        }
        crc = ota_crc32_update(crc, chunk, n);
        addr += n;
        len -= n;
    }

    *out = crc;
    return true;
}
