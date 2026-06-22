idf_component_register(
    SRCS "ecg_network.c"
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        freertos
        log
        esp_timer

    # TODO: Nếu dùng cJSON:
    # json
)

# TODO: Nhúng web dashboard:
# target_add_binary_data(${COMPONENT_LIB} "web/index.html" TEXT)
