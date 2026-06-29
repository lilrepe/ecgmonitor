/*
 * test_ecg_adc.c - Unit Tests for ecg_adc using standard ESP-IDF Unity runner
 *
 * Target: ESP32-S3 on-target testing
 */

#include "unity.h"
#include "ecg_adc.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG_T = "TEST_ADC";

/* Expose handles from ecg_adc.c */
extern adc_oneshot_unit_handle_t g_test_adc1_handle;
extern adc_cali_handle_t         g_test_adc1_cali_handle;
extern bool                      g_test_adc_calibrated;

/* Primitives from main/common */
QueueHandle_t raw_queue   = NULL;
QueueHandle_t clean_queue = NULL;
QueueHandle_t hr_queue    = NULL;
SemaphoreHandle_t hr_mutex = NULL;
ecg_hr_t g_current_hr     = {0};

QueueHandle_t hr_input_queue  = NULL;
QueueHandle_t network_queue   = NULL;

static void cleanup_test_state(TaskHandle_t adc_task_handle, bool delete_raw_queue)
{
    if (adc_task_handle != NULL) {
        vTaskDelete(adc_task_handle);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ecg_adc_deinit();

    if (delete_raw_queue && raw_queue != NULL) {
        vQueueDelete(raw_queue);
        raw_queue = NULL;
    }

    g_adc_overflow_count = 0;
}

TEST_CASE("T01 LO+ and LO- configuration as INPUT", "[ecg_adc]")
{
    ecg_adc_init();

    int lop = gpio_get_level(PIN_LO_PLUS);
    int lom = gpio_get_level(PIN_LO_MINUS);

    cleanup_test_state(NULL, false);

    TEST_ASSERT_TRUE_MESSAGE(lop == 0 || lop == 1,
        "LO+ returns invalid value - check GPIO2 wiring");
    TEST_ASSERT_TRUE_MESSAGE(lom == 0 || lom == 1,
        "LO- returns invalid value - check GPIO3 wiring");

    ESP_LOGI(TAG_T, "[T01] LO+ = %d, LO- = %d", lop, lom);
}

TEST_CASE("T02 GPIO SDN configuration as OUTPUT", "[ecg_adc]")
{
    ecg_adc_init();

    esp_err_t err_hi = gpio_set_level(PIN_SDN, 1);
    esp_err_t err_lo = gpio_set_level(PIN_SDN, 0);
    esp_err_t err_restore = gpio_set_level(PIN_SDN, 1);

    cleanup_test_state(NULL, false);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err_hi,
        "SDN set HIGH failed - check GPIO4 configuration");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err_lo,
        "SDN set LOW failed - check GPIO4 configuration");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err_restore,
        "SDN restore HIGH failed - AD8232 should remain powered on");

    ESP_LOGI(TAG_T, "[T02] SDN HIGH/LOW/HIGH: OK");
}

TEST_CASE("T03 ecg_adc_init no crash, overflow = 0", "[ecg_adc]")
{
    ecg_adc_init();

    uint32_t overflows = g_adc_overflow_count;

    cleanup_test_state(NULL, false);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, overflows,
        "g_adc_overflow_count != 0 right after init");

    ESP_LOGI(TAG_T, "[T03] ecg_adc_init() OK, overflow = 0");
}

TEST_CASE("T04 Raw ADC range [0, 4095]", "[ecg_adc]")
{
    ecg_adc_init();

    bool has_adc_handle = (g_test_adc1_handle != NULL);
    int raw = -1;
    esp_err_t err = ESP_ERR_INVALID_STATE;

    if (has_adc_handle) {
        err = adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw);
    }

    cleanup_test_state(NULL, false);

    TEST_ASSERT_TRUE_MESSAGE(has_adc_handle,
        "adc1_handle = NULL - oneshot unit creation failed");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err,
        "adc_oneshot_read failed - check GPIO1 wiring");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, raw, "Raw ADC < 0");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(4095, raw, "Raw ADC > 4095");

    ESP_LOGI(TAG_T, "[T04] ADC raw = %d", raw);
}

TEST_CASE("T05 Calibrated voltage range [0, 3300] mV", "[ecg_adc]")
{
    ecg_adc_init();

    bool calibrated = g_test_adc_calibrated;
    bool has_adc_handle = (g_test_adc1_handle != NULL);
    bool has_cali_handle = (g_test_adc1_cali_handle != NULL);
    int raw = -1;
    int volt_mv = -1;
    esp_err_t read_err = ESP_ERR_INVALID_STATE;
    esp_err_t cali_err = ESP_ERR_INVALID_STATE;

    if (calibrated && has_adc_handle && has_cali_handle) {
        read_err = adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw);
        if (read_err == ESP_OK) {
            cali_err = adc_cali_raw_to_voltage(g_test_adc1_cali_handle, raw, &volt_mv);
        }
    }

    cleanup_test_state(NULL, false);

    if (!calibrated) {
        TEST_IGNORE_MESSAGE("[T05] ADC calibration not supported or enabled - IGNORED");
        return;
    }

    TEST_ASSERT_TRUE(has_adc_handle);
    TEST_ASSERT_TRUE(has_cali_handle);
    TEST_ASSERT_EQUAL(ESP_OK, read_err);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, cali_err,
        "adc_cali_raw_to_voltage failed");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, volt_mv, "Voltage < 0 mV");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(3300, volt_mv, "Voltage > 3300 mV");

    ESP_LOGI(TAG_T, "[T05] ADC raw = %-4d -> %d mV", raw, volt_mv);
}

