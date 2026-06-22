/*
 * ecg_inference.c — Inference Task (TFLite Micro skeleton)
 *
 * ╔══════════════════════════════════════════════════════════╗
 * ║  SKELETON — CHƯA TÍCH HỢP MODEL THỰC                   ║
 * ║                                                          ║
 * ║  Hiện tại: pass-through (raw float → cleaned float)     ║
 * ║                                                          ║
 * ║  Để tích hợp TFLite Micro:                              ║
 * ║    1. Thêm dependency esp-tflite-micro vào idf_manifest  ║
 * ║    2. Nhúng model .tflite vào firmware (CMake EMBED)     ║
 * ║    3. Điền vào TODO blocks bên dưới                     ║
 * ╚══════════════════════════════════════════════════════════╝
 */

#include "ecg_inference.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "ECG_INF";

uint32_t g_inference_window_count = 0;
uint32_t g_inference_avg_us       = 0;

/* ─── Sliding window buffer (nằm trong heap, không trong stack) ── */
static float    s_window_buf[ECG_WINDOW_SIZE];  // Buffer float để feed vào model
static uint16_t s_window_fill = 0;              // Số sample hiện có trong buffer

/* ════════════════════════════════════════════════════════════
 *  [SKELETON] Hàm inference thực tế
 *
 *  Input:  input_f[ECG_WINDOW_SIZE]  — tín hiệu thô (normalized 0.0–1.0)
 *  Output: output_f[ECG_WINDOW_SIZE] — tín hiệu đã lọc
 *
 *  TODO: Thay toàn bộ nội dung hàm này bằng TFLite Micro inference:
 *
 *  static tflite::MicroInterpreter *interpreter = nullptr;
 *
 *  static bool run_model(const float *input_f, float *output_f) {
 *      TfLiteTensor *input_tensor = interpreter->input(0);
 *      // Copy và quantize INT8 nếu dùng INT8 model
 *      memcpy(input_tensor->data.f, input_f, ECG_WINDOW_SIZE * sizeof(float));
 *      if (interpreter->Invoke() != kTfLiteOk) return false;
 *      TfLiteTensor *output_tensor = interpreter->output(0);
 *      memcpy(output_f, output_tensor->data.f, ECG_WINDOW_SIZE * sizeof(float));
 *      return true;
 *  }
 * ════════════════════════════════════════════════════════════ */
static bool run_inference_stub(const float *input_f, float *output_f)
{
    /* STUB: pass-through — copy input sang output không thay đổi */
    memcpy(output_f, input_f, ECG_WINDOW_SIZE * sizeof(float));
    return true;
}

/* ════════════════════════════════════════════════════════════
 *  ecg_inference_init
 * ════════════════════════════════════════════════════════════ */
bool ecg_inference_init(void)
{
    ESP_LOGI(TAG, "Inference init (SKELETON mode — model not loaded)");

    /*
     * TODO: Khi tích hợp TFLite Micro, làm theo thứ tự:
     *
     * 1. Khai báo extern byte array model (được generate bằng xxd hoặc CMake EMBED):
     *    extern const uint8_t ecg_model_tflite_start[] asm("_binary_ecg_model_tflite_start");
     *    extern const uint8_t ecg_model_tflite_end[]   asm("_binary_ecg_model_tflite_end");
     *
     * 2. Cấp phát tensor arena từ PSRAM (ESP32-S3 N16R8 có 8MB PSRAM):
     *    static uint8_t *tensor_arena = NULL;
     *    tensor_arena = (uint8_t*)heap_caps_malloc(TENSOR_ARENA_SIZE, MALLOC_CAP_SPIRAM);
     *
     * 3. Khởi tạo MicroInterpreter và AllocateTensors()
     *
     * 4. Kiểm tra input/output tensor shape khớp ECG_WINDOW_SIZE
     */

    memset(s_window_buf, 0, sizeof(s_window_buf));
    s_window_fill = 0;
    return true;
}

/* ════════════════════════════════════════════════════════════
 *  ecg_inference_task
 * ════════════════════════════════════════════════════════════ */
void ecg_inference_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Inference task started (window=%d, step=%d)",
             ECG_WINDOW_SIZE, ECG_WINDOW_STEP);

    ecg_raw_sample_t    raw;
    ecg_clean_window_t  clean;

    /* Timestamp của sample đầu tiên trong window hiện tại */
    int64_t window_start_ts = 0;
    bool    window_has_lead_off = false;

    /* Buffer output của model */
    static float output_buf[ECG_WINDOW_SIZE];

    while (1) {
        /* ─── Nhận từng sample từ raw_queue ──────────── */
        if (xQueueReceive(raw_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Lưu timestamp đầu tiên */
        if (s_window_fill == 0) {
            window_start_ts      = raw.timestamp_us;
            window_has_lead_off  = false;
        }

        if (raw.lead_off) {
            window_has_lead_off = true;
        }

        /* Normalize raw ADC (0–4095) về float (0.0–1.0) */
        s_window_buf[s_window_fill++] = (float)raw.raw / 4095.0f;

        /* ─── Đủ window → inference ───────────────────── */
        if (s_window_fill >= ECG_WINDOW_SIZE) {

            int64_t t_start = esp_timer_get_time();

            bool ok = run_inference_stub(s_window_buf, output_buf);

            int64_t t_end = esp_timer_get_time();
            uint32_t inf_us = (uint32_t)(t_end - t_start);

            /* Cập nhật thống kê (rolling average đơn giản) */
            g_inference_window_count++;
            g_inference_avg_us = (g_inference_avg_us * 7 + inf_us) / 8;

            if (ok) {
                /* ─── Đóng gói clean window ─────────── */
                clean.timestamp_us = window_start_ts;
                clean.n_samples    = ECG_WINDOW_SIZE;
                clean.valid        = !window_has_lead_off;
                memcpy(clean.cleaned, output_buf, ECG_WINDOW_SIZE * sizeof(float));

                /* Đẩy vào clean_queue — block tối đa 5ms */
                if (xQueueSend(clean_queue, &clean, pdMS_TO_TICKS(5)) != pdTRUE) {
                    ESP_LOGW(TAG, "clean_queue full — drop window #%lu",
                             g_inference_window_count);
                }
            }

            /* ─── Slide window (overlap 50%) ────────── */
            /*
             * Bỏ ECG_WINDOW_STEP sample đầu,
             * dịch phần overlap về đầu buffer
             */
            memmove(s_window_buf,
                    s_window_buf + ECG_WINDOW_STEP,
                    ECG_WINDOW_OVERLAP * sizeof(float));
            s_window_fill = ECG_WINDOW_OVERLAP;

            /* Log thống kê mỗi 100 window */
            if (g_inference_window_count % 100 == 0) {
                ESP_LOGI(TAG, "windows=%lu  avg_inf=%lu us",
                         g_inference_window_count, g_inference_avg_us);
            }
        }
    }

    vTaskDelete(NULL);
}
