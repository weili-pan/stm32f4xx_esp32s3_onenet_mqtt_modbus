#include "connect_stm.h"

#include <inttypes.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "connect_stm";

#define STM_UART_NUM    CONFIG_CONNECT_STM_UART_NUM
#define STM_UART_TX_PIN CONFIG_CONNECT_STM_TX_GPIO
#define STM_UART_RX_PIN CONFIG_CONNECT_STM_RX_GPIO
#define STM_UART_BAUD   CONFIG_CONNECT_STM_BAUD_RATE

#define RX_RINGBUF_SIZE 4096
#define RX_TASK_STACK   4096
#define RX_TASK_PRIO    12
#define TX_TIMEOUT_MS   500

#define MAX_PAYLOAD_LEN (CONFIG_CONNECT_STM_CHUNK_SIZE + 64)
#define ACK_SEQ_NONE    0xFFFF

static SemaphoreHandle_t s_tx_mutex;   /* 串行化发送 */
static SemaphoreHandle_t s_ack_sem;    /* ACK / CRC_RESULT 到达时释放 */
static connect_stm_rx_cb_t s_rx_cb;
static volatile uint16_t s_ack_seq;    /* 最近一次 ACK 的序号 */
static volatile uint8_t s_ack_status;
static volatile bool s_crc_result_valid;
static volatile uint8_t s_crc_result_status;
static volatile int64_t s_last_rx_us;  /* 最近收到 STM32 帧的时刻 */

/* ---------------------------------------------------------------- CRC16 */

static const uint16_t s_crc16_table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,
};

