#include "onenet_token.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static const char *TAG = "onenet_token";

/* DeviceSecret base64 解码后通常为 16~32 字节，留足余量 */
#define SECRET_DECODED_MAX 64
#define SIG_RAW_MAX        64 /* SHA1 的 base64 为 28 字节 */

/* 把 src 做严格 URL 编码（仅保留 RFC3986 unreserved 字符）写入 dst */
static esp_err_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        const bool unreserved = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ||
                                *p == '~';
        const size_t need = unreserved ? 1 : 3;
        if (o + need + 1 > dst_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (unreserved) {
            dst[o++] = (char)*p;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[*p >> 4];
            dst[o++] = hex[*p & 0x0F];
        }
    }
    dst[o] = '\0';
    return ESP_OK;
}

/* base64(hmac_<method>(base64_decode(access_key), "et\nmethod\nresource\nversion")) */
static esp_err_t calc_sign_raw(char *out, size_t out_size, const char *resource, uint32_t et,
                               const char *method, const char *access_key)
{
    uint8_t key[SECRET_DECODED_MAX];
    size_t key_len = 0;

    const int rc =
        mbedtls_base64_decode(key, sizeof(key), &key_len, (const unsigned char *)access_key,
                              strlen(access_key));
    if (rc != 0) {
        ESP_LOGE(TAG, "DeviceSecret 不是合法的 base64 (mbedtls rc=%d)", rc);
        return ESP_ERR_INVALID_ARG;
    }

    const mbedtls_md_type_t md_type =
        (strcmp(method, "sha256") == 0) ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA1;
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_type);
    if (md_info == NULL) {
        return ESP_FAIL;
    }

    /* 签名字符串按参数名排序：et、method、res、version */
    char str_for_sig[256];
    const int n = snprintf(str_for_sig, sizeof(str_for_sig), "%u\n%s\n%s\n%s", (unsigned)et, method,
                           resource, ONENET_TOKEN_VERSION);
    if (n <= 0 || n >= (int)sizeof(str_for_sig)) {
        ESP_LOGE(TAG, "签名字符串过长");
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hmac[32];
    if (mbedtls_md_hmac(md_info, key, key_len, (const unsigned char *)str_for_sig, (size_t)n,
                        hmac) != 0) {
        return ESP_FAIL;
    }

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_size, &olen, hmac,
                              mbedtls_md_get_size(md_info)) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t onenet_token_sign(char *out, size_t out_size, const char *resource, uint32_t et,
                            const char *method, const char *access_key)
{
    char raw[SIG_RAW_MAX];
    esp_err_t err = calc_sign_raw(raw, sizeof(raw), resource, et, method, access_key);
    if (err != ESP_OK) {
        return err;
    }
    return url_encode(raw, out, out_size);
}

esp_err_t onenet_token_generate(char *out, size_t out_size, const char *product_key,
                                const char *device_name, const char *device_secret, uint32_t et)
{
    if (out == NULL || product_key == NULL || device_name == NULL || device_secret == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (et == 0) {
        et = (uint32_t)time(NULL) + CONFIG_ONENET_TOKEN_LIFETIME_S;
    }

    char resource[160];
    const int rn = snprintf(resource, sizeof(resource), "products/%s/devices/%s", product_key,
                            device_name);
    if (rn <= 0 || rn >= (int)sizeof(resource)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char sig_raw[SIG_RAW_MAX];
    esp_err_t err = calc_sign_raw(sig_raw, sizeof(sig_raw), resource, et, "sha1", device_secret);
    if (err != ESP_OK) {
        return err;
    }

    /* token 中每个 value 都要 URL 编码 */
    char res_enc[256];
    char sig_enc[SIG_RAW_MAX * 3 + 1];
    if (url_encode(resource, res_enc, sizeof(res_enc)) != ESP_OK ||
        url_encode(sig_raw, sig_enc, sizeof(sig_enc)) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int n = snprintf(out, out_size, "version=%s&res=%s&et=%u&method=sha1&sign=%s",
                           ONENET_TOKEN_VERSION, res_enc, (unsigned)et, sig_enc);
    if (n <= 0 || n >= (int)out_size) {
        ESP_LOGE(TAG, "token 缓冲不足，需要 %d 字节", n);
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGD(TAG, "et=%u token 生成成功", (unsigned)et);
    return ESP_OK;
}
