# ecg_adc

Module nay doc tin hieu tu AD8232 bang ADC1 va day sample tho vao `raw_queue`.

## Luong logic

1. `ecg_adc_init()` cau hinh:
   - GPIO LO+ va LO- lam input de phat hien dien cuc bi bong.
   - GPIO SDN lam output va set HIGH de bat AD8232.
   - ADC1 one-shot tren `ADC_CHANNEL_0` tuong ung GPIO1.
   - ADC calibration neu chip/IDF ho tro.
2. `ecg_adc_task()` chay vong lap 500 Hz dua tren `esp_timer_get_time()`.
3. Moi chu ky, task doc lead-off, doc raw ADC, tinh voltage mV neu co calibration.
4. Task day `ecg_raw_sample_t` vao `raw_queue`.
5. Neu queue day, sample bi drop va `g_adc_overflow_count` tang.

## Tuong tac voi project

Output cua module la `raw_queue`. `ecg_inference_task()` la consumer cua queue nay. Khi bat `CONFIG_ECG_SERIAL_DEBUG`, `ecg_serial_debug_task` se doc `raw_queue` de in UART thay vi chay full pipeline.

## Yeu cau phan cung

- AD8232 `OUTPUT` noi GPIO1.
- AD8232 `LO+` noi GPIO2.
- AD8232 `LO-` noi GPIO3.
- AD8232 `SDN` noi GPIO4.
- AD8232 dung nguon 3.3V va chung GND voi ESP32-S3.
