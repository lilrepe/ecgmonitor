/*
 * ecg_inference.c - ECG denoise task.
 *
 * Backend 1, default: classic DSP filters.
 *   raw ADC -> normalize -> high-pass 0.5 Hz -> notch 50 Hz -> low-pass 40 Hz
 *
 * Backend 2: AI placeholder.
 *   raw ADC -> normalize -> pass-through. This keeps the API and task graph
 *   ready for a future TensorFlow Lite Micro or ESP-DL model.
 */

#include "ecg_inference.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>

static const char *TAG = "ECG_DENOISE";

#define CLASSIC_HIGHPASS_HZ  0.5f
#define CLASSIC_NOTCH_HZ     50.0f
#define CLASSIC_NOTCH_Q      30.0f
#define CLASSIC_LOWPASS_HZ   40.0f
#define CLASSIC_FILTER_Q     0.70710678f
#define CLASSIC_OUTPUT_GAIN  1.4f
#define PI_F                 3.14159265358979323846f

uint32_t g_inference_window_count = 0;
uint32_t g_inference_avg_us       = 0;

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
} biquad_t;

static float    s_window_raw[ECG_WINDOW_SIZE];
static int64_t  s_window_ts[ECG_WINDOW_SIZE];
static bool     s_window_lead_off[ECG_WINDOW_SIZE];
static uint16_t s_window_fill = 0;

static biquad_t s_hp;
static biquad_t s_notch;
static biquad_t s_lp;
static bool     s_ai_warning_logged = false;

ecg_denoise_backend_t ecg_inference_get_backend(void)
{
#if defined(CONFIG_ECG_DENOISE_AI)
    return ECG_DENOISE_BACKEND_AI_PLACEHOLDER;
#else
    return ECG_DENOISE_BACKEND_CLASSIC;
#endif
}

const char *ecg_inference_get_backend_name(void)
{
    return ecg_inference_get_backend() == ECG_DENOISE_BACKEND_AI_PLACEHOLDER
        ? "ai-placeholder"
        : "classic-dsp";
}

static void biquad_reset(biquad_t *f)
{
    f->z1 = 0.0f;
    f->z2 = 0.0f;
}

static void biquad_set_lowpass(biquad_t *f, float fs_hz, float cutoff_hz, float q)
{
    float omega = 2.0f * PI_F * cutoff_hz / fs_hz;
    float sn = sinf(omega);
    float cs = cosf(omega);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;

    f->b0 = ((1.0f - cs) * 0.5f) / a0;
    f->b1 = (1.0f - cs) / a0;
    f->b2 = ((1.0f - cs) * 0.5f) / a0;
    f->a1 = (-2.0f * cs) / a0;
    f->a2 = (1.0f - alpha) / a0;
    biquad_reset(f);
}

static void biquad_set_highpass(biquad_t *f, float fs_hz, float cutoff_hz, float q)
{
    float omega = 2.0f * PI_F * cutoff_hz / fs_hz;
    float sn = sinf(omega);
    float cs = cosf(omega);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;

    f->b0 = ((1.0f + cs) * 0.5f) / a0;
    f->b1 = -(1.0f + cs) / a0;
    f->b2 = ((1.0f + cs) * 0.5f) / a0;
    f->a1 = (-2.0f * cs) / a0;
    f->a2 = (1.0f - alpha) / a0;
    biquad_reset(f);
}

static void biquad_set_notch(biquad_t *f, float fs_hz, float notch_hz, float q)
{
    float omega = 2.0f * PI_F * notch_hz / fs_hz;
    float sn = sinf(omega);
    float cs = cosf(omega);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;

    f->b0 = 1.0f / a0;
    f->b1 = (-2.0f * cs) / a0;
    f->b2 = 1.0f / a0;
    f->a1 = (-2.0f * cs) / a0;
    f->a2 = (1.0f - alpha) / a0;
    biquad_reset(f);
}

