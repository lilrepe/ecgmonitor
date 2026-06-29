/*
 * ecg_display.c — Display Task
 *
 * Dùng esp_lcd_panel_io_i2c + SSD1306 driver (built-in IDF >= 5.1).
 * Nếu dùng IDF cũ hơn hoặc muốn font đẹp: thay bằng u8g2_esp32_hal.
 *
 * Double-buffer pattern:
 *   - Vẽ vào framebuffer[] trong RAM
 *   - Flush toàn bộ 128×64 = 1024 bytes qua I2C DMA
 *   - Không tear vì flush atomic
 */

#include "ecg_display.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ECG_DISP";

/* ─── Framebuffer 128×64 / 8 = 1024 bytes ──────────────── */
static uint8_t s_fb[OLED_WIDTH * OLED_HEIGHT / 8];

/* ─── Rolling waveform buffer ───────────────────────────── */
static uint8_t s_wave[WAVE_WIDTH];   // Y value (0–WAVE_HEIGHT) cho mỗi cột pixel
static int     s_wave_write = 0;     // Con trỏ ghi (vòng tròn)

/* ════════════════════════════════════════════════════════════
 *  Framebuffer helpers
 *  (Nếu dùng u8g2, bỏ phần này và dùng u8g2_DrawPixel)
 * ════════════════════════════════════════════════════════════ */
static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    int byte_idx = (y / 8) * OLED_WIDTH + x;
    int bit_idx  = y % 8;
    if (on) {
        s_fb[byte_idx] |=  (1 << bit_idx);
    } else {
        s_fb[byte_idx] &= ~(1 << bit_idx);
    }
}

static void fb_draw_hline(int x, int y, int len)
{
    for (int i = 0; i < len; i++) fb_set_pixel(x + i, y, true);
}

/* ─── Vẽ số đơn giản (3×5 pixel, không cần font library) ── */
/* TODO: Thay bằng u8g2_DrawStr nếu dùng u8g2              */
static const uint8_t DIGIT_5X3[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b001, 0b001, 0b001}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
};

static void fb_draw_digit(int x, int y, int digit)
{
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (DIGIT_5X3[digit][row] & (1 << (2 - col))) {
                fb_set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void fb_draw_number(int x, int y, uint8_t num)
{
    /* Vẽ tối đa 3 chữ số (0–255) */
    if (num >= 100) fb_draw_digit(x,      y, num / 100);
    if (num >= 10)  fb_draw_digit(x + 4,  y, (num / 10) % 10);
    fb_draw_digit(x + 8, y, num % 10);
}

/* ════════════════════════════════════════════════════════════
 *  SSD1306 I2C helpers (dùng driver/i2c.h legacy API của IDF)
 *
 *  TODO: Migrate sang esp_lcd_panel_io_i2c khi IDF >= 5.1
 *        để dùng DMA và tránh blocking.
 * ════════════════════════════════════════════════════════════ */
static void ssd1306_write_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {0x00, cmd};
    i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR, buf, 2,
                               pdMS_TO_TICKS(10));
}

static void ssd1306_flush(void)
{
    /* Set column/page address rồi bulk-write framebuffer */
    ssd1306_write_cmd(0x21); ssd1306_write_cmd(0); ssd1306_write_cmd(127); // Col
    ssd1306_write_cmd(0x22); ssd1306_write_cmd(0); ssd1306_write_cmd(7);   // Page

    /* Gửi framebuffer với control byte 0x40 (data stream). */
    static uint8_t tx_buf[1 + sizeof(s_fb)];
    tx_buf[0] = 0x40;
    memcpy(&tx_buf[1], s_fb, sizeof(s_fb));
    i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR, tx_buf, sizeof(tx_buf),
                               pdMS_TO_TICKS(50));
}

/* ════════════════════════════════════════════════════════════
 *  ecg_display_init
 * ════════════════════════════════════════════════════════════ */