static uint16_t crc16_modbus(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ s_crc16_table[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}

/* ---------------------------------------------------------------- 发送 */

esp_err_t connect_stm_send_frame(uint8_t type, uint16_t seq, const void *payload, uint16_t len)
{
    if (len > MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "payload too large: %u", len);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 帧头 + payload 拼在一起，便于统一计算 CRC16 */
    uint8_t buf[CONNECT_STM_HEADER_LEN + MAX_PAYLOAD_LEN + CONNECT_STM_CRC_LEN];
    buf[0] = CONNECT_STM_SOF_0;
    buf[1] = CONNECT_STM_SOF_1;
    buf[2] = type;
    buf[3] = (uint8_t)(seq & 0xFF);
    buf[4] = (uint8_t)(seq >> 8);
    buf[5] = (uint8_t)(len & 0xFF);
    buf[6] = (uint8_t)(len >> 8);
    if (len > 0 && payload != NULL) {
        memcpy(&buf[CONNECT_STM_HEADER_LEN], payload, len);
    }

    /* CRC16 覆盖 TYPE..PAYLOAD，不含 SOF */
    const uint16_t crc = crc16_modbus(&buf[2], CONNECT_STM_HEADER_LEN - 2 + len);
    const size_t frame_len = CONNECT_STM_HEADER_LEN + len;
    buf[frame_len] = (uint8_t)(crc & 0xFF);
    buf[frame_len + 1] = (uint8_t)(crc >> 8);

    const size_t total_len = frame_len + CONNECT_STM_CRC_LEN;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    const int written = uart_write_bytes(STM_UART_NUM, (const char *)buf, total_len);
    esp_err_t ret = ESP_OK;
    if (written != (int)total_len) {
        ESP_LOGE(TAG, "uart_write_bytes short write: %d", written);
        ret = ESP_FAIL;
    } else {
        ret = uart_wait_tx_done(STM_UART_NUM, pdMS_TO_TICKS(TX_TIMEOUT_MS));
    }
    xSemaphoreGive(s_tx_mutex);
    return ret;
}

/* 阻塞等待序号为 seq 的 ACK */
static esp_err_t wait_ack(uint16_t seq, TickType_t timeout)
{
    const int64_t deadline_us =
        esp_timer_get_time() + (int64_t)timeout * 1000000 / configTICK_RATE_HZ;

    while (esp_timer_get_time() < deadline_us) {
        const int64_t remain_us = deadline_us - esp_timer_get_time();
        const TickType_t remain = pdMS_TO_TICKS(remain_us / 1000) + 1;
        if (xSemaphoreTake(s_ack_sem, remain) != pdTRUE) {
            break; /* 本轮超时 */
        }
        if (s_ack_seq == seq) {
            return (s_ack_status == CONNECT_STM_ACK_OK) ? ESP_OK : ESP_FAIL;
        }
        /* 是其它序号的 ACK（例如上一片迟到的应答），继续等 */
    }
    return ESP_ERR_TIMEOUT;
}

/* ---------------------------------------------------------------- 接收 */

/* 读满 len 字节，超时或对端断开返回 false */
static bool read_exact(uint8_t *dst, size_t len, TickType_t timeout)
{
    size_t got = 0;
    while (got < len) {
        const int n = uart_read_bytes(STM_UART_NUM, dst + got, len - got, timeout);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

static void dispatch_frame(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t len)
{
    s_last_rx_us = esp_timer_get_time();

    if (type == CONNECT_STM_FRAME_ACK && len >= sizeof(connect_stm_ack_payload_t)) {
        connect_stm_ack_payload_t ack;
        memcpy(&ack, payload, sizeof(ack));
        s_ack_seq = ack.ack_seq;
        s_ack_status = ack.status;
        xSemaphoreGive(s_ack_sem);
        return;
    }
    if (type == CONNECT_STM_FRAME_CRC_RESULT && len >= 1) {
        s_crc_result_status = payload[0];
        s_crc_result_valid = true;
        xSemaphoreGive(s_ack_sem);
        return;
    }

    if (s_rx_cb != NULL) {
        const connect_stm_frame_t frame = { .data = payload, .len = len };
        s_rx_cb(type, seq, &frame);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    static uint8_t payload[MAX_PAYLOAD_LEN];
    static uint8_t cover[CONNECT_STM_HEADER_LEN - 2 + MAX_PAYLOAD_LEN];
    uint8_t hdr[CONNECT_STM_HEADER_LEN];
    uint8_t crcbuf[CONNECT_STM_CRC_LEN];

    while (true) {
        /* 1. 寻找 SOF。uart_read_bytes 无法回退，故逐字节推进直到 0xAA 0x55 */
        if (!read_exact(&hdr[0], 1, pdMS_TO_TICKS(100)) || hdr[0] != CONNECT_STM_SOF_0) {
            continue;
        }
        if (!read_exact(&hdr[1], 1, pdMS_TO_TICKS(100)) || hdr[1] != CONNECT_STM_SOF_1) {
            continue;
        }

        /* 2. 读 TYPE/SEQ/LEN */
        if (!read_exact(&hdr[2], CONNECT_STM_HEADER_LEN - 2, pdMS_TO_TICKS(100))) {
            ESP_LOGW(TAG, "header timeout, resync");
            continue;
        }

        const uint8_t type = hdr[2];
        const uint16_t seq = (uint16_t)hdr[3] | ((uint16_t)hdr[4] << 8);
        const uint16_t len = (uint16_t)hdr[5] | ((uint16_t)hdr[6] << 8);

        if (len > MAX_PAYLOAD_LEN) {
            ESP_LOGW(TAG, "bad len %u, resync", len);
            continue;
        }

        /* 3. 读 payload 与 CRC */
        if (!read_exact(payload, len, pdMS_TO_TICKS(200))) {
            ESP_LOGW(TAG, "payload timeout, resync");
            continue;
        }
        if (!read_exact(crcbuf, CONNECT_STM_CRC_LEN, pdMS_TO_TICKS(100))) {
            ESP_LOGW(TAG, "crc timeout, resync");
            continue;
        }

        /* 4. 校验 CRC16，覆盖 TYPE..PAYLOAD */
        memcpy(cover, &hdr[2], CONNECT_STM_HEADER_LEN - 2);
        if (len > 0) {
            memcpy(&cover[CONNECT_STM_HEADER_LEN - 2], payload, len);
        }
        const uint16_t crc_calc = crc16_modbus(cover, CONNECT_STM_HEADER_LEN - 2 + len);
        const uint16_t crc_recv = (uint16_t)crcbuf[0] | ((uint16_t)crcbuf[1] << 8);
        if (crc_calc != crc_recv) {
            ESP_LOGW(TAG, "crc mismatch type=0x%02X seq=%u calc=0x%04X recv=0x%04X", type, seq,
                     crc_calc, crc_recv);
            continue;
        }

        dispatch_frame(type, seq, payload, len);
    }
}

/* ---------------------------------------------------------------- OTA */

esp_err_t connect_stm_ota_begin(uint32_t total_size, uint16_t chunk_size, uint32_t crc32,
                                const char *version)
{
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t ver_len = strlen(version);
    if (ver_len > 32) {
        ESP_LOGE(TAG, "version too long: %u", (unsigned)ver_len);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[sizeof(connect_stm_ota_start_t) + 32];
    const connect_stm_ota_start_t hdr = {
        .total_size = total_size,
        .chunk_size = chunk_size,
        .crc32 = crc32,
        .version_len = (uint8_t)ver_len,
    };
    memcpy(payload, &hdr, sizeof(hdr));
    memcpy(payload + sizeof(hdr), version, ver_len);
    const uint16_t len = (uint16_t)(sizeof(hdr) + ver_len);

    for (int attempt = 1; attempt <= CONFIG_CONNECT_STM_MAX_RETRY; attempt++) {
        s_ack_seq = ACK_SEQ_NONE;
        esp_err_t err = connect_stm_send_frame(CONNECT_STM_FRAME_OTA_START, 0, payload, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OTA_START send failed: %s", esp_err_to_name(err));
            continue;
        }
        err = wait_ack(0, pdMS_TO_TICKS(CONFIG_CONNECT_STM_ACK_TIMEOUT_MS));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA_START acked: size=%" PRIu32 " chunk=%u crc=0x%08" PRIX32 " ver=%s",
                     total_size, chunk_size, crc32, version);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "OTA_START retry %d/%d (%s)", attempt, CONFIG_CONNECT_STM_MAX_RETRY,
                 esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "OTA_START failed after %d attempts", CONFIG_CONNECT_STM_MAX_RETRY);
    return ESP_FAIL;
}

esp_err_t connect_stm_ota_data(uint16_t seq, const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0 || len > CONFIG_CONNECT_STM_CHUNK_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int attempt = 1; attempt <= CONFIG_CONNECT_STM_MAX_RETRY; attempt++) {
        s_ack_seq = ACK_SEQ_NONE;
        esp_err_t err = connect_stm_send_frame(CONNECT_STM_FRAME_OTA_DATA, seq, buf, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "chunk %u send failed: %s", seq, esp_err_to_name(err));
        } else {
            err = wait_ack(seq, pdMS_TO_TICKS(CONFIG_CONNECT_STM_ACK_TIMEOUT_MS));
            if (err == ESP_OK) {
                return ESP_OK;
            }
            ESP_LOGW(TAG, "chunk %u retry %d/%d (%s)", seq, attempt,
                     CONFIG_CONNECT_STM_MAX_RETRY, esp_err_to_name(err));
        }
        /* 让 STM32 有时间从错误状态恢复 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "chunk %u failed after %d attempts", seq, CONFIG_CONNECT_STM_MAX_RETRY);
    return ESP_FAIL;
}

esp_err_t connect_stm_ota_finish(uint32_t crc32)
{
    for (int attempt = 1; attempt <= CONFIG_CONNECT_STM_MAX_RETRY; attempt++) {
        s_crc_result_valid = false;
        esp_err_t err =
            connect_stm_send_frame(CONNECT_STM_FRAME_OTA_FINISH, 0, &crc32, sizeof(crc32));
        if (err != ESP_OK) {
            continue;
        }
        /* CRC_RESULT 与 ACK 共用同一个信号量唤醒 */
        if (xSemaphoreTake(s_ack_sem, pdMS_TO_TICKS(CONFIG_CONNECT_STM_ACK_TIMEOUT_MS * 4)) ==
            pdTRUE) {
            if (s_crc_result_valid && s_crc_result_status == CONNECT_STM_ACK_OK) {
                ESP_LOGI(TAG, "OTA_FINISH verified by STM32, crc=0x%08" PRIX32, crc32);
                return ESP_OK;
            }
            ESP_LOGE(TAG, "STM32 CRC verify failed, status=%u", s_crc_result_status);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "OTA_FINISH retry %d/%d", attempt, CONFIG_CONNECT_STM_MAX_RETRY);
    }
    return ESP_FAIL;
}

esp_err_t connect_stm_ota_abort(connect_stm_abort_reason_t reason)
{
    const uint8_t r = (uint8_t)reason;
    return connect_stm_send_frame(CONNECT_STM_FRAME_OTA_ABORT, 0, &r, sizeof(r));
}

esp_err_t connect_stm_send_link_status(const connect_stm_link_status_t *st)
{
    if (st == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* SEQ 用 0：STM32 侧不按序号匹配，状态帧是纯单向通知 */
    return connect_stm_send_frame(CONNECT_STM_FRAME_LINK_STATUS, 0, st, sizeof(*st));
}

bool connect_stm_is_link_up(void)
{
    if (s_last_rx_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - s_last_rx_us) < (int64_t)CONFIG_CONNECT_STM_LINK_TIMEOUT_MS * 1000;
}

/* ---------------------------------------------------------------- init */

void connect_stm_register_rx_cb(connect_stm_rx_cb_t cb)
{
    s_rx_cb = cb;
}

esp_err_t connect_stm_init(void)
{
    if (s_tx_mutex != NULL) {
        return ESP_OK; /* 已初始化 */
    }

    s_tx_mutex = xSemaphoreCreateMutex();
    s_ack_sem = xSemaphoreCreateBinary();
    if (s_tx_mutex == NULL || s_ack_sem == NULL) {
        ESP_LOGE(TAG, "failed to create sync primitives");
        return ESP_ERR_NO_MEM;
    }

    const uart_config_t cfg = {
        .baud_rate = STM_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(STM_UART_NUM, RX_RINGBUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(STM_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_set_pin(STM_UART_NUM, STM_UART_TX_PIN, STM_UART_RX_PIN, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(rx_task, "stm_rx", RX_TASK_STACK, NULL, RX_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create rx task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "uart%u ready: tx=%d rx=%d baud=%d chunk=%d", STM_UART_NUM, STM_UART_TX_PIN,
             STM_UART_RX_PIN, STM_UART_BAUD, CONFIG_CONNECT_STM_CHUNK_SIZE);
    return ESP_OK;
}