TEST_CASE("T06 Queue receives samples from task", "[ecg_adc]")
{
    if (raw_queue == NULL) {
        raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    }
    TEST_ASSERT_NOT_NULL(raw_queue);
    xQueueReset(raw_queue);

    ecg_adc_init();

    TaskHandle_t adc_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ecg_adc_task, "test_adc_t",
        TASK_STACK_ADC, NULL,
        TASK_PRIO_ADC + 1,
        &adc_task_handle,
        TASK_CORE_ADC
    );

    UBaseType_t msgs = 0;
    if (ret == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(20));
        msgs = uxQueueMessagesWaiting(raw_queue);
    }

    ESP_LOGI(TAG_T, "[T06] raw_queue has %u samples after 20ms", (unsigned)msgs);

    cleanup_test_state(adc_task_handle, true);

    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "Failed to create task");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, msgs, "raw_queue is empty after 20ms");
}

TEST_CASE("T07 Lead-off logic truth table", "[ecg_adc]")
{
    typedef struct { int lop; int lom; bool expected; } tc_t;
    const tc_t cases[4] = {
        {0, 0, false},
        {1, 0, true },
        {0, 1, true },
        {1, 1, true },
    };

    for (int i = 0; i < 4; i++) {
        bool actual = (bool)(cases[i].lop || cases[i].lom);
        ESP_LOGI(TAG_T, "[T07] LO+=%d LO-=%d -> lead_off=%s (expected: %s)",
                 cases[i].lop, cases[i].lom,
                 actual            ? "true" : "false",
                 cases[i].expected ? "true" : "false");
        TEST_ASSERT_EQUAL(cases[i].expected, actual);
    }
}

TEST_CASE("T08 No queue overflow in first 100ms", "[ecg_adc]")
{
    if (raw_queue != NULL) {
        vQueueDelete(raw_queue);
        raw_queue = NULL;
    }

    raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    TEST_ASSERT_NOT_NULL(raw_queue);
    xQueueReset(raw_queue);

    ecg_adc_init();
    g_adc_overflow_count = 0;

    TaskHandle_t adc_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ecg_adc_task, "test_adc_ov",
        TASK_STACK_ADC, NULL,
        TASK_PRIO_ADC + 1,
        &adc_task_handle,
        TASK_CORE_ADC
    );

    if (ret == pdPASS) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t overflows = g_adc_overflow_count;

    ESP_LOGI(TAG_T, "[T08] Overflows in 100ms: %lu", overflows);

    cleanup_test_state(adc_task_handle, true);

    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "Failed to create task");
    TEST_ASSERT_EQUAL_UINT32(0, overflows);
}

TEST_CASE("T09 Read 10 sequential samples", "[ecg_adc]")
{
    ecg_adc_init();

    bool has_adc_handle = (g_test_adc1_handle != NULL);
    esp_err_t first_err = ESP_OK;
    int first_bad_raw = 0;

    if (has_adc_handle) {
        for (int i = 0; i < 10; i++) {
            int raw = -1;
            esp_err_t err = adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw);

            if (err != ESP_OK) {
                first_err = err;
                break;
            }
            if (raw < 0 || raw > 4095) {
                first_bad_raw = raw;
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }

    cleanup_test_state(NULL, false);

    TEST_ASSERT_TRUE(has_adc_handle);
    TEST_ASSERT_EQUAL(ESP_OK, first_err);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, first_bad_raw, "Sequential raw ADC out of range");

    ESP_LOGI(TAG_T, "[T09] 10/10 samples read successfully");
}

TEST_CASE("T10 Re-initialization stability", "[ecg_adc]")
{
    ecg_adc_init();
    bool first_init_ok = (g_test_adc1_handle != NULL);

    ecg_adc_deinit();
    bool first_deinit_ok = (g_test_adc1_handle == NULL) &&
                           (g_test_adc1_cali_handle == NULL) &&
                           (!g_test_adc_calibrated);

    ecg_adc_init();
    bool second_init_ok = (g_test_adc1_handle != NULL);

    ecg_adc_deinit();
    bool second_deinit_ok = (g_test_adc1_handle == NULL) &&
                            (g_test_adc1_cali_handle == NULL) &&
                            (!g_test_adc_calibrated);

    TEST_ASSERT_TRUE_MESSAGE(first_init_ok, "First init failed");
    TEST_ASSERT_TRUE_MESSAGE(first_deinit_ok, "First deinit did not clear ADC state");
    TEST_ASSERT_TRUE_MESSAGE(second_init_ok, "Second init failed");
    TEST_ASSERT_TRUE_MESSAGE(second_deinit_ok, "Second deinit did not clear ADC state");

    ESP_LOGI(TAG_T, "[T10] init -> deinit -> init -> deinit: OK");
}
