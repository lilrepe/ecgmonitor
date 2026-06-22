# ECG Monitor — ESP32-S3 + AD8232

Hệ thống đo ECG realtime: lọc nhiễu deep learning, hiển thị OLED, gửi lên web dashboard.

## Cấu trúc project

```
ecg_monitor/
├── CMakeLists.txt              # Root CMake
├── sdkconfig.defaults          # FreeRTOS tick 1000Hz, WebSocket, v.v.
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild       # menuconfig: WiFi SSID/Pass, debug options
│   └── main.c                  # app_main: tạo queue, spawn tasks
├── components/
│   ├── ecg_common/
│   │   └── ecg_common.h        # Shared types, queue handles, constants
│   ├── ecg_adc/
│   │   ├── ecg_adc.h
│   │   └── ecg_adc.c           # ADC Task @ 500Hz (migrate từ doluongecg.c)
│   ├── ecg_inference/
│   │   ├── ecg_inference.h
│   │   └── ecg_inference.c     # Inference Task (TFLite skeleton)
│   ├── ecg_hr_detect/
│   │   ├── ecg_hr_detect.h
│   │   └── ecg_hr_detect.c     # Pan-Tompkins HR detection
│   ├── ecg_display/
│   │   ├── ecg_display.h
│   │   └── ecg_display.c       # OLED SSD1306 @ 25fps
│   └── ecg_network/
│       ├── ecg_network.h       # (chứa 3 phương án thiết kế)
│       └── ecg_network.c       # WebSocket server skeleton
└── web/
    └── index.html              # Web dashboard (canvas + WebSocket)
```

## Luồng dữ liệu

```
AD8232 (GPIO1)
    │
    ▼ [500 Hz, esp_timer]
[ADC Task — Core 0, P5]
    │
    │ raw_queue (ecg_raw_sample_t)
    ▼
[Inference Task — Core 0, P4]  ← TFLite Micro (skeleton)
    │
    ├──► clean_queue ──────────► [Display Task — Core 1, P3] → OLED
    ├──► hr_input_queue ───────► [HR Detect — Core 0, P3] → g_current_hr
    └──► network_queue ────────► [Network Task — Core 1, P2] → WebSocket
```

## Wiring

| AD8232 Pin | ESP32-S3 Pin |
|-----------|-------------|
| OUTPUT    | GPIO1 (ADC1_CH0) |
| LO+       | GPIO2 |
| LO-       | GPIO3 |
| SDN       | GPIO4 (LOW = active) |
| 3.3V      | 3V3 |
| GND       | GND |

| SSD1306 Pin | ESP32-S3 Pin |
|------------|-------------|
| SDA        | GPIO8 |
| SCL        | GPIO9 |
| VCC        | 3V3 |
| GND        | GND |

## Build & Flash

```bash
# Cấu hình WiFi
idf.py menuconfig
# → ECG Monitor Configuration → WiFi SSID / Password

# Build
idf.py build

# Flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

## TODO — Các bước tiếp theo

### 1. Tích hợp TFLite Micro (ecg_inference.c)
- [ ] Train model denoiser (1D U-Net hoặc autoencoder) trên tập ECG có noise
- [ ] Quantize INT8: `tflite_convert` → `full_integer_quantization`
- [ ] Nhúng vào firmware: `target_add_binary_data` trong CMakeLists.txt
- [ ] Điền vào `TODO` blocks trong `ecg_inference.c`
- [ ] Đo latency inference (mục tiêu < 10ms/window @ 128 samples)

### 2. Fan-out clean window (main.c / ecg_inference.c)
- [ ] Refactor Inference Task để push copy tới cả 3 queue:
      `clean_queue`, `hr_input_queue`, `network_queue`
- [ ] Hoặc dùng callback pattern để tránh duplicate struct lớn

### 3. Web dashboard (web/index.html + ecg_network.c)
- [ ] Thay canvas thủ công bằng lightweight-charts
- [ ] Implement WebSocket broadcast với fd list (xem comment trong ecg_network.c)
- [ ] Nhúng index.html vào firmware (SPIFFS hoặc `target_add_binary_data`)
- [ ] Thêm binary protocol để giảm bandwidth

### 4. OLED driver (ecg_display.c)
- [ ] Migrate từ i2c legacy sang esp_lcd (IDF >= 5.1) để dùng DMA
- [ ] Hoặc dùng u8g2 nếu muốn font đẹp hơn

### 5. Production hardening
- [ ] WiFi Provisioning qua BLE (thay hardcode SSID/Pass)
- [ ] OTA update support
- [ ] Watchdog timer cho tất cả tasks
- [ ] NVS lưu cấu hình

## Phương án Web (3 options — xem ecg_network.h)

| | Option A: WS Server | Option B: MQTT | Option C: HTTP POST |
|-|---------------------|----------------|---------------------|
| Latency | ~10ms (LAN) | ~50ms | ~200ms+ |
| Infrastructure | Không cần | Cần broker | Cần backend |
| Multi-device | Khó | Dễ | Trung bình |
| Offline buffer | Manual | Built-in QoS | Manual |
| **Khuyến nghị** | **✓ Demo/prototype** | Nhiều thiết bị | Lưu database |
