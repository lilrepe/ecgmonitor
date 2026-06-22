idf_component_register(
    SRCS "ecg_hr_detect.c"
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        freertos
        log
        esp_timer
)
