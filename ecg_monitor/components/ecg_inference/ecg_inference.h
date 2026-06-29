/*
 * ecg_inference.h - ECG denoise task.
 *
 * The public name is kept as "inference" because the rest of the project
 * already routes raw samples through this task. Internally it now supports
 * two denoise backends:
 *   1. Classic DSP filters: high-pass, notch, low-pass.
 *   2. AI placeholder: a future model hook, currently pass-through.
 */
#pragma once

#include "ecg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ECG_DENOISE_BACKEND_CLASSIC = 0,
    ECG_DENOISE_BACKEND_AI_PLACEHOLDER,
} ecg_denoise_backend_t;

/**
 * @brief Initialize the selected ECG denoise backend.
 *
 * Classic mode initializes IIR filter state and coefficients.
 * AI mode initializes only the placeholder hook so the project still builds.
 *
 * @return true when the selected backend is ready.
 */
bool ecg_inference_init(void);

/**
 * @brief FreeRTOS task: raw_queue -> denoise -> clean/hr/network queues.
 *
 * Flow:
 *   1. Read raw_queue and collect a sliding window.
 *   2. Run the selected denoise backend.
 *   3. Publish the clean window to display, HR, and network queues.
 *   4. Slide by ECG_WINDOW_STEP while keeping ECG_WINDOW_OVERLAP samples.
 */
void ecg_inference_task(void *pvParameters);

/**
 * @brief Return selected backend enum/name for logs or diagnostics.
 */
ecg_denoise_backend_t ecg_inference_get_backend(void);
const char *ecg_inference_get_backend_name(void);

/**
 * @brief Number of processed windows and average processing time in us.
 */
extern uint32_t g_inference_window_count;
extern uint32_t g_inference_avg_us;

#ifdef __cplusplus
}
#endif
