idf_component_register(
    SRCS "ecg_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        driver
        freertos
        log
        esp_timer

    # TODO: Nếu dùng esp_lcd (IDF >= 5.1):
    # esp_lcd

    # TODO: Nếu dùng u8g2:
    # u8g2  (thêm via idf_component.yml)
)
