/*
 * ecg_network.c — Network Task (Option A: ESP32 WebSocket Server)
 *
 * SKELETON: Cấu trúc đầy đủ, chưa implement JSON serializer và
 *           ring buffer flush hoàn chỉnh.
 *
 * Dependencies cần thêm vào idf_component.yml:
 *   esp_wifi, esp_event, esp_netif, esp_http_server (built-in IDF)
 */

#include "ecg_network.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ECG_NET";

/* ─── Globals ───────────────────────────────────────────── */
int           g_ws_client_count = 0;
QueueHandle_t network_queue     = NULL;

/* ─── WiFi event ────────────────────────────────────────── */
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRY      5
static int s_retry_count = 0;

/* ─── HTTP server handle ─────────────────────────────────── */
static httpd_handle_t s_server = NULL;

/* ─── Ring buffer (5 giây × 500 Hz × 4 bytes float) ─────── */
#define NET_RING_SIZE   RING_BUFFER_SIZE
static float  s_ring[NET_RING_SIZE];
static int    s_ring_write = 0;
static int    s_ring_count = 0;

/* ════════════════════════════════════════════════════════════
 *  WiFi event handler
 * ════════════════════════════════════════════════════════════ */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
        g_ws_client_count = 0;
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ════════════════════════════════════════════════════════════
 *  WebSocket handler
 * ════════════════════════════════════════════════════════════ */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake — upgrade tự động bởi esp_http_server */
        ESP_LOGI(TAG, "WS client connected, fd=%d", httpd_req_to_sockfd(req));
        g_ws_client_count++;
        return ESP_OK;
    }

    /* Nhận frame từ client (nếu cần bidirectional) */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len > 0) {
        /* TODO: Xử lý lệnh từ client (ví dụ: {"cmd":"pause"}) */
        ESP_LOGI(TAG, "WS recv: %zu bytes", ws_pkt.len);
    }
    return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    .uri          = ECG_WS_URI,
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .is_websocket = true,
};

/* ════════════════════════════════════════════════════════════
 *  Serve web dashboard (index.html nhúng vào firmware)
 *
 *  TODO: Tạo file web/index.html và nhúng vào firmware:
 *    target_add_binary_data(${COMPONENT_LIB} "web/index.html" TEXT)
 *  Sau đó serve qua HTTP GET /
 * ════════════════════════════════════════════════════════════ */
static esp_err_t root_handler(httpd_req_t *req)
{
    /* PLACEHOLDER — trả về HTML tối thiểu */
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<title>ECG Monitor</title>"
        "<script>"
        "const ws = new WebSocket('ws://' + location.host + '/ecg');"
        "ws.onmessage = e => {"
        "  const d = JSON.parse(e.data);"
        "  document.getElementById('bpm').textContent = d.bpm;"
        "};"
        "</script></head><body>"
        "<h2>ECG Monitor</h2>"
        "<p>BPM: <span id='bpm'>--</span></p>"
        "<p>TODO: Thay thế bằng dashboard React/Chart.js thực tế</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t root_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = root_handler,
};

/* ════════════════════════════════════════════════════════════
 *  Broadcast JSON tới tất cả WS clients
 *
 *  TODO: Implement binary format để giảm bandwidth:
 *    struct { uint32_t ts_ms; uint16_t bpm; uint8_t flags;
 *             int16_t samples[NETWORK_BATCH_SIZE]; } __packed;
 * ════════════════════════════════════════════════════════════ */
