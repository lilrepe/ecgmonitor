/*
 * ecg_inference.h — Inference Task (TFLite Micro skeleton)
 *
 * Nhận raw samples từ raw_queue, gom thành window 128 sample,
 * chạy model deep learning denoiser, đẩy kết quả vào clean_queue.
 *
 * TRẠNG THÁI HIỆN TẠI: SKELETON — chưa tích hợp model thực.
 *   Hàm inference hiện tại chỉ pass-through (copy raw sang cleaned).
 *   TODO: Tích hợp TFLite Micro khi có model .tflite đã quantize INT8.
 */
#pragma once

#include "ecg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo inference engine.
 *        Cấp phát tensor arena, load model từ flash (SPIFFS/embedded).
 *        SKELETON: hiện tại không làm gì — chỉ log thông báo.
 *
 * @return true nếu init OK
 */
bool ecg_inference_init(void);

/**
 * @brief FreeRTOS task: Inference Task.
 *        Core 0, Priority 4.
 *
 *        Flow:
 *          1. Đọc raw_queue, tích lũy vào sliding window (overlap 50%)
 *          2. Khi đủ ECG_WINDOW_SIZE sample → chạy inference
 *          3. Đẩy ecg_clean_window_t vào clean_queue
 *          4. Slide window: bỏ ECG_WINDOW_STEP sample đầu, giữ phần overlap
 */
void ecg_inference_task(void *pvParameters);

/**
 * @brief Số window đã xử lý (để monitor throughput).
 */
extern uint32_t g_inference_window_count;

/**
 * @brief Thời gian inference trung bình (microseconds).
 *        Đo bằng esp_timer_get_time() trước và sau model inference.
 */
extern uint32_t g_inference_avg_us;

#ifdef __cplusplus
}
#endif
