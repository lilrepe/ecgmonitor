/*
 * ecg_adc.h — ADC Task: đọc tín hiệu từ AD8232 @ 500 Hz
 */
#pragma once

#include "ecg_common.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo GPIO (LO+, LO-, SDN) và ADC1 + calibration.
 *        Gọi trước khi tạo task.
 */
void ecg_adc_init(void);

/**
 * @brief Giải phóng ADC1 + calibration. Có thể gọi nhiều lần an toàn.
 */
void ecg_adc_deinit(void);

/**
 * @brief FreeRTOS task: đọc ADC @ 500 Hz bằng esp_timer, đẩy vào raw_queue.
 *        Pin to Core 0, Priority 5.
 *
 *        Thuật toán timing:
 *          - Dùng esp_timer_get_time() tích lũy (tránh drift)
 *          - Nếu chưa đến lúc lấy mẫu: vTaskDelay(1) để nhường CPU
 *          - Không busy-wait cứng
 *
 *        Xử lý overflow queue:
 *          - Nếu raw_queue đầy: drop sample, tăng g_adc_overflow_count
 */
void ecg_adc_task(void *pvParameters);

/**
 * @brief Số sample bị drop do queue đầy (để monitor/debug).
 */
extern uint32_t g_adc_overflow_count;

/* Expose variables for unit testing linking */
extern adc_oneshot_unit_handle_t g_test_adc1_handle;
extern adc_cali_handle_t         g_test_adc1_cali_handle;
extern bool                      g_test_adc_calibrated;

#ifdef __cplusplus
}
#endif