static void broadcast_ecg(const float *samples, int n, uint8_t bpm, bool lead_off)
{
    if (g_ws_client_count == 0 || s_server == NULL) return;

    /* Build JSON — TODO: dùng cJSON hoặc thư viện nhẹ hơn */
    static char json_buf[1024];
    int pos = snprintf(json_buf, sizeof(json_buf),
                       "{\"t\":%lld,\"fs\":%d,\"bpm\":%u,\"lo\":%s,\"ecg\":[",
                       (long long)(esp_timer_get_time() / 1000),
                       ECG_SAMPLE_RATE_HZ,
                       bpm,
                       lead_off ? "true" : "false");

    for (int i = 0; i < n && pos < (int)sizeof(json_buf) - 20; i++) {
        pos += snprintf(json_buf + pos, sizeof(json_buf) - pos,
                        i < n-1 ? "%.3f," : "%.3f", samples[i]);
    }
    pos += snprintf(json_buf + pos, sizeof(json_buf) - pos, "]}");

    httpd_ws_frame_t ws_pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_buf,
        .len     = (size_t)pos,
    };

    /* Broadcast tới tất cả client */
    /* TODO: Dùng httpd_ws_send_frame_async với task handle để không block */
    /* esp_http_server không có API broadcast sẵn — cần lưu fd list */
    /* Tham khảo: esp-idf/examples/protocols/http_server/ws_echo_server */
    ESP_LOGD(TAG, "TX %d bytes to %d clients", pos, g_ws_client_count);
}

/* ════════════════════════════════════════════════════════════
 *  ecg_network_init
 * ════════════════════════════════════════════════════════════ */
esp_err_t ecg_network_init(void)
{
    s_wifi_eg = xEventGroupCreate();

    /* 1. Init netif */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* 2. WiFi config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = CONFIG_ECG_WIFI_SSID,
            .password = CONFIG_ECG_WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", CONFIG_ECG_WIFI_SSID);

    /* 3. Đợi kết nối (timeout 30s) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_FAIL;
    }

    /* 4. Start HTTP/WebSocket server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port    = ECG_WS_PORT;
    http_cfg.max_open_sockets = ECG_WS_MAX_CLIENTS + 2;

    ESP_ERROR_CHECK(httpd_start(&s_server, &http_cfg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &ws_uri));

    ESP_LOGI(TAG, "WebSocket server started at ws://[IP]:%d%s",
             ECG_WS_PORT, ECG_WS_URI);

    /* 5. Tạo network_queue */
    network_queue = xQueueCreate(8, sizeof(ecg_clean_window_t));
    configASSERT(network_queue);

    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════
 *  ecg_network_task
 * ════════════════════════════════════════════════════════════ */
void ecg_network_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Network task started");

    ecg_clean_window_t win;
    TickType_t         last_send = xTaskGetTickCount();

    /* Batch buffer: tích lũy trước khi gửi */
    static float batch[NETWORK_BATCH_SIZE];
    int          batch_fill = 0;
    bool         batch_lead_off = false;

    while (1) {
        /* ─── Drain network_queue ──────────────────────── */
        while (xQueueReceive(network_queue, &win, 0) == pdTRUE) {
            if (!win.valid) {
                batch_lead_off = true;
            }

            /* Tích lũy vào ring buffer và batch */
            for (int i = 0; i < win.n_samples; i++) {
                /* Ring buffer (5 giây) */
                s_ring[s_ring_write] = win.cleaned[i];
                s_ring_write = (s_ring_write + 1) % NET_RING_SIZE;
                if (s_ring_count < NET_RING_SIZE) s_ring_count++;

                /* Batch cho lần gửi tiếp theo */
                if (batch_fill < NETWORK_BATCH_SIZE) {
                    batch[batch_fill++] = win.cleaned[i];
                }
            }
        }

        /* ─── Gửi theo chu kỳ NETWORK_SEND_PERIOD_MS ──── */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_send) >= pdMS_TO_TICKS(NETWORK_SEND_PERIOD_MS)
            && batch_fill > 0)
        {
            /* Lấy HR snapshot */
            uint8_t bpm = 0;
            if (xSemaphoreTake(hr_mutex, 0) == pdTRUE) {
                bpm = g_current_hr.valid ? g_current_hr.bpm : 0;
                xSemaphoreGive(hr_mutex);
            }

            broadcast_ecg(batch, batch_fill, bpm, batch_lead_off);

            batch_fill      = 0;
            batch_lead_off  = false;
            last_send       = now;
        }

        /* Nhường CPU nếu không có gì để làm */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelete(NULL);
}
