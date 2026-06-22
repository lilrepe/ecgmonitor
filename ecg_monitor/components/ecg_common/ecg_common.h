/*
 * ecg_common.h — Shared types, queue handles, config constants
 *
 * Mọi component đều include file này.
 * Không được include header của component khác ở đây để tránh circular deps.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════
 *  Cấu hình lấy mẫu
 * ═══════════════════════════════════════════════════════════ */
#define ECG_SAMPLE_RATE_HZ      500         // Tần số lấy mẫu
#define ECG_PERIOD_US           (1000000 / ECG_SAMPLE_RATE_HZ)  // 2000 us

/* ─── Cửa sổ inference ─────────────────────────────────── */
#define ECG_WINDOW_SIZE         128         // Số sample mỗi lần inference
#define ECG_WINDOW_OVERLAP      64          // Overlap 50% giữa các window
#define ECG_WINDOW_STEP         (ECG_WINDOW_SIZE - ECG_WINDOW_OVERLAP)

/* ─── GPIO ──────────────────────────────────────────────── */
#define ECG_ADC_CHANNEL         ADC_CHANNEL_0   // GPIO1
#define PIN_LO_PLUS             GPIO_NUM_2
#define PIN_LO_MINUS            GPIO_NUM_3
#define PIN_SDN                 GPIO_NUM_4

/* ─── Display ───────────────────────────────────────────── */
#define DISPLAY_FPS             25
#define DISPLAY_PERIOD_MS       (1000 / DISPLAY_FPS)    // 40ms

/* ─── Network ───────────────────────────────────────────── */
#define NETWORK_BATCH_SIZE      50          // Số sample gửi mỗi gói WebSocket
#define NETWORK_SEND_PERIOD_MS  140         // ~7 gói/giây
#define RING_BUFFER_SECONDS     5           // Lưu 5 giây dữ liệu local
#define RING_BUFFER_SIZE        (ECG_SAMPLE_RATE_HZ * RING_BUFFER_SECONDS)

/* ═══════════════════════════════════════════════════════════
 *  Struct trao đổi giữa các task
 * ═══════════════════════════════════════════════════════════ */

/* Sample thô từ ADC */
typedef struct {
    int64_t  timestamp_us;  // esp_timer_get_time() lúc đọc
    int      raw;           // Giá trị ADC thô 0–4095
    int      voltage_mv;    // Đã qua calibration (0 nếu chưa cali)
    bool     lead_off;      // true nếu điện cực bị bong
} ecg_raw_sample_t;

/* Window đã qua inference — output từ Inference Task */
typedef struct {
    int64_t  timestamp_us;              // Timestamp của sample đầu tiên trong window
    float    cleaned[ECG_WINDOW_SIZE];  // Tín hiệu đã lọc nhiễu
    uint16_t n_samples;                 // Số sample hợp lệ (thường = ECG_WINDOW_SIZE)
    bool     valid;                     // false nếu có lead_off trong window này
} ecg_clean_window_t;

/* Thông tin nhịp tim — từ HR Detection Task */
typedef struct {
    uint8_t  bpm;           // Nhịp tim (beats per minute)
    int64_t  last_peak_us;  // Timestamp đỉnh R gần nhất
    bool     valid;         // false nếu chưa đủ dữ liệu
} ecg_hr_t;

/* ═══════════════════════════════════════════════════════════
 *  Queue handles (định nghĩa trong main.c, extern ở đây)
 *
 *  Luồng dữ liệu:
 *    ADC Task → [raw_queue] → Inference Task
 *                                  ↓
 *               [clean_queue] → Display Task
 *                                  ↓
 *               [clean_queue] → Network Task (đọc cùng queue)
 *                                  ↓
 *               [hr_queue]   → HR Detection → Display / Network
 * ═══════════════════════════════════════════════════════════ */
extern QueueHandle_t raw_queue;    // ecg_raw_sample_t,   capacity = 2×WINDOW_SIZE
extern QueueHandle_t clean_queue;  // ecg_clean_window_t, capacity = 4
extern QueueHandle_t hr_queue;     // ecg_hr_t,           capacity = 4

/* Mutex bảo vệ shared HR value (Display đọc, HR task ghi) */
extern SemaphoreHandle_t hr_mutex;
extern ecg_hr_t          g_current_hr;

/* ═══════════════════════════════════════════════════════════
 *  Task priorities & stack sizes
 * ═══════════════════════════════════════════════════════════ */
#define TASK_PRIO_ADC           5
#define TASK_PRIO_INFERENCE     4
#define TASK_PRIO_HR_DETECT     3
#define TASK_PRIO_DISPLAY       3
#define TASK_PRIO_NETWORK       2

#define TASK_STACK_ADC          4096
#define TASK_STACK_INFERENCE    32768   // TFLite cần nhiều stack
#define TASK_STACK_HR_DETECT    8192
#define TASK_STACK_DISPLAY      8192
#define TASK_STACK_NETWORK      16384

/* Core assignment */
#define TASK_CORE_ADC           0
#define TASK_CORE_INFERENCE     0
#define TASK_CORE_HR_DETECT     0
#define TASK_CORE_DISPLAY       1
#define TASK_CORE_NETWORK       1

#ifdef __cplusplus
}
#endif
