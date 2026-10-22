/*
 * 应用编排层
 *
 *   OneNET 云端 ──MQTT──▶ onenet_mqtt ──property/set 回调──▶ get_wifi_ota ──HTTPS──▶ 固件服务器
 *                                                                  │
 *                                                        UART 分片 │ (target=stm32)
 *                                                                  ▼
 *                                                              connect_stm ──▶ STM32F407
 *
 *   STM32F407 ──UART JSON──▶ connect_stm ──上行回调──▶ onenet_mqtt ──属性上报──▶ OneNET
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "connect_stm.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "get_wifi_ota.h"
#include "onenet_mqtt.h"
#include "sdkconfig.h"

static const char *TAG = "app";

#define MAX_PARAMS_LEN 384

/* ------------------------------------------------------- SNTP 对时 */

/* OneNET 的 token 中 et 依赖系统时间，时间不对会直接鉴权失败 */
static void start_sntp_and_wait(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    for (int retry = 1; retry <= 15; retry++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        if (now > 1700000000) { /* 晚于 2023-11-14 视为已同步 */
            ESP_LOGI(TAG, "SNTP 对时成功: %s", ctime(&now));
            return;
        }
    }
    ESP_LOGW(TAG, "SNTP 对时超时，token 可能被云端拒绝（et 基于错误时间）");
}

/* ------------------------------------------------------- 下行：OTA 下发 */

/* 从属性设置报文中提取 OTA 任务。形如：
   {"id":"1","params":{"url":"https://...","version":"1.0.1","size":1024,
                       "md5":"...","target":"stm32"}} */
/* OneJSON 下发的属性会包一层 {"value":...}；此函数两种写法都兼容，
   是对象就取里面的 value，扁平值直接返回。 */
static const cJSON *job_field(const cJSON *params, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(params, key);
    if (it != NULL && cJSON_IsObject(it)) {
        return cJSON_GetObjectItemCaseSensitive(it, "value");
    }
    return it;
}

