idf_component_register(SRCS "onenet_mqtt.c" "onenet_token.c"
                    INCLUDE_DIRS "include"
                    REQUIRES mqtt mbedtls json esp-tls esp_event)
