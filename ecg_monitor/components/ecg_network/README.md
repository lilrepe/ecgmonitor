# ecg_network

Module nay ket noi WiFi va phat ECG realtime qua WebSocket server tren chinh ESP32-S3.

## Luong logic

1. `ecg_network_init()` khoi tao `esp_netif`, event loop va WiFi station.
2. Module doc SSID/password tu `menuconfig`.
3. Khi co IP, module start HTTP server port `ECG_WS_PORT`.
4. HTTP `/` tra ve dashboard HTML toi thieu.
5. WebSocket `/ecg` nhan client browser.
6. `ecg_network_task()` doc `ecg_clean_window_t` tu `network_queue`.
7. Task gom sample thanh batch va broadcast JSON cho tat ca WebSocket client.

## Data duoc ban di dau?

ESP32-S3 la WebSocket server. May tinh/dien thoai cung mang WiFi mo browser vao:

```text
http://<IP-cua-ESP32>/
```

Dashboard tren trang nay tu ket noi toi:

```text
ws://<IP-cua-ESP32>/ecg
```

Moi goi JSON co dang:

```json
{
  "t": 123456,
  "fs": 500,
  "bpm": 72,
  "lo": false,
  "ecg": [0.48, 0.51, 0.55]
}
```

## Cach nhan du lieu tu app rieng

Neu viet web/mobile app rieng, chi can tao WebSocket toi `ws://<IP>/ecg` va parse JSON trong event `onmessage`.

## Yeu cau cau hinh

Dat WiFi trong:

```bash
idf.py menuconfig
```

Vao `ECG Monitor Configuration -> WiFi SSID / WiFi Password`.