void ecg_display_init(void)
{
    /* 1. Khởi tạo I2C master */
    i2c_config_t i2c_cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = OLED_I2C_SDA,
        .scl_io_num       = OLED_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &i2c_cfg));
    ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    /* 2. Init SSD1306 */
    vTaskDelay(pdMS_TO_TICKS(100)); // Đợi OLED power-on
    ssd1306_write_cmd(0xAE); // Display OFF
    ssd1306_write_cmd(0xD5); ssd1306_write_cmd(0x80); // Clock div
    ssd1306_write_cmd(0xA8); ssd1306_write_cmd(0x3F); // Multiplex 64
    ssd1306_write_cmd(0xD3); ssd1306_write_cmd(0x00); // Display offset
    ssd1306_write_cmd(0x40);                           // Start line 0
    ssd1306_write_cmd(0x8D); ssd1306_write_cmd(0x14); // Charge pump ON
    ssd1306_write_cmd(0x20); ssd1306_write_cmd(0x00); // Horizontal addressing
    ssd1306_write_cmd(0xA1);                           // Segment remap
    ssd1306_write_cmd(0xC8);                           // COM scan direction
    ssd1306_write_cmd(0xDA); ssd1306_write_cmd(0x12); // COM pins
    ssd1306_write_cmd(0x81); ssd1306_write_cmd(0xCF); // Contrast
    ssd1306_write_cmd(0xD9); ssd1306_write_cmd(0xF1); // Pre-charge
    ssd1306_write_cmd(0xDB); ssd1306_write_cmd(0x40); // VCOMH
    ssd1306_write_cmd(0xA4);                           // Display RAM
    ssd1306_write_cmd(0xA6);                           // Normal display
    ssd1306_write_cmd(0xAF);                           // Display ON

    memset(s_wave, WAVE_HEIGHT / 2, sizeof(s_wave));   // Waveform ở giữa
    ESP_LOGI(TAG, "OLED SSD1306 initialized (%dx%d)", OLED_WIDTH, OLED_HEIGHT);
}

/* ════════════════════════════════════════════════════════════
 *  ecg_display_task
 * ════════════════════════════════════════════════════════════ */
void ecg_display_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Display task started @ %d fps", DISPLAY_FPS);

    ecg_clean_window_t win;
    TickType_t         last_frame = xTaskGetTickCount();

    while (1) {
        /* ─── Drain clean_queue: lấy tất cả window available ── */
        while (xQueueReceive(clean_queue, &win, 0) == pdTRUE) {
            if (!win.valid) continue;

            /* Downsample: lấy đều ECG_WINDOW_STEP sample, map vào cột pixel */
            int step = win.n_samples / WAVE_WIDTH;
            if (step < 1) step = 1;

            for (int i = 0; i < win.n_samples; i += step) {
                /* Map float (0.0–1.0) → Y pixel (inverted vì OLED Y tăng xuống) */
                float norm = win.cleaned[i];
                if (norm < 0.0f) norm = 0.0f;
                if (norm > 1.0f) norm = 1.0f;
                uint8_t y = (uint8_t)((1.0f - norm) * (WAVE_HEIGHT - 1));
                s_wave[s_wave_write % WAVE_WIDTH] = y;
                s_wave_write++;
            }
        }

        /* ─── Render frame ─────────────────────────────────── */
        fb_clear();

        /* Vẽ waveform rolling */
        int write_pos = s_wave_write % WAVE_WIDTH;
        for (int x = 0; x < WAVE_WIDTH; x++) {
            int buf_idx = (write_pos + x) % WAVE_WIDTH;
            int y = WAVE_Y_OFFSET + s_wave[buf_idx];
            fb_set_pixel(x, y, true);
        }

        /* Separator line */
        fb_draw_hline(0, WAVE_Y_OFFSET - 1, WAVE_WIDTH);

        /* HR display */
        ecg_hr_t hr_snap;
        if (xSemaphoreTake(hr_mutex, 0) == pdTRUE) {
            hr_snap = g_current_hr;
            xSemaphoreGive(hr_mutex);
        }

        if (hr_snap.valid) {
            /* "BPM: 72" ở góc trên trái */
            fb_draw_number(0, 4, hr_snap.bpm);
        } else {
            /* Lead-off indicator: "--" */
            fb_draw_hline(0, 6, 6);
            fb_draw_hline(8, 6, 6);
        }

        /* Flush tới OLED */
        ssd1306_flush();

        /* ─── Đợi đến frame tiếp theo (25 fps) ──────────── */
        vTaskDelayUntil(&last_frame, pdMS_TO_TICKS(DISPLAY_PERIOD_MS));
    }

    vTaskDelete(NULL);
}
