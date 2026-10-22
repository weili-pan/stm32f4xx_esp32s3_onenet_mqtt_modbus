idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES onenet_mqtt connect_stm get_wifi_ota json esp_app_format lwip)
