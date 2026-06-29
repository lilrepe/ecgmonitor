# ECG Monitor - ESP32-S3 + AD8232

Project ESP-IDF do ECG realtime. Mac dinh project chay full pipeline:

```text
AD8232 -> ADC 500 Hz -> denoise -> HR detect -> OLED + WebSocket
```

Tang loc nhieu co 2 lua chon:

- `Classic DSP filters`: dang bat mac dinh, chay that voi high-pass, notch, low-pass.
- `AI denoise placeholder`: chi la khung de sau nay gan model AI.

## Noi chan phan cung

AD8232:

| AD8232 | ESP32-S3 |
| --- | --- |
| OUTPUT | GPIO1 / ADC1_CH0 |
| LO+ | GPIO2 |
| LO- | GPIO3 |
| SDN | GPIO4 |
| 3.3V | 3V3 |
| GND | GND |

OLED SSD1306 I2C 128x64:

| OLED | ESP32-S3 |
| --- | --- |
| SDA | GPIO8 |
| SCL | GPIO9 |
| VCC | 3V3 |
| GND | GND |

## Chay project bang ESP-IDF

Mo ESP-IDF terminal, vao thu muc project:

```bash
cd D:/esp/code/ecg_monitor/ecg_monitor
```

Chon chip:

```bash
idf.py set-target esp32s3
```

Cau hinh WiFi va che do loc nhieu:

```bash
idf.py menuconfig
```

Trong menu:

- `ECG Monitor Configuration -> WiFi SSID`
- `ECG Monitor Configuration -> WiFi Password`
- `ECG Monitor Configuration -> ECG denoise backend`

Build:

```bash
idf.py build
```

Flash va monitor, thay `COMx` bang cong serial cua board:

```bash
idf.py -p COMx flash monitor
```

Thoat monitor bang `Ctrl+]`.

## Xem dashboard web

Sau khi flash, monitor se in IP cua ESP32-S3, vi du:

```text
Got IP: 192.168.1.50
```

Mo browser cung mang WiFi:

```text
http://192.168.1.50/
```

Dashboard se tu ket noi WebSocket toi `/ecg` de nhan ECG JSON realtime.

## Cau truc module

Moi module co file `README.md` rieng:

- `main/README.md`: app_main, queue, task graph.
- `components/ecg_common/README.md`: shared types/config.
- `components/ecg_adc/README.md`: doc ADC tu AD8232.
- `components/ecg_inference/README.md`: classic filter va AI placeholder.
- `components/ecg_hr_detect/README.md`: tinh BPM.
- `components/ecg_display/README.md`: OLED SSD1306.
- `components/ecg_network/README.md`: WiFi/WebSocket va format data.

## Ghi chu nhanh

Neu chi muon test ADC qua UART, bat:

```text
CONFIG_ECG_SERIAL_DEBUG=y
```

trong `menuconfig`. Khi bat che do nay, display/network/denoise se khong chay.
