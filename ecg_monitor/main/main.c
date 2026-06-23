/*
 * main.c — app_main: Điều phối toàn bộ hệ thống ECG
 *
 * Thứ tự khởi tạo:
 *   1. Tạo tất cả FreeRTOS queue, mutex, semaphore
 *   2. Khởi tạo từng component (hardware init)
 *   3. Spawn tất cả task theo đúng core + priority
 *   4. app_main thoát (FreeRTOS scheduler tiếp quản)
 *
 * Luồng dữ liệu:
 *   AD8232
 *     └─ [ADC Task, C0P5] ──raw_queue──► [Inference Task, C0P4]
 *                                              │
 *                              ┌───────────────┼─────────────────┐
 *                              │               │                 │
 *                         clean_queue   hr_input_queue    network_queue
 *                              │               │                 │
 *                    [Display C1P3]  [HR Detect C0P3]  [Network C1P2]
 *                              │               │
 *                           OLED          hr_queue ──► Display
 *                                          + g_current_hr (mutex)
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ecg_common.h"
#include "ecg_adc.h"
#include "ecg_inference.h"
#include "ecg_hr_detect.h"
#include "ecg_display.h"
#include "ecg_network.h"
#include "ecg_serial_debug.h"

static const char *TAG = "ECG_MAIN";

/* ════════════════════════════════════════════════════════════
 *  Định nghĩa global (extern trong ecg_common.h)
 * ════════════════════════════════════════════════════════════ */
QueueHandle_t    raw_queue    = NULL;
QueueHandle_t    clean_queue  = NULL;
QueueHandle_t    hr_queue     = NULL;
SemaphoreHandle_t hr_mutex    = NULL;
ecg_hr_t         g_current_hr = {0};

/* ════════════════════════════════════════════════════════════
 *  Inference Task (mở rộng): fan-out clean window tới
 *  hr_input_queue và network_queue sau khi đẩy clean_queue
 *
 *  Lý do tách riêng: Không muốn HR Detect và Network task
 *  cùng đọc clean_queue (chỉ 1 task nhận được mỗi item).
 *  Thay vào đó, Inference Task tự fan-out (copy struct).
 * ════════════════════════════════════════════════════════════ */
static void inference_fanout_task(void *pvParameters)
{
    /*
     * Wrapper: chạy logic inference, sau đó fan-out.
     *
     * Thực tế cần sửa ecg_inference_task để expose clean window
     * thay vì push thẳng vào queue — hoặc dùng pattern này:
     * Inference task push vào internal_clean_queue,
     * fanout task đọc và copy tới 3 destination queue.
     *
     * TODO: Refactor ecg_inference.c để return window qua callback
     *       hoặc expose internal queue để fanout ở đây.
     *
     * HIỆN TẠI: ecg_inference_task push trực tiếp vào clean_queue.
     *           Display task đọc clean_queue.
     *           HR Detect và Network đọc từ queue riêng của chúng
     *           (sẽ được cấp dữ liệu bằng cách sửa ecg_inference.c).
     */
    vTaskDelete(NULL); // Placeholder — xem TODO ở trên
}

/* ════════════════════════════════════════════════════════════
 *  Monitor task (optional, debug only)
 *  In thống kê mỗi 10 giây
 * ════════════════════════════════════════════════════════════ */
#ifdef CONFIG_ECG_ENABLE_MONITOR
static void monitor_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "=== System Stats ===");
        ESP_LOGI(TAG, "ADC overflows:    %lu", g_adc_overflow_count);
        ESP_LOGI(TAG, "Inference windows: %lu", g_inference_window_count);
        ESP_LOGI(TAG, "Inf avg time:      %lu us", g_inference_avg_us);
        ESP_LOGI(TAG, "WS clients:        %d", g_ws_client_count);
        ESP_LOGI(TAG, "raw_queue:         %u/%u",
                 (unsigned)uxQueueMessagesWaiting(raw_queue),
                 (unsigned)uxQueueSpacesAvailable(raw_queue) +
                 (unsigned)uxQueueMessagesWaiting(raw_queue));
        ESP_LOGI(TAG, "clean_queue:       %u msgs waiting",
                 (unsigned)uxQueueMessagesWaiting(clean_queue));
        ESP_LOGI(TAG, "Current HR:        %u bpm (valid=%d)",
                 g_current_hr.bpm, g_current_hr.valid);

        /* Heap */
        ESP_LOGI(TAG, "Free heap:         %lu bytes", esp_get_free_heap_size());
        ESP_LOGI(TAG, "Min free heap:     %lu bytes", esp_get_minimum_free_heap_size());
    }
}
#endif

