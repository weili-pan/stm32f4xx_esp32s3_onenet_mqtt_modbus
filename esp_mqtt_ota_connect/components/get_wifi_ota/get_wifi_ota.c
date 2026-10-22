#include "get_wifi_ota.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "connect_stm.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "get_wifi_ota";

#define OTA_TASK_STACK 8192
#define OTA_TASK_PRIO  5

/* 下载缓冲区复用串口分片大小：读一片转发一片，无需二次拷贝。
   connect_stm 的 Kconfig 已把该值限制在 256~4096。 */
#define READ_CHUNK CONFIG_CONNECT_STM_CHUNK_SIZE

#define RECONNECT_DELAY_MS 1000

static QueueHandle_t s_queue;
static ota_progress_cb_t s_progress_cb;
static esp_http_client_handle_t s_http; /* 当前进行中的下载句柄 */
static volatile bool s_got_ip;
static volatile bool s_busy;

static void ota_task_entry(void *arg);

/* ---------------------------------------------------------------- WiFi */

bool wifi_is_connected(void)
{
    return s_got_ip;
}

int8_t wifi_get_rssi(void)
{
    if (!s_got_ip) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_got_ip = false;
            const wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "WiFi 断开 (reason=%d)，%d ms 后重连", d->reason, RECONNECT_DELAY_MS);
            /* 固定退避重连。事件回调所在任务允许短暂阻塞，避免密集中断时刷屏 */
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            esp_wifi_connect();
            break;
        }
        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
    }
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!s_got_ip) {
        if (esp_timer_get_time() >= deadline_us) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

esp_err_t get_wifi_ota_init(void)
{
    if (CONFIG_GET_WIFI_OTA_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "WiFi SSID 未配置，请运行 idf.py menuconfig 填写");
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(1, sizeof(ota_job_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_GET_WIFI_OTA_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_GET_WIFI_OTA_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi 已启动，正在连接 \"%s\"", CONFIG_GET_WIFI_OTA_WIFI_SSID);

    if (xTaskCreate(ota_task_entry, "ota_task", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "创建 OTA 任务失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 校验 */

static bool md5_matches(const uint8_t digest[16], const char *expect)
{
    if (expect == NULL || expect[0] == '\0') {
        return true; /* 未提供期望值，跳过校验 */
    }
    char hex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(&hex[i * 2], 3, "%02x", digest[i]);
    }
    if (strcasecmp(hex, expect) != 0) {
        ESP_LOGE(TAG, "MD5 不匹配: 实际 %s 期望 %s", hex, expect);
        return false;
    }
    ESP_LOGI(TAG, "MD5 校验通过");
    return true;
}

/* ---------------------------------------------------------------- 下载 */

/* 下载并转发给 STM32：不写 ESP32S3 flash，边下边通过 UART 分片传输 */
static esp_err_t download_to_stm32(const ota_job_t *job, uint8_t *buf, size_t chunk)
{
    /* 分片序号是 uint16，超过 65535 片就没法用帧头 SEQ 表示了 */
    const uint32_t max_chunks = (job->size + chunk - 1) / chunk;
    if (max_chunks > UINT16_MAX) {
        ESP_LOGE(TAG, "固件过大：需要 %" PRIu32 " 片，超出序号上限 %u（请加大分片）", max_chunks,
                 UINT16_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_MD5), 0);
    if (rc == 0) {
        rc = mbedtls_md_starts(&md);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "初始化 MD5 失败 (rc=%d)", rc);
        mbedtls_md_free(&md);
        return ESP_FAIL;
    }

    uint32_t crc = 0; /* esp_rom_crc32_le 初始值取 0，STM32 侧需一致 */
    uint16_t seq = 0;
    uint32_t done = 0;

    /* 整包 CRC32 只有下完才知道，故 OTA_START 里传 0，
       真正的校验值随后在 OTA_FINISH 中下发 */
    esp_err_t err = connect_stm_ota_begin(job->size, (uint16_t)chunk, 0, job->version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA_START 失败，STM32 未应答");
        mbedtls_md_free(&md);
        return err;
    }

    while (true) {
        const int n = esp_http_client_read(s_http, (char *)buf, (int)chunk);
        if (n < 0) {
            ESP_LOGE(TAG, "HTTP 读取失败");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break; /* 下载结束 */
        }

        crc = esp_rom_crc32_le(crc, buf, (uint32_t)n);
        rc |= mbedtls_md_update(&md, buf, (size_t)n);

        err = connect_stm_ota_data(seq, buf, (uint16_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "第 %u 片转发失败", seq);
            connect_stm_ota_abort(CONNECT_STM_ABORT_DOWNLOAD_FAIL);
            break;
        }
        seq++;
        done += (uint32_t)n;
        if (s_progress_cb != NULL) {
            s_progress_cb(job, done, job->size, ESP_OK, false);
        }
    }

    if (err == ESP_OK) {
        uint8_t digest[16];
        rc |= mbedtls_md_finish(&md, digest);
        if (rc != 0) {
            ESP_LOGE(TAG, "MD5 计算失败");
            err = ESP_FAIL;
        } else if (!md5_matches(digest, job->md5)) {
            connect_stm_ota_abort(CONNECT_STM_ABORT_CRC_MISMATCH);
            err = ESP_ERR_INVALID_CRC;
        } else if (job->size != 0 && done != job->size) {
            ESP_LOGE(TAG, "长度不符: 收到 %" PRIu32 " 期望 %" PRIu32, done, job->size);
            connect_stm_ota_abort(CONNECT_STM_ABORT_SIZE_MISMATCH);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = connect_stm_ota_finish(crc);
        }
    }

    mbedtls_md_free(&md);
    return err;
}

/* 下载并写入 ESP32S3 的 OTA 分区。镜像完整性由 esp_ota_end 内置的
   SHA256 校验保证（比外部 MD5 更强），因此这里不重复做 MD5。 */
static esp_err_t download_to_esp32s3(const ota_job_t *job, uint8_t *buf, size_t chunk)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "找不到可用的 OTA 分区");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t handle = 0;
    const size_t image_size = (job->size > 0) ? job->size : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(part, image_size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t done = 0;
    while (true) {
        const int n = esp_http_client_read(s_http, (char *)buf, (int)chunk);
        if (n < 0) {
            ESP_LOGE(TAG, "HTTP 读取失败");
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            break;
        }
        err = esp_ota_write(handle, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写 OTA 分区失败: %s", esp_err_to_name(err));
            break;
        }
        done += (uint32_t)n;
        if (s_progress_cb != NULL) {
            s_progress_cb(job, done, job->size, ESP_OK, false);
        }
    }

    if (err == ESP_OK && job->size != 0 && done != job->size) {
        ESP_LOGE(TAG, "长度不符: 收到 %" PRIu32 " 期望 %" PRIu32, done, job->size);
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(handle); /* 成功后 handle 失效，不能再 abort */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "固件镜像校验失败: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_ota_set_boot_partition(part);
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "已切换到分区 %s，重启后生效", part->label);
        }
        return err;
    }

    esp_ota_abort(handle);
    return err;
}

