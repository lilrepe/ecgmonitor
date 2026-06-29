# ecg_display

Module nay hien thi ECG va BPM len OLED SSD1306 128x64 qua I2C.

## Luong logic

1. `ecg_display_init()` cau hinh I2C va gui chuoi lenh init SSD1306.
2. `ecg_display_task()` chay khoang 25 FPS.
3. Task drain `clean_queue` de lay cac window ECG moi.
4. Sample `cleaned[]` duoc map tu `0.0..1.0` sang toa do Y tren man hinh.
5. Task ve waveform rolling vao framebuffer RAM.
6. Task doc `g_current_hr` duoi `hr_mutex` va ve BPM neu valid.
7. Framebuffer 1024 byte duoc flush qua I2C.

## Tuong tac voi project

Input cua module la `clean_queue` va `g_current_hr`. Module khong can biet ADC hay network dang hoat dong ra sao.

## Yeu cau phan cung

- OLED SDA noi GPIO8.
- OLED SCL noi GPIO9.
- OLED VCC noi 3V3.
- OLED GND noi GND.
- Dia chi I2C mac dinh la `0x3C`.
