idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        ecg_adc
        ecg_inference
        ecg_hr_detect
        ecg_display
        ecg_network
        nvs_flash
        freertos
        log
)
