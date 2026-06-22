idf_component_register(
    SRCS "ecg_adc.c"
         $<$<BOOL:${CONFIG_ECG_UNIT_TEST}>:test_ecg_adc.c>
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        esp_adc
        driver
        esp_timer
        freertos
        log
        unity
)
