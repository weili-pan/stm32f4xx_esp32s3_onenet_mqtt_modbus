idf_component_register(SRCS "get_wifi_ota.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES connect_stm
                    REQUIRES esp_wifi nvs_flash esp_http_client mbedtls app_update esp_netif esp_event esp-tls esp_timer)
