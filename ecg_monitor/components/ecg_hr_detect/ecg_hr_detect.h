/*
 * ecg_hr_detect.h — HR Detection Task
 *
 * Nhận cleaned window từ clean_queue (hoặc subscribe riêng),
 * phát hiện đỉnh R bằng thuật toán Pan-Tompkins đơn giản,
 * tính BPM và cập nhật g_current_hr (shared, bảo vệ bằng mutex).
 */
#pragma once

#include "ecg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo HR detector (reset state machine Pan-Tompkins).
 */
void ecg_hr_detect_init(void);

/**
 * @brief FreeRTOS task: HR Detection.
 *        Core 0, Priority 3.
 *
 *        Thuật toán:
 *          1. Nhận cleaned window từ hr_input_queue
 *          2. Chạy bộ lọc derivative + squaring + moving average
 *          3. Phát hiện đỉnh R bằng adaptive threshold
 *          4. Tính BPM từ RR interval
 *          5. Cập nhật g_current_hr dưới mutex
 *
 *        NOTE: hr_input_queue là queue riêng do main.c tạo,
 *              Inference Task gửi copy sang đây (tách biệt clean_queue
 *              để Display/Network không bị chặn bởi HR processing).
 */
void ecg_hr_detect_task(void *pvParameters);

/* Queue riêng cho HR task (nhận copy của clean window) */
extern QueueHandle_t hr_input_queue;

#ifdef __cplusplus
}
#endif
