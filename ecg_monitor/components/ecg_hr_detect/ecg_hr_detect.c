/*
 * ecg_hr_detect.c — HR Detection Task
 *
 * Skeleton Pan-Tompkins: cấu trúc đầy đủ, thuật toán đơn giản hóa.
 * TODO: Bổ sung bandpass filter 5-15 Hz và adaptive threshold đầy đủ.
 */

#include "ecg_hr_detect.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ECG_HR";

QueueHandle_t hr_input_queue = NULL;

/* ─── State machine Pan-Tompkins đơn giản ──────────────── */
#define PT_MWI_SIZE     30      // Moving window integration: 30 sample @ 500Hz = 60ms
#define PT_RR_HISTORY   8       // Lưu 8 RR interval để tính average
#define PT_MIN_RR_MS    300     // BPM tối đa 200
#define PT_MAX_RR_MS    2000    // BPM tối thiểu 30

typedef struct {
    float    mwi_buf[PT_MWI_SIZE];  // Moving window integration buffer
    int      mwi_idx;
    float    mwi_sum;

    float    threshold;             // Adaptive threshold
    int64_t  last_peak_us;          // Timestamp đỉnh R gần nhất
    uint32_t rr_history_ms[PT_RR_HISTORY];
    int      rr_idx;
    uint8_t  rr_count;             // Số RR interval đã thu thập

    bool     in_refractory;        // True: đang trong giai đoạn refractory
    int      refractory_count;

    bool     has_prev_sample;
    float    prev_sample;
    bool     has_processed_window;
} pt_state_t;

static pt_state_t s_pt = {0};

/* ════════════════════════════════════════════════════════════
 *  ecg_hr_detect_init
 * ════════════════════════════════════════════════════════════ */
void ecg_hr_detect_init(void)
{
    memset(&s_pt, 0, sizeof(s_pt));
    s_pt.threshold   = 0.3f;  // Ngưỡng ban đầu (sẽ thích nghi)
    s_pt.last_peak_us = 0;
    ESP_LOGI(TAG, "HR detector initialized");
}

/* ════════════════════════════════════════════════════════════
 *  Xử lý 1 sample qua Pan-Tompkins (đơn giản hóa)
 *
 *  TODO: Thêm bandpass filter FIR 5–15 Hz trước bước này.
 *        Hiện tại chỉ dùng derivative + squaring + MWI.
 * ════════════════════════════════════════════════════════════ */
static float pt_process_sample(float sample, float prev_sample)
{
    /* 1. Derivative (xấp xỉ đạo hàm) */
    float deriv = sample - prev_sample;

    /* 2. Squaring */
    float squared = deriv * deriv;

    /* 3. Moving Window Integration */
    s_pt.mwi_sum -= s_pt.mwi_buf[s_pt.mwi_idx];
    s_pt.mwi_buf[s_pt.mwi_idx] = squared;
    s_pt.mwi_sum += squared;
    s_pt.mwi_idx = (s_pt.mwi_idx + 1) % PT_MWI_SIZE;

    return s_pt.mwi_sum / PT_MWI_SIZE;
}

/* ════════════════════════════════════════════════════════════
 *  Xử lý 1 window, trả về true nếu phát hiện peak mới
 * ════════════════════════════════════════════════════════════ */
static bool pt_process_window(const ecg_clean_window_t *win, uint8_t *out_bpm)
{
    bool found_peak = false;
    int start = s_pt.has_processed_window ? ECG_WINDOW_OVERLAP : 0;
    float prev = s_pt.has_prev_sample ? s_pt.prev_sample : win->cleaned[start];

    for (int i = start; i < win->n_samples; i++) {
        float mwi_out = pt_process_sample(win->cleaned[i], prev);
        prev = win->cleaned[i];

        /* Refractory period: bỏ qua 150ms sau mỗi peak */
        if (s_pt.in_refractory) {
            s_pt.refractory_count--;
            if (s_pt.refractory_count <= 0) {
                s_pt.in_refractory = false;
            }
            continue;
        }

        /* Phát hiện peak vượt threshold */
        if (mwi_out > s_pt.threshold) {
            int64_t now_us = win->timestamp_us + (int64_t)i * ECG_PERIOD_US;

            if (s_pt.last_peak_us > 0) {
                uint32_t rr_ms = (uint32_t)((now_us - s_pt.last_peak_us) / 1000);

                if (rr_ms >= PT_MIN_RR_MS && rr_ms <= PT_MAX_RR_MS) {
                    /* Lưu RR interval */
                    s_pt.rr_history_ms[s_pt.rr_idx] = rr_ms;
                    s_pt.rr_idx = (s_pt.rr_idx + 1) % PT_RR_HISTORY;
                    if (s_pt.rr_count < PT_RR_HISTORY) s_pt.rr_count++;

                    /* Tính BPM trung bình */
                    uint32_t rr_sum = 0;
                    for (int j = 0; j < s_pt.rr_count; j++) {
                        rr_sum += s_pt.rr_history_ms[j];
                    }
                    *out_bpm = (uint8_t)(60000 / (rr_sum / s_pt.rr_count));
                    found_peak = true;
                }
            }

            s_pt.last_peak_us = now_us;

            /* Cập nhật threshold thích nghi (75% signal peak) */
            s_pt.threshold = s_pt.threshold * 0.875f + mwi_out * 0.125f * 0.75f;

            /* Vào refractory 150ms = 75 sample @ 500Hz */
            s_pt.in_refractory    = true;
            s_pt.refractory_count = 75;
        }
    }
    s_pt.prev_sample = prev;
    s_pt.has_prev_sample = true;
    s_pt.has_processed_window = true;
    return found_peak;
}

/* ════════════════════════════════════════════════════════════
 *  ecg_hr_detect_task
 * ════════════════════════════════════════════════════════════ */
void ecg_hr_detect_task(void *pvParameters)
{
    ESP_LOGI(TAG, "HR detect task started");

    ecg_clean_window_t win;
    ecg_hr_t           hr_result;
    uint8_t            bpm = 0;

    while (1) {
        if (xQueueReceive(hr_input_queue, &win, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Skip window có lead-off */
        if (!win.valid) {
            continue;
        }

        if (pt_process_window(&win, &bpm)) {
            hr_result.bpm          = bpm;
            hr_result.last_peak_us = s_pt.last_peak_us;
            hr_result.valid        = true;

            /* Cập nhật shared global HR dưới mutex */
            if (xSemaphoreTake(hr_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                g_current_hr = hr_result;
                xSemaphoreGive(hr_mutex);
            }

            /* Gửi vào hr_queue để Display/Network lấy */
            xQueueOverwrite(hr_queue, &hr_result);

            ESP_LOGI(TAG, "HR = %u bpm", bpm);
        }
    }

    vTaskDelete(NULL);
}
