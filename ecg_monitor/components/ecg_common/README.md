# ecg_common

Module nay chua cac kieu du lieu va hang so dung chung cho toan bo project.

## Luong logic

`ecg_common.h` khong co task rieng. Cac component include file nay de dung chung:

- Tan so lay mau `ECG_SAMPLE_RATE_HZ`.
- Kich thuoc window `ECG_WINDOW_SIZE`, overlap va step.
- GPIO cho AD8232 va OLED.
- Struct `ecg_raw_sample_t`, `ecg_clean_window_t`, `ecg_hr_t`.
- Queue handle global: `raw_queue`, `clean_queue`, `hr_input_queue`, `network_queue`, `hr_queue`.
- Priority, stack size va core assignment cho cac task.

## Tuong tac voi project

Day la hop dong du lieu giua cac module. Neu thay doi kich thuoc window, sample rate, struct data hoac queue, can kiem tra lai ADC, denoise, HR, display va network.

## Yeu cau code

- Khong include header cua component khac trong `ecg_common.h` de tranh circular dependency.
- Cac global queue chi khai bao `extern` o day, con tao that trong `main.c`.
