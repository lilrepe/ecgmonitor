/*
 * ecg_serial_debug.c — Serial Debug Task (Yêu cầu 1)
 *
 * Mục đích: In dữ liệu thô từ ADC ra UART để kiểm tra tín hiệu ECG
 *           trước khi tích hợp inference / display / network.
 *
 * Chỉ compile khi CONFIG_ECG_SERIAL_DEBUG=y
 *
 * Output format (tab-separated, dễ copy vào Excel / Python):
 *   [t_ms]  raw   mv    lo
 *   1000    2048  1650  0
 *   1002    2051  1652  0
 *   ...
 *
 * Để bật:
 *   idf.py menuconfig → ECG Monitor Configuration → Serial Debug Mode
 *   hoặc thêm vào sdkconfig.defaults:
 *     CONFIG_ECG_SERIAL_DEBUG=y
 */

#include "ecg_serial_debug.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ECG_DBG";

/* ═══════════════════════════════════════════════════════════
 *  Cấu hình in
 * ═══════════════════════════════════════════════════════════ */

/* Số sample in mỗi lần (in thành block để giảm tải UART) */
#define DBG_PRINT_BLOCK     10

/* In header mỗi N dòng để dễ đọc khi scroll */
#define DBG_HEADER_EVERY    200

/* Tốc độ in tối đa: thêm delay nếu cần tránh nghẽn UART.
 * UART 115200 baud ≈ ~11 KB/s. Mỗi dòng ~25 bytes → tối đa ~440 dòng/s.
 * Ở 500 Hz sample rate → cần in 500 dòng/s → có thể nghẽn.
 * Giải pháp: in 1 trong mỗi DBG_DECIMATION sample (ví dụ 1:5 → 100 dòng/s).
 */
#define DBG_DECIMATION      1   /* 1 = in tất cả, 5 = bỏ 4/5 */

/* ═══════════════════════════════════════════════════════════
 *  ecg_serial_debug_task
 * ═══════════════════════════════════════════════════════════ */
void ecg_serial_debug_task(void *pvParameters)
{
    ESP_LOGI(TAG, "=== ECG Serial Debug Mode ===");
    ESP_LOGI(TAG, "Sample rate: %d Hz | Decimation: 1:%d",
             ECG_SAMPLE_RATE_HZ, DBG_DECIMATION);
    ESP_LOGI(TAG, "Format: timestamp_ms <TAB> raw(0-4095) <TAB> voltage_mv <TAB> lead_off");
    ESP_LOGI(TAG, "---------------------------------------------");

    /* In header lần đầu */
    printf("\n# ECG RAW DATA — ESP32-S3 + AD8232\n");
    printf("# sample_rate=%d Hz | decimation=1:%d\n",
           ECG_SAMPLE_RATE_HZ, DBG_DECIMATION);
    printf("t_ms\traw\tmv\tlo\n");

    ecg_raw_sample_t sample;
    uint32_t total   = 0;   /* Tổng số sample đã đọc */
    uint32_t printed = 0;   /* Số dòng đã in */
    uint32_t lo_count = 0;  /* Số lần lead_off */

    /* Timestamp gốc để tính relative time */
    int64_t t0_us = 0;
    bool    t0_set = false;

    while (1) {
        /* Block chờ sample từ raw_queue (timeout 1 giây) */
        if (xQueueReceive(raw_queue, &sample, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "raw_queue timeout — ADC task running? "
                          "total=%lu lo=%lu", total, lo_count);
            continue;
        }

        total++;

        /* Ghi nhận timestamp gốc từ sample đầu tiên */
        if (!t0_set) {
            t0_us = sample.timestamp_us;
            t0_set = true;
            ESP_LOGI(TAG, "First sample received — t0=%lld us", t0_us);
        }

        /* Theo dõi lead_off */
        if (sample.lead_off) {
            lo_count++;
        }

        /* Decimation: chỉ in 1 trong DBG_DECIMATION sample */
        if ((total % DBG_DECIMATION) != 0) {
            continue;
        }

        /* In header định kỳ */
        if (printed % DBG_HEADER_EVERY == 0 && printed > 0) {
            printf("# [%lu samples | lo_count=%lu]\n", total, lo_count);
            printf("t_ms\traw\tmv\tlo\n");
        }

        /* In dòng dữ liệu
         *   t_ms : thời gian tương đối từ sample đầu (ms)
         *   raw  : giá trị ADC 12-bit (0–4095)
         *   mv   : điện áp đã calibrate (mV), 0 nếu chưa cali
         *   lo   : 1 = lead off (điện cực bị bong), 0 = OK
         */
        int64_t t_ms = (sample.timestamp_us - t0_us) / 1000;
        printf("%lld\t%d\t%d\t%d\n",
               t_ms,
               sample.raw,
               sample.voltage_mv,
               (int)sample.lead_off);

        printed++;

        /* Flush mỗi block để giảm độ trễ hiển thị trên terminal */
        if (printed % DBG_PRINT_BLOCK == 0) {
            fflush(stdout);

            /* Mỗi 1000 dòng: in tóm tắt trạng thái vào stderr (không lẫn data) */
            if (printed % 1000 == 0) {
                ESP_LOGI(TAG, "Progress: total=%lu printed=%lu lo=%lu",
                         total, printed, lo_count);
            }
        }
    }

    vTaskDelete(NULL);
}

