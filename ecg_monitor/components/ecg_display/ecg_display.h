/*
 * ecg_display.h — Display Task: OLED SSD1306 128×64
 *
 * Vẽ waveform ECG rolling window + BPM + lead-off warning.
 * Chạy @ 25 fps, Core 1, Priority 3.
 *
 * Thư viện OLED: esp_lcd (built-in IDF) + driver SSD1306.
 * Có thể thay bằng u8g2 nếu muốn font đẹp hơn.
 */
#pragma once

#include "ecg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Cấu hình I2C + OLED ───────────────────────────────── */
#define OLED_I2C_PORT       I2C_NUM_0
#define OLED_I2C_SDA        GPIO_NUM_8
#define OLED_I2C_SCL        GPIO_NUM_9
#define OLED_I2C_FREQ_HZ    400000      // 400 kHz Fast mode
#define OLED_I2C_ADDR       0x3C        // SSD1306 default address

#define OLED_WIDTH          128
#define OLED_HEIGHT         64

/* Vùng vẽ waveform: để lại 16px trên cùng cho thông tin */
#define WAVE_Y_OFFSET       16
#define WAVE_HEIGHT         (OLED_HEIGHT - WAVE_Y_OFFSET)
#define WAVE_WIDTH          OLED_WIDTH

/**
 * @brief Khởi tạo I2C + SSD1306.
 */
void ecg_display_init(void);

/**
 * @brief FreeRTOS task: Display Task @ 25 fps.
 *        Core 1, Priority 3.
 *
 *        Mỗi frame:
 *          1. Nhận tối đa N sample từ clean_queue (drain nhanh)
 *          2. Downsample → 128 điểm waveform
 *          3. Lấy HR từ g_current_hr (shared, mutex)
 *          4. Clear buffer → vẽ waveform → vẽ HR text → flush I2C
 */
void ecg_display_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