/* ════════════════════════════════════════════════════════════
 *  app_main
 * ════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ECG Monitor v0.1 (ESP32-S3 + AD8232) ===");
    ESP_LOGI(TAG, "Built: %s %s", __DATE__, __TIME__);

/* ════════════════════════════════════════════════════════════
 *  CHẾ ĐỘ SERIAL DEBUG (Yêu cầu 1)
 *
 *  Khi CONFIG_ECG_SERIAL_DEBUG=y:
 *    - Chỉ khởi tạo ADC + raw_queue
 *    - Spawn ADC task + serial print task
 *    - KHÔNG khởi tạo inference / display / network
 *
 *  Bật bằng:
 *    idf.py menuconfig → ECG Monitor Configuration → Serial Debug Mode
 *  hoặc:
 *    Thêm CONFIG_ECG_SERIAL_DEBUG=y vào sdkconfig.defaults
 * ════════════════════════════════════════════════════════════ */
#ifdef CONFIG_ECG_SERIAL_DEBUG

    ESP_LOGI(TAG, "*** SERIAL DEBUG MODE — inference/display/network DISABLED ***");

    /* ── 1. Chỉ tạo raw_queue ───────────────────────── */
    raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    configASSERT(raw_queue != NULL);
    ESP_LOGI(TAG, "raw_queue created (capacity=%d)", 2 * ECG_WINDOW_SIZE);

    /* ── 2. Khởi tạo ADC ────────────────────────────── */
    ecg_adc_init();
    ESP_LOGI(TAG, "ADC initialized (GPIO1=ADC, GPIO2=LO+, GPIO3=LO-, GPIO4=SDN)");

    /* ── 3. Spawn tasks ─────────────────────────────── */
    BaseType_t ret;

    /* ADC Task — Core 0, Priority 5 */
    ret = xTaskCreatePinnedToCore(
        ecg_adc_task, "ecg_adc",
        TASK_STACK_ADC, NULL, TASK_PRIO_ADC,
        NULL, TASK_CORE_ADC);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "ADC task spawned [Core %d, P%d]", TASK_CORE_ADC, TASK_PRIO_ADC);

    /* Serial Debug Task — Core 1, Priority 2 */
    ret = xTaskCreatePinnedToCore(
        ecg_serial_debug_task, "ecg_dbg",
        4096, NULL, 2,
        NULL, 1);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "Serial debug task spawned [Core 1, P2]");

    ESP_LOGI(TAG, "Serial debug ready — open terminal @ 115200 baud");
    /* app_main thoát — scheduler tiếp quản */

