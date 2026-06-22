idf_component_register(
    SRCS "ecg_inference.c"
    INCLUDE_DIRS "."
    REQUIRES
        ecg_common
        freertos
        log
        esp_timer

    # TODO: Khi tích hợp TFLite Micro, thêm:
    # esp-tflite-micro
)

# TODO: Nhúng model file vào firmware:
# target_add_binary_data(${COMPONENT_LIB} "model/ecg_model.tflite" BINARY)