static bool parse_ota_job(const char *json, ota_job_t *job)
{
    cJSON *root = cJSON_Parse(json); /* 非 const：cJSON_Delete 需要可写指针 */
    if (root == NULL) {
        ESP_LOGW(TAG, "下行报文不是合法 JSON");
        return false;
    }

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    const cJSON *url = job_field(params, "url");
    bool ok = false;

    if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
        memset(job, 0, sizeof(*job));
        strlcpy(job->url, url->valuestring, sizeof(job->url));
        job->target = OTA_TARGET_STM32; /* 默认目标为 STM32 */

        const cJSON *ver = job_field(params, "version");
        if (cJSON_IsString(ver)) {
            strlcpy(job->version, ver->valuestring, sizeof(job->version));
        }
        const cJSON *md5 = job_field(params, "md5");
        if (cJSON_IsString(md5)) {
            strlcpy(job->md5, md5->valuestring, sizeof(job->md5));
        }
        const cJSON *size = job_field(params, "size");
        if (cJSON_IsNumber(size) && size->valuedouble > 0) {
            job->size = (uint32_t)size->valuedouble;
        }
        const cJSON *target = job_field(params, "target");
        if (cJSON_IsString(target) && strcmp(target->valuestring, "esp32s3") == 0) {
            job->target = OTA_TARGET_ESP32S3;
        }
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

static void on_downlink(const char *topic, const char *json, int len)
{
    (void)topic;
    (void)len;

    ota_job_t job;
    if (!parse_ota_job(json, &job)) {
        ESP_LOGD(TAG, "下行报文不含 OTA 任务，已忽略");
        return;
    }

    if (job.version[0] == '\0') {
        strlcpy(job.version, "unknown", sizeof(job.version));
    }
    ESP_LOGI(TAG, "收到 OTA 任务: target=%s version=%s size=%" PRIu32,
             job.target == OTA_TARGET_STM32 ? "STM32" : "ESP32S3", job.version, job.size);

    const esp_err_t err = get_wifi_ota_start(&job);
    if (err != ESP_OK) {
        onenet_mqtt_report_ota_progress("failed", -1, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------- OTA 进度上报 */

static void on_ota_progress(const ota_job_t *job, uint32_t done, uint32_t total, esp_err_t err,
                           bool finished)
{
    if (!finished) {
        const int percent = (total > 0) ? (int)((uint64_t)done * 100 / total) : -1;
        /* 每 5% 上报一次，避免刷屏 */
        static int last_bucket = -1;
        const int bucket = (percent >= 0) ? percent / 5 : -1;
        if (bucket != last_bucket) {
            last_bucket = bucket;
            onenet_mqtt_report_ota_progress("downloading", percent, NULL);
        }
        return;
    }

    if (err == ESP_OK) {
        onenet_mqtt_report_ota_progress("success", 100, job->version);
    } else {
        onenet_mqtt_report_ota_progress("failed", -1, esp_err_to_name(err));
    }
}

/* ------------------------------------------------------- 上行：STM32 数据 */

static void on_stm_frame(uint8_t type, uint16_t seq, const connect_stm_frame_t *payload)
{
    (void)seq;
    switch (type) {
    case CONNECT_STM_FRAME_STM_DATA:
    case CONNECT_STM_FRAME_STM_STATUS: {
        if (payload->len == 0 || payload->len >= MAX_PARAMS_LEN) {
            ESP_LOGW(TAG, "STM32 上行帧长度异常: %u", payload->len);
            return;
        }
        /* STM32 上报的是一段 JSON 对象，直接作为 params 透传到云端 */
        char params[MAX_PARAMS_LEN];
        memcpy(params, payload->data, payload->len);
        params[payload->len] = '\0';
        if (onenet_mqtt_publish_property(params) != ESP_OK) {
            ESP_LOGW(TAG, "上行数据转发到云端失败（MQTT 未连接?）");
        }
        break;
    }
    default:
        ESP_LOGD(TAG, "忽略未处理的 STM32 帧类型 0x%02X", type);
        break;
    }
}

/* ------------------------------------------------------- 周期上报 */

/* 用独立任务而非 esp_timer 回调：读 RSSI 要拿 WiFi 锁，发 MQTT 可能要等发送队列，
   这些操作都不适合放在 esp_timer 的任务里，会拖慢同任务上的其它定时器。 */
static void report_task(void *arg)
{
    (void)arg;
    const esp_app_desc_t *app = esp_app_get_description();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_ONENET_REPORT_PERIOD_S * 1000));
        char params[MAX_PARAMS_LEN];
        snprintf(params, sizeof(params),
                 "{\"wifi_rssi\":%d,\"firmware_version\":\"%s\",\"stm_link\":%s}", wifi_get_rssi(),
                 app->version, connect_stm_is_link_up() ? "true" : "false");
        if (onenet_mqtt_publish_property(params) != ESP_OK) {
            ESP_LOGD(TAG, "周期上报跳过（MQTT 未连接）");
        }
    }
}

/* ------------------------------------------------------- 下行：向 STM32 推状态 */

/* 把 WiFi/MQTT/RSSI/OTA 状态打成 LINK_STATUS 帧发给 STM32，驱动屏幕上的
   「已连接 / WiFi -55 dBm / 设备在线 / OTA 就绪」。
   策略：每 200ms 采一次，只要 wifi/mqtt/devices/ota_ready 有变化就立刻发（保证连上或
   断开的瞬间就上屏）；否则每 STATUS_HEARTBEAT_MS 发一次，顺带刷新 RSSI 并当心跳
   —— STM32 侧超过 3s 收不到就判定链路断开，状态整体回落。
   注意变更判定不含 rssi：否则信号抖动会让它每 200ms 发一帧、白白占串口。 */
#define STATUS_POLL_MS      200
#define STATUS_HEARTBEAT_MS 1000

static void stm_status_task(void *arg)
{
    (void)arg;

    connect_stm_link_status_t last = {0};
    bool have_last = false;
    TickType_t last_send = xTaskGetTickCount();

    while (true) {
        const bool wifi = wifi_is_connected();
        const bool mqtt = onenet_mqtt_is_connected();

        const connect_stm_link_status_t st = {
            .wifi = wifi ? 1 : 0,
            .mqtt = mqtt ? 1 : 0,
            .rssi = wifi ? wifi_get_rssi() : 0,
            /* ESP32 视角只有一条 STM32 链路，在线即为 1 */
            .devices = connect_stm_is_link_up() ? 1 : 0,
            .ota_ready = (wifi && mqtt) ? 1 : 0,
        };

        const bool changed = !have_last || st.wifi != last.wifi || st.mqtt != last.mqtt ||
                             st.devices != last.devices || st.ota_ready != last.ota_ready;
        const bool heartbeat =
            (xTaskGetTickCount() - last_send) >= pdMS_TO_TICKS(STATUS_HEARTBEAT_MS);

        if (changed || heartbeat) {
            if (connect_stm_send_link_status(&st) == ESP_OK) {
                last = st;
                have_last = true;
                last_send = xTaskGetTickCount();
            } else {
                ESP_LOGW(TAG, "LINK_STATUS 发送失败，下轮重试");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(STATUS_POLL_MS));
    }
}

/* ------------------------------------------------------- app_main */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32S3 OneNET MQTT + OTA 桥接启动，固件版本 %s",
             esp_app_get_description()->version);

    /* 1. WiFi：connect_stm 与 onenet 都依赖它，故先初始化并等待联网。
       配置缺失时只是打印错误并停在日志里，避免 ESP_ERROR_CHECK 造成重启循环。 */
    if (get_wifi_ota_init() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 初始化失败，请检查 menuconfig 中的 SSID/密码后重启");
        return;
    }
    if (!wifi_wait_connected(30000)) {
        ESP_LOGW(TAG, "30 秒内未连上 WiFi，后续依赖自动重连");
    }

    /* 2. 串口链路 */
    if (connect_stm_init() != ESP_OK) {
        ESP_LOGE(TAG, "串口初始化失败");
    }
    connect_stm_register_rx_cb(on_stm_frame);

    /* 3. OTA 进度外抛 */
    get_wifi_ota_register_progress_cb(on_ota_progress);

    /* 4. 云端连接：token 的 et 依赖系统时间，必须先对时 */
    if (wifi_is_connected()) {
        start_sntp_and_wait();
    }
    if (onenet_mqtt_init() != ESP_OK) {
        ESP_LOGE(TAG, "OneNET topic 模板配置有误，请检查 menuconfig");
    }
    onenet_mqtt_register_downlink_cb(on_downlink);
    if (onenet_mqtt_start() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT 启动失败，请检查 menuconfig 中的三元组与接入地址");
    } else if (!onenet_mqtt_is_connected()) {
        ESP_LOGW(TAG, "MQTT 尚未连上（WiFi 恢复后会自动重连）");
    }

    /* 5. 周期性上报设备状态 */
    if (xTaskCreate(report_task, "report", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建周期上报任务失败");
    } else {
        ESP_LOGI(TAG, "初始化完成，每 %d 秒上报一次设备状态", CONFIG_ONENET_REPORT_PERIOD_S);
    }

    /* 6. 周期性把链路状态推给 STM32，驱动屏幕状态（兼作心跳） */
    if (xTaskCreate(stm_status_task, "stm_status", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建 STM32 状态推送任务失败");
    }
}