static float biquad_process(biquad_t *f, float x)
{
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static bool window_has_lead_off(void)
{
    for (int i = 0; i < ECG_WINDOW_SIZE; i++) {
        if (s_window_lead_off[i]) {
            return true;
        }
    }
    return false;
}

static void normalize_window(float *samples, int n)
{
    float min_v = samples[0];
    float max_v = samples[0];

    for (int i = 1; i < n; i++) {
        if (samples[i] < min_v) {
            min_v = samples[i];
        }
        if (samples[i] > max_v) {
            max_v = samples[i];
        }
    }

    float span = max_v - min_v;
    if (span < 0.0001f) {
        for (int i = 0; i < n; i++) {
            samples[i] = 0.5f;
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        samples[i] = (samples[i] - min_v) / span;
    }
}

static bool run_classic_filters(const float *input, float *output)
{
    for (int i = 0; i < ECG_WINDOW_SIZE; i++) {
        float x = input[i] - 0.5f;
        float y = biquad_process(&s_hp, x);
        y = biquad_process(&s_notch, y);
        y = biquad_process(&s_lp, y);
        output[i] = y * CLASSIC_OUTPUT_GAIN;
    }

    normalize_window(output, ECG_WINDOW_SIZE);
    return true;
}

static bool run_ai_placeholder(const float *input, float *output)
{
    if (!s_ai_warning_logged) {
        ESP_LOGW(TAG, "AI denoise selected but no model is integrated; using pass-through");
        s_ai_warning_logged = true;
    }

    /*
     * Future model hook:
     *   1. Embed the .tflite/.espdl model with target_add_binary_data().
     *   2. Allocate tensor/model arena, preferably in PSRAM.
     *   3. Quantize/copy input[ECG_WINDOW_SIZE] to model input tensor.
     *   4. Invoke the model and copy its output to output[].
     */
    memcpy(output, input, ECG_WINDOW_SIZE * sizeof(float));
    return true;
}

static bool denoise_window(const float *input, float *output)
{
    if (ecg_inference_get_backend() == ECG_DENOISE_BACKEND_AI_PLACEHOLDER) {
        return run_ai_placeholder(input, output);
    }

    return run_classic_filters(input, output);
}

static void publish_clean_window(const ecg_clean_window_t *clean)
{
    if (xQueueSend(clean_queue, clean, pdMS_TO_TICKS(5)) != pdTRUE) {
        ESP_LOGW(TAG, "clean_queue full, drop display window #%lu",
                 g_inference_window_count);
    }

    if (hr_input_queue != NULL &&
        xQueueSend(hr_input_queue, clean, 0) != pdTRUE) {
        ESP_LOGW(TAG, "hr_input_queue full, drop HR window #%lu",
                 g_inference_window_count);
    }

    if (network_queue != NULL &&
        xQueueSend(network_queue, clean, 0) != pdTRUE) {
        ESP_LOGW(TAG, "network_queue full, drop network window #%lu",
                 g_inference_window_count);
    }
}

bool ecg_inference_init(void)
{
    memset(s_window_raw, 0, sizeof(s_window_raw));
    memset(s_window_ts, 0, sizeof(s_window_ts));
    memset(s_window_lead_off, 0, sizeof(s_window_lead_off));
    s_window_fill = 0;
    s_ai_warning_logged = false;

    biquad_set_highpass(&s_hp, ECG_SAMPLE_RATE_HZ, CLASSIC_HIGHPASS_HZ,
                        CLASSIC_FILTER_Q);
    biquad_set_notch(&s_notch, ECG_SAMPLE_RATE_HZ, CLASSIC_NOTCH_HZ,
                     CLASSIC_NOTCH_Q);
    biquad_set_lowpass(&s_lp, ECG_SAMPLE_RATE_HZ, CLASSIC_LOWPASS_HZ,
                       CLASSIC_FILTER_Q);

    ESP_LOGI(TAG, "Denoise backend: %s", ecg_inference_get_backend_name());
    if (ecg_inference_get_backend() == ECG_DENOISE_BACKEND_CLASSIC) {
        ESP_LOGI(TAG, "Classic filters: HP %.1f Hz, notch %.1f Hz, LP %.1f Hz",
                 CLASSIC_HIGHPASS_HZ, CLASSIC_NOTCH_HZ, CLASSIC_LOWPASS_HZ);
    }

    return true;
}

void ecg_inference_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ECG denoise task started (window=%d, step=%d, backend=%s)",
             ECG_WINDOW_SIZE, ECG_WINDOW_STEP, ecg_inference_get_backend_name());

    ecg_raw_sample_t raw;
    ecg_clean_window_t clean;

    while (1) {
        if (xQueueReceive(raw_queue, &raw, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_window_raw[s_window_fill] = (float)raw.raw / 4095.0f;
        s_window_ts[s_window_fill] = raw.timestamp_us;
        s_window_lead_off[s_window_fill] = raw.lead_off;
        s_window_fill++;

        if (s_window_fill >= ECG_WINDOW_SIZE) {
            int64_t t_start = esp_timer_get_time();

            clean.timestamp_us = s_window_ts[0];
            clean.n_samples = ECG_WINDOW_SIZE;
            clean.valid = !window_has_lead_off();

            if (!denoise_window(s_window_raw, clean.cleaned)) {
                memcpy(clean.cleaned, s_window_raw,
                       ECG_WINDOW_SIZE * sizeof(float));
                clean.valid = false;
            }

            int64_t t_end = esp_timer_get_time();
            uint32_t elapsed_us = (uint32_t)(t_end - t_start);

            g_inference_window_count++;
            g_inference_avg_us = (g_inference_avg_us * 7 + elapsed_us) / 8;

            publish_clean_window(&clean);

            memmove(s_window_raw,
                    s_window_raw + ECG_WINDOW_STEP,
                    ECG_WINDOW_OVERLAP * sizeof(float));
            memmove(s_window_ts,
                    s_window_ts + ECG_WINDOW_STEP,
                    ECG_WINDOW_OVERLAP * sizeof(int64_t));
            memmove(s_window_lead_off,
                    s_window_lead_off + ECG_WINDOW_STEP,
                    ECG_WINDOW_OVERLAP * sizeof(bool));
            s_window_fill = ECG_WINDOW_OVERLAP;

            if (g_inference_window_count % 100 == 0) {
                ESP_LOGI(TAG, "windows=%lu avg_process=%lu us backend=%s",
                         g_inference_window_count,
                         g_inference_avg_us,
                         ecg_inference_get_backend_name());
            }
        }
    }

    vTaskDelete(NULL);
}
