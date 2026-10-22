#include "onenet_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mqtt_client.h"
#include "onenet_token.h"
#include "sdkconfig.h"

static const char *TAG = "onenet_mqtt";

#define TOPIC_BUF_LEN   256
#define PAYLOAD_BUF_LEN 512
#define TOKEN_BUF_LEN   320

static esp_mqtt_client_handle_t s_client;
static onenet_downlink_cb_t s_downlink_cb;
static volatile bool s_connected;
static char s_topic_property_post[TOPIC_BUF_LEN];
static char s_topic_property_post_reply[TOPIC_BUF_LEN];
static char s_topic_property_set[TOPIC_BUF_LEN];
static char s_topic_property_set_reply[TOPIC_BUF_LEN];
static char s_password[TOKEN_BUF_LEN];

/* OneNET 旧版多协议接入的 topic 固定以 $sys/{pk}/{dn}/ 开头。
   这个前缀不能写进 Kconfig —— Kconfig 字符串会进入 sdkconfig.cmake，
   而 CMake 不接受 "$sys/{" 这种写法（$ 后跟非 {} 形式的 {），会直接报语法错误。 */
#define TOPIC_PREFIX_FMT "$sys/%s/%s/"

static esp_err_t build_one_topic(char *out, size_t out_size, const char *prefix,
                                const char *suffix)
{
    const int n = snprintf(out, out_size, "%s%s", prefix, suffix);
    if (n <= 0 || n >= (int)out_size) {
        ESP_LOGE(TAG, "topic 超长（%d 字节），请缩短 ProductKey/DeviceName 或后缀", n);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t build_topics(void)
{
    char prefix[TOPIC_BUF_LEN];
    const int n = snprintf(prefix, sizeof(prefix), TOPIC_PREFIX_FMT, CONFIG_ONENET_PRODUCT_KEY,
                           CONFIG_ONENET_DEVICE_NAME);
    if (n <= 0 || n >= (int)sizeof(prefix)) {
        ESP_LOGE(TAG, "ProductKey/DeviceName 过长，无法拼出 topic 前缀");
        return ESP_ERR_INVALID_SIZE;
    }

    const struct {
        const char *suffix;
        char *out;
        size_t out_size;
    } items[] = {
        { CONFIG_ONENET_TOPIC_PROPERTY_POST, s_topic_property_post, sizeof(s_topic_property_post) },
        { CONFIG_ONENET_TOPIC_PROPERTY_POST_REPLY, s_topic_property_post_reply,
          sizeof(s_topic_property_post_reply) },
        { CONFIG_ONENET_TOPIC_PROPERTY_SET, s_topic_property_set, sizeof(s_topic_property_set) },
        { CONFIG_ONENET_TOPIC_PROPERTY_SET_REPLY, s_topic_property_set_reply,
          sizeof(s_topic_property_set_reply) },
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        const esp_err_t err = build_one_topic(items[i].out, items[i].out_size, prefix,
                                             items[i].suffix);
        if (err != ESP_OK) {
            return err;
        }
    }

    ESP_LOGI(TAG, "上报属性: %s", s_topic_property_post);
    ESP_LOGI(TAG, "订阅设置: %s", s_topic_property_set);
    return ESP_OK;
}

static void handle_downlink(const char *topic, const char *data, int len)
{
    /* payload 不保证以 '\0' 结尾，先复制一份 */
    char *json = malloc((size_t)len + 1);
    if (json == NULL) {
        ESP_LOGE(TAG, "下行 payload 内存不足 (%d 字节)", len);
        return;
    }
    memcpy(json, data, (size_t)len);
    json[len] = '\0';
    ESP_LOGI(TAG, "收到下行 [%s] %s", topic, json);

    /* 属性设置必须应答，否则云端判定下发失败 */
    if (strcmp(topic, s_topic_property_set) == 0 && s_topic_property_set_reply[0] != '\0') {
        char reply[128];
        cJSON *root = cJSON_Parse(json); /* 非 const：cJSON_Delete 需要可写指针 */
        const cJSON *id = (root != NULL) ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        snprintf(reply, sizeof(reply), "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}",
                 (id != NULL && cJSON_IsString(id)) ? id->valuestring : "0");
        esp_mqtt_client_publish(s_client, s_topic_property_set_reply, reply, 0, 1, 0);
        cJSON_Delete(root);
    }

    if (s_downlink_cb != NULL) {
        s_downlink_cb(topic, json, len);
    }
    free(json);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                               void *event_data)
{
    (void)handler_args;
    (void)base;
    const esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "已连接到 OneNET");
        esp_mqtt_client_subscribe(s_client, s_topic_property_set, 1);
        esp_mqtt_client_subscribe(s_client, s_topic_property_post_reply, 1);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "与 OneNET 断开，等待自动重连");
        break;

    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && event->data_len > 0) {
            char topic[TOPIC_BUF_LEN];
            const int tlen = (event->topic_len < (int)sizeof(topic) - 1) ? event->topic_len
                                                                        : (int)sizeof(topic) - 1;
            memcpy(topic, event->topic, (size_t)tlen);
            topic[tlen] = '\0';
            handle_downlink(topic, event->data, event->data_len);
        } else if (event->topic_len == 0) {
            ESP_LOGW(TAG, "收到 MQTT 分片消息，当前实现未做拼接，已忽略");
        }
        break;

    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGE(TAG, "连接被拒绝: return_code=%d（请检查三元组与 token）",
                     event->error_handle->connect_return_code);
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "传输层错误: esp-tls=0x%x, errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "MQTT 错误: type=%d", event->error_handle->error_type);
        }
        break;

    default:
        break;
    }
}