/* ---------------------------------------------------------------- 任务 */

static void run_job(const ota_job_t *job_in)
{
    ota_job_t job = *job_in; /* 本地副本：可能需要用 Content-Length 补全 size */

    ESP_LOGI(TAG, "OTA 任务开始: target=%s ver=%s url=%s",
             job.target == OTA_TARGET_STM32 ? "STM32" : "ESP32S3", job.version, job.url);

    if (!wifi_is_connected() && !wifi_wait_connected(10000)) {
        ESP_LOGE(TAG, "WiFi 未连接，OTA 中止");
        if (s_progress_cb != NULL) {
            s_progress_cb(&job, 0, 0, ESP_ERR_INVALID_STATE, true);
        }
        return;
    }

    const esp_http_client_config_t http_cfg = {
        .url = job.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_GET_WIFI_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    s_http = esp_http_client_init(&http_cfg);
    esp_err_t err = ESP_OK;
    uint8_t *buf = NULL;

    if (s_http == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init 失败（URL 是否合法?）");
        err = ESP_FAIL;
        goto done;
    }

    err = esp_http_client_open(s_http, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "连接固件地址失败: %s", esp_err_to_name(err));
        goto done;
    }

    const int64_t content_len = esp_http_client_fetch_headers(s_http);
    const int status = esp_http_client_get_status_code(s_http);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP 状态码 %d，固件获取失败", status);
        err = ESP_FAIL;
        goto done;
    }
    if (content_len > 0 && job.size == 0) {
        job.size = (uint32_t)content_len;
    }
    if (job.size == 0) {
        /* STM32 需要在 OTA_START 中知道总长度以预留空间 */
        ESP_LOGE(TAG, "响应缺少 Content-Length，无法确定固件大小");
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }
    ESP_LOGI(TAG, "固件大小 %" PRIu32 " 字节", job.size);

    buf = malloc(READ_CHUNK);
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    err = (job.target == OTA_TARGET_STM32) ? download_to_stm32(&job, buf, READ_CHUNK)
                                          : download_to_esp32s3(&job, buf, READ_CHUNK);

done:
    free(buf);
    if (s_http != NULL) {
        esp_http_client_close(s_http);
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA 任务成功");
    } else {
        ESP_LOGE(TAG, "OTA 任务失败: %s", esp_err_to_name(err));
    }
    if (s_progress_cb != NULL) {
        s_progress_cb(&job, job.size, job.size, err, true);
    }
}

static void ota_task_entry(void *arg)
{
    (void)arg;
    ota_job_t job;
    while (true) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE) {
            run_job(&job);
            s_busy = false;
        }
    }
}

esp_err_t get_wifi_ota_start(const ota_job_t *job)
{
    if (job == NULL || job->url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) {
        ESP_LOGW(TAG, "已有 OTA 任务在执行，忽略本次请求");
        return ESP_ERR_INVALID_STATE;
    }
    if (job->target == OTA_TARGET_ESP32S3 && !CONFIG_GET_WIFI_OTA_ALLOW_SELF_UPDATE) {
        ESP_LOGW(TAG, "ESP32S3 自升级未启用（menuconfig 中打开「允许自升级」）");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_busy = true;
    if (xQueueSend(s_queue, job, 0) != pdTRUE) {
        s_busy = false;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void get_wifi_ota_register_progress_cb(ota_progress_cb_t cb)
{
    s_progress_cb = cb;
}

bool get_wifi_ota_is_busy(void)
{
    return s_busy;
}
