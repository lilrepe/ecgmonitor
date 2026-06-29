/*
 * ecg_adc.c — ADC Task
 *
 * Migrate từ doluongecg.c gốc:
 *   - Giữ nguyên cấu trúc gpio_init(), adc_init(), timing esp_timer
 *   - Thay printf → đẩy struct vào raw_queue
 *   - Thêm xử lý queue overflow
 */

#include "ecg_adc.h"
#include "ecg_common.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"

static const char *TAG = "ECG_ADC";

/* Expose variables for unit testing linking */
adc_oneshot_unit_handle_t g_test_adc1_handle    = NULL;
adc_cali_handle_t         g_test_adc1_cali_handle = NULL;
bool                      g_test_adc_calibrated  = false;
#define adc1_handle       g_test_adc1_handle
#define adc1_cali_handle  g_test_adc1_cali_handle
#define adc_calibrated    g_test_adc_calibrated

uint32_t g_adc_overflow_count = 0;

/* ══════════════════════════════════════════════════════════
 *  gpio_init — giống file gốc, không thay đổi
 * ══════════════════════════════════════════════════════════ */
static void gpio_init_internal(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << PIN_LO_PLUS) | (1ULL << PIN_LO_MINUS),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LO GPIO config failed: %s", esp_err_to_name(err));
        return;
    }

    gpio_config_t sdn_cfg = {
        .pin_bit_mask = (1ULL << PIN_SDN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&sdn_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDN GPIO config failed: %s", esp_err_to_name(err));
        return;
    }
    gpio_set_level(PIN_SDN, 1);
    ESP_LOGI(TAG, "GPIO initialized. AD8232 powered ON.");
}

/* ══════════════════════════════════════════════════════════
 *  adc_init — giống file gốc, không thay đổi
 * ══════════════════════════════════════════════════════════ */
static void adc_init_internal(void)
{
    if (adc1_handle != NULL) {
        ESP_LOGD(TAG, "ADC already initialized");
        return;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&unit_cfg, &adc1_handle);
    if (ret != ESP_OK) {
        adc1_handle = NULL;
        ESP_LOGE(TAG, "ADC oneshot unit init failed: %s", esp_err_to_name(ret));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc1_handle, ECG_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adc1_handle);
        adc1_handle = NULL;
        return;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .chan     = ECG_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &adc1_cali_handle);
#else
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_line_fitting(&cali_cfg, &adc1_cali_handle);
#endif

    if (ret == ESP_OK) {
        adc_calibrated = true;
        ESP_LOGI(TAG, "ADC calibration: OK");
    } else {
        adc1_cali_handle = NULL;
        adc_calibrated = false;
        ESP_LOGW(TAG, "ADC calibration: FAILED, using raw values");
    }
}

/* ══════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════ */
void ecg_adc_init(void)
{
    gpio_init_internal();
    adc_init_internal();
}

void ecg_adc_deinit(void)
{
    if (adc1_cali_handle != NULL) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        esp_err_t ret = adc_cali_delete_scheme_curve_fitting(adc1_cali_handle);
#else
        esp_err_t ret = adc_cali_delete_scheme_line_fitting(adc1_cali_handle);
#endif
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC calibration deinit failed: %s", esp_err_to_name(ret));
        }
        adc1_cali_handle = NULL;
    }
    adc_calibrated = false;

    if (adc1_handle != NULL) {
        esp_err_t ret = adc_oneshot_del_unit(adc1_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ADC oneshot unit deinit failed: %s", esp_err_to_name(ret));
        }
        adc1_handle = NULL;
    }
}

/* ══════════════════════════════════════════════════════════
 *  ecg_adc_task
 *
 *  Timing: giữ nguyên logic esp_timer từ file gốc (tích lũy, không drift)
 *  Thay đổi: thay printf bằng xQueueSend vào raw_queue
 * ══════════════════════════════════════════════════════════ */
void ecg_adc_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ADC task started @ %d Hz (period %d us)",
             ECG_SAMPLE_RATE_HZ, ECG_PERIOD_US);

    int64_t next_sample = esp_timer_get_time();

    while (1) {
        int64_t now = esp_timer_get_time();

        if (now < next_sample) {
            /* Chưa đến lúc — nhường CPU, không busy-wait */
            vTaskDelay(1);
            continue;
        }

        /* Tích lũy mốc tiếp theo (tránh drift tích lũy) */
        next_sample += ECG_PERIOD_US;

        /* ─── Đọc lead-off + ADC ─────────────────────── */
        ecg_raw_sample_t sample = {
            .timestamp_us = now,
            .raw          = 0,
            .voltage_mv   = 0,
            .lead_off     = gpio_get_level(PIN_LO_PLUS) || gpio_get_level(PIN_LO_MINUS),
        };

        if (!sample.lead_off) {
            ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ECG_ADC_CHANNEL, &sample.raw));
            if (adc_calibrated) {
                adc_cali_raw_to_voltage(adc1_cali_handle, sample.raw, &sample.voltage_mv);
            }
        }

        /* ─── Đẩy vào queue, không block (0 ticks) ──── */
        if (xQueueSend(raw_queue, &sample, 0) != pdTRUE) {
            g_adc_overflow_count++;
            /* Log mỗi 100 lần drop để không spam */
            if (g_adc_overflow_count % 100 == 0) {
                ESP_LOGW(TAG, "raw_queue overflow! total_drops=%lu", g_adc_overflow_count);
            }
        }
    }
    /* Không bao giờ đến đây */
    adc_oneshot_del_unit(adc1_handle);
    vTaskDelete(NULL);
}