static esp_err_t start_client(void)
{
    /* ClientID = DeviceName，Username = ProductKey，Password = token */
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_ONENET_BROKER_URI,
        .credentials.client_id = CONFIG_ONENET_DEVICE_NAME,
        .credentials.username = CONFIG_ONENET_PRODUCT_KEY,
        .credentials.authentication.password = s_password,
        .session.keepalive = CONFIG_ONENET_KEEPALIVE_S,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
#if CONFIG_ONENET_USE_TLS
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init 失败");
        return ESP_FAIL;
    }

    esp_err_t err =
        esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "注册 MQTT 事件失败: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "连接 %s", CONFIG_ONENET_BROKER_URI);
    return esp_mqtt_client_start(s_client);
}

esp_err_t onenet_mqtt_refresh_token(void)
{
    const esp_err_t err = onenet_token_generate(s_password, sizeof(s_password),
                                                CONFIG_ONENET_PRODUCT_KEY,
                                                CONFIG_ONENET_DEVICE_NAME,
                                                CONFIG_ONENET_DEVICE_SECRET, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "token 生成失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "token 已刷新（有效期 %d 秒）", CONFIG_ONENET_TOKEN_LIFETIME_S);

    /* esp-mqtt 在 init 时会复制 config 字符串，改 s_password 对已运行的客户端无效，
       因此刷新 token 必须重建客户端。首次启动时 s_client == NULL，跳过销毁直接创建。 */
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
    return start_client();
}

esp_err_t onenet_mqtt_init(void)
{
    return build_topics();
}

esp_err_t onenet_mqtt_start(void)
{
    if (CONFIG_ONENET_PRODUCT_KEY[0] == '\0' || CONFIG_ONENET_DEVICE_NAME[0] == '\0' ||
        CONFIG_ONENET_DEVICE_SECRET[0] == '\0') {
        ESP_LOGE(TAG, "OneNET 三元组未配置，请运行 idf.py menuconfig 填写");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client != NULL) {
        return ESP_OK; /* 已启动 */
    }
    return onenet_mqtt_refresh_token();
}

void onenet_mqtt_register_downlink_cb(onenet_downlink_cb_t cb)
{
    s_downlink_cb = cb;
}

esp_err_t onenet_mqtt_publish_property(const char *params_json)
{
    if (params_json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 调用方传的是扁平对象（如 {"wifi_rssi":-55}）。实测本平台要求 params 里
       每个属性都写成 {"value":<值>}，扁平写法会被拒：code 2402 request format
       error。故在此统一包一层 value，调用方无需关心。 */
    cJSON *flat = cJSON_Parse(params_json);
    if (flat == NULL) {
        ESP_LOGE(TAG, "params 不是合法 JSON: %s", params_json);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *params = cJSON_CreateObject();
    cJSON *root = cJSON_CreateObject();
    if (params == NULL || root == NULL) {
        cJSON_Delete(flat);
        cJSON_Delete(params);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    for (const cJSON *it = flat->child; it != NULL; it = it->next) {
        if (it->string == NULL) {
            continue;
        }
        cJSON *dup = cJSON_Duplicate(it, true);
        cJSON *wrapped = cJSON_CreateObject();
        if (dup == NULL || wrapped == NULL) {
            cJSON_Delete(dup);
            cJSON_Delete(wrapped);
            cJSON_Delete(flat);
            cJSON_Delete(params);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToObject(wrapped, "value", dup);
        cJSON_AddItemToObject(params, it->string, wrapped); /* it->string 会被复制 */
    }
    cJSON_Delete(flat);

    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%u", (unsigned)(esp_random() % 100000)); /* id 为字符串数字 */
    cJSON_AddStringToObject(root, "id", id_buf);
    cJSON_AddStringToObject(root, "version", "1.0");
    cJSON_AddItemToObject(root, "params", params); /* params 所有权移交 root */

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG, "属性上报 %s", payload);
    const int msg_id = esp_mqtt_client_publish(s_client, s_topic_property_post, payload, 0, 1, 0);
    cJSON_free(payload);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "属性上报失败");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t onenet_mqtt_report_ota_progress(const char *stage, int percent, const char *msg)
{
    if (stage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char params[PAYLOAD_BUF_LEN];
    if (msg != NULL) {
        snprintf(params, sizeof(params),
                 "{\"ota_stage\":\"%s\",\"ota_progress\":%d,\"ota_message\":\"%s\"}", stage,
                 percent, msg);
    } else {
        snprintf(params, sizeof(params), "{\"ota_stage\":\"%s\",\"ota_progress\":%d}", stage,
                 percent);
    }
    return onenet_mqtt_publish_property(params);
}

bool onenet_mqtt_is_connected(void)
{
    return s_connected;
}

esp_err_t onenet_mqtt_publish_raw(const char *topic, const char *payload)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return (esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0) < 0) ? ESP_FAIL : ESP_OK;
}
