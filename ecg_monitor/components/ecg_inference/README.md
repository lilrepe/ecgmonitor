# ecg_inference

Module nay la tang loc nhieu ECG. Ten "inference" duoc giu lai vi task graph cu da noi raw data qua day, nhung hien tai module co 2 backend.

## Hai lua chon loc nhieu

- `Classic DSP filters`: dang dung mac dinh va chay that. Pipeline la high-pass 0.5 Hz, notch 50 Hz, low-pass 40 Hz.
- `AI denoise placeholder`: khung de sau nay gan model. Hien tai chi pass-through, co log canh bao, va compile duoc.

Chon backend bang:

```bash
idf.py menuconfig
```

Sau do vao `ECG Monitor Configuration -> ECG denoise backend`.

## Luong logic

1. `ecg_inference_task()` nhan tung `ecg_raw_sample_t` tu `raw_queue`.
2. Gom du `ECG_WINDOW_SIZE` sample thanh sliding window.
3. Neu classic backend:
   - Normalize raw ADC ve `0.0..1.0`.
   - Tru baseline 0.5.
   - Chay high-pass, notch, low-pass bang biquad IIR.
   - Normalize output window ve `0.0..1.0` cho OLED/web.
4. Neu AI backend:
   - Goi hook placeholder.
   - Sau nay thay bang model inference that.
5. Dong goi `ecg_clean_window_t`.
6. Fan-out cung mot window sang:
   - `clean_queue` cho OLED.
   - `hr_input_queue` cho HR detection.
   - `network_queue` cho WebSocket.
7. Slide window theo `ECG_WINDOW_STEP`, giu lai overlap.

## Khung AI sau nay

Khi co model, can lam trong `run_ai_placeholder()`:

1. Them dependency TFLite Micro hoac ESP-DL.
2. Embed model vao firmware bang `target_add_binary_data()`.
3. Cap phat arena/tensor buffer, uu tien PSRAM.
4. Copy `input[ECG_WINDOW_SIZE]` vao model input.
5. Invoke model.
6. Copy model output vao `output[ECG_WINDOW_SIZE]`.

## Yeu cau code

- Output phai giu cung shape `ECG_WINDOW_SIZE`.
- Output nen nam trong khoang `0.0..1.0` de display va dashboard ve dung.
- Khong block lau trong task nay vi ADC dang day sample 500 Hz.