#else /* CONFIG_ECG_SERIAL_DEBUG not set — full system mode */

    ESP_LOGI(TAG, "Sample rate: %d Hz | Window: %d | Overlap: %d",
             ECG_SAMPLE_RATE_HZ, ECG_WINDOW_SIZE, ECG_WINDOW_OVERLAP);

    /* ── 0. NVS flash (cần cho WiFi) ─────────────────── */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    /* ── 1. Tạo FreeRTOS primitives ─────────────────── */
    /*
     * raw_queue: Capacity = 2 × WINDOW_SIZE để buffer khi Inference
     *            đang bận (256 × sizeof(ecg_raw_sample_t) = ~4KB)
     */
    raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    configASSERT(raw_queue != NULL);

    /*
     * clean_queue: Capacity nhỏ — Display drain nhanh mỗi 40ms.
     * sizeof(ecg_clean_window_t) = 128×4 + 12 ≈ 524 bytes
     * 4 phần tử × 524 bytes ≈ 2KB
     */
    clean_queue = xQueueCreate(4, sizeof(ecg_clean_window_t));
    configASSERT(clean_queue != NULL);

    /*
     * hr_queue: Dùng xQueueOverwrite (capacity=1) vì chỉ cần giá trị mới nhất.
     */
    hr_queue = xQueueCreate(1, sizeof(ecg_hr_t));
    configASSERT(hr_queue != NULL);

    /*
     * hr_input_queue: HR Task nhận copy clean window từ Inference Task
     */
    hr_input_queue = xQueueCreate(4, sizeof(ecg_clean_window_t));
    configASSERT(hr_input_queue != NULL);

    /*
     * network_queue: Network Task nhận copy clean window
     */
    network_queue = xQueueCreate(8, sizeof(ecg_clean_window_t));
    configASSERT(network_queue != NULL);

    /* Mutex bảo vệ g_current_hr */
    hr_mutex = xSemaphoreCreateMutex();
    configASSERT(hr_mutex != NULL);

    ESP_LOGI(TAG, "FreeRTOS queues and mutex created");

    /* ── 2. Hardware init ────────────────────────────── */
    ecg_adc_init();
    ESP_LOGI(TAG, "ADC initialized");

    if (!ecg_inference_init()) {
        ESP_LOGE(TAG, "Inference init FAILED — continuing in pass-through mode");
    }

    ecg_hr_detect_init();
    ESP_LOGI(TAG, "HR detector initialized");

    ecg_display_init();
    ESP_LOGI(TAG, "Display initialized");

    /* WiFi + WebSocket — không fatal nếu fail */
    esp_err_t net_ret = ecg_network_init();
    if (net_ret != ESP_OK) {
        ESP_LOGW(TAG, "Network init failed — running offline mode");
    }

    /* ── 3. Spawn FreeRTOS tasks ─────────────────────── */
    /*
     * Thứ tự spawn: task cao priority trước để scheduler
     * nhận task ngay khi tạo xong (không cần delay).
     */

    BaseType_t ret;

    /* ADC Task — Core 0, Priority 5 (cao nhất trong system) */
    ret = xTaskCreatePinnedToCore(
        ecg_adc_task, "ecg_adc",
        TASK_STACK_ADC, NULL, TASK_PRIO_ADC,
        NULL, TASK_CORE_ADC);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "ADC task spawned [Core %d, P%d]", TASK_CORE_ADC, TASK_PRIO_ADC);

    /* Inference Task — Core 0, Priority 4 */
    ret = xTaskCreatePinnedToCore(
        ecg_inference_task, "ecg_inf",
        TASK_STACK_INFERENCE, NULL, TASK_PRIO_INFERENCE,
        NULL, TASK_CORE_INFERENCE);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "Inference task spawned [Core %d, P%d]",
             TASK_CORE_INFERENCE, TASK_PRIO_INFERENCE);

    /* HR Detection Task — Core 0, Priority 3 */
    ret = xTaskCreatePinnedToCore(
        ecg_hr_detect_task, "ecg_hr",
        TASK_STACK_HR_DETECT, NULL, TASK_PRIO_HR_DETECT,
        NULL, TASK_CORE_HR_DETECT);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "HR detect task spawned [Core %d, P%d]",
             TASK_CORE_HR_DETECT, TASK_PRIO_HR_DETECT);

    /* Display Task — Core 1, Priority 3 */
    ret = xTaskCreatePinnedToCore(
        ecg_display_task, "ecg_disp",
        TASK_STACK_DISPLAY, NULL, TASK_PRIO_DISPLAY,
        NULL, TASK_CORE_DISPLAY);
    configASSERT(ret == pdPASS);
    ESP_LOGI(TAG, "Display task spawned [Core %d, P%d]",
             TASK_CORE_DISPLAY, TASK_PRIO_DISPLAY);

    /* Network Task — Core 1, Priority 2 (thấp nhất) */
    if (net_ret == ESP_OK) {
        ret = xTaskCreatePinnedToCore(
            ecg_network_task, "ecg_net",
            TASK_STACK_NETWORK, NULL, TASK_PRIO_NETWORK,
            NULL, TASK_CORE_NETWORK);
        configASSERT(ret == pdPASS);
        ESP_LOGI(TAG, "Network task spawned [Core %d, P%d]",
                 TASK_CORE_NETWORK, TASK_PRIO_NETWORK);
    }

    /* Monitor task (debug) */
#ifdef CONFIG_ECG_ENABLE_MONITOR
    xTaskCreate(monitor_task, "ecg_mon", 4096, NULL, 1, NULL);
    ESP_LOGI(TAG, "Monitor task spawned");
#endif

    ESP_LOGI(TAG, "All tasks spawned — app_main returning (scheduler takes over)");
    /* app_main thoát — FreeRTOS scheduler điều phối các task */

#endif /* CONFIG_ECG_SERIAL_DEBUG */
}
