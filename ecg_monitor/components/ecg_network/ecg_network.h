/*
 * ecg_network.h — Network Task: WiFi + WebSocket server
 *
 * ╔══════════════════════════════════════════════════════════╗
 * ║  THIẾT KẾ ĐỀ XUẤT — 3 PHƯƠNG ÁN                       ║
 * ║                                                          ║
 * ║  OPTION A: ESP32 làm WebSocket SERVER                   ║
 * ║    Browser kết nối thẳng vào IP của ESP32               ║
 * ║    + Không cần server bên ngoài                         ║
 * ║    + Latency thấp (local LAN)                           ║
 * ║    - ESP32 phải ở cùng mạng WiFi với browser            ║
 * ║    - esp_http_server hỗ trợ WebSocket built-in (IDF>=5) ║
 * ║                                                          ║
 * ║  OPTION B: ESP32 làm MQTT CLIENT → Node.js broker       ║
 * ║    ESP32 → MQTT broker (local hoặc cloud) → Web dashboard║
 * ║    + Nhiều device → 1 dashboard                         ║
 * ║    + Offline buffer dễ hơn                              ║
 * ║    - Cần thêm broker (Mosquitto/EMQX)                   ║
 * ║    - Latency cao hơn 1 hop                              ║
 * ║                                                          ║
 * ║  OPTION C: ESP32 làm HTTP POST → REST API backend       ║
 * ║    Đơn giản nhất, không cần WebSocket                   ║
 * ║    + Dễ implement                                        ║
 * ║    + Dễ store vào database                              ║
 * ║    - Pull-based → latency cao, không realtime           ║
 * ║                                                          ║
 * ║  KHUYẾN NGHỊ: OPTION A (WebSocket Server)               ║
 * ║    Lý do: realtime nhất, không cần infrastructure       ║
 * ║    Skeleton hiện tại implement Option A                  ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * WiFi credentials → sdkconfig (menuconfig) hoặc Provisioning BLE.
 */
#pragma once

#include "ecg_common.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── WiFi credentials (overridden by menuconfig) ──────── */
#ifndef CONFIG_ECG_WIFI_SSID
#define CONFIG_ECG_WIFI_SSID    "YOUR_SSID"
#endif
#ifndef CONFIG_ECG_WIFI_PASS
#define CONFIG_ECG_WIFI_PASS    "YOUR_PASSWORD"
#endif

/* ─── WebSocket server ──────────────────────────────────── */
#define ECG_WS_PORT             80
#define ECG_WS_MAX_CLIENTS      4
#define ECG_WS_URI              "/ecg"

/*
 * Format JSON gửi lên web (1 gói = NETWORK_BATCH_SIZE samples):
 * {
 *   "t":   1234567890,        // timestamp ms (epoch) của sample đầu tiên
 *   "fs":  500,               // sample rate Hz
 *   "ecg": [0.12, 0.14, ...], // mảng float cleaned ECG (NETWORK_BATCH_SIZE phần tử)
 *   "bpm": 72,                // nhịp tim hiện tại (0 nếu chưa valid)
 *   "lo":  false              // lead-off status
 * }
 *
 * Binary format thay thế (nhỏ hơn ~6x):
 * [4 bytes: timestamp_ms] [2 bytes: bpm] [1 byte: flags] [N×2 bytes: int16 ECG samples]
 * → NETWORK_BATCH_SIZE=50: 50×2 + 7 = 107 bytes/gói vs ~400 bytes JSON
 * TODO: Implement binary format khi cần tiết kiệm bandwidth
 */

/**
 * @brief Khởi tạo WiFi station mode, connect, start WebSocket server.
 *        Block cho đến khi có IP (hoặc timeout 30s).
 *
 * @return ESP_OK nếu kết nối thành công
 */
esp_err_t ecg_network_init(void);

/**
 * @brief FreeRTOS task: Network Task.
 *        Core 1, Priority 2.
 *
 *        Flow:
 *          1. Drain network_queue (copy của clean windows)
 *          2. Tích lũy vào ring buffer RAM (5 giây)
 *          3. Mỗi NETWORK_SEND_PERIOD_MS: pack JSON và broadcast tới tất cả WS clients
 *          4. WiFi disconnect → tiếp tục buffer, retry kết nối trong background
 */
void ecg_network_task(void *pvParameters);

/**
 * @brief Số client WebSocket đang kết nối.
 */
extern int g_ws_client_count;

/* Queue riêng cho network (copy từ Inference Task, tránh chặn Display) */
extern QueueHandle_t network_queue;

#ifdef __cplusplus
}
#endif
