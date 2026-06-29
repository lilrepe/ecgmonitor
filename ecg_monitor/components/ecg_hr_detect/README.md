# ecg_hr_detect

Module nay tinh BPM tu tin hieu ECG da loc.

## Luong logic

1. `ecg_hr_detect_init()` reset state detector.
2. `ecg_hr_detect_task()` doc `ecg_clean_window_t` tu `hr_input_queue`.
3. Bo qua window khong valid, vi co lead-off.
4. Moi sample di qua pipeline Pan-Tompkins don gian:
   - Derivative.
   - Squaring.
   - Moving window integration.
   - Adaptive threshold.
5. Khi phat hien R-peak hop le, module tinh RR interval va BPM trung binh.
6. BPM moi duoc ghi vao `g_current_hr` duoi `hr_mutex`.
7. BPM cung duoc day vao `hr_queue` bang `xQueueOverwrite()`.

## Tuong tac voi project

Module nay chi nhan du lieu da loc tu `hr_input_queue`. Display va network doc BPM moi nhat tu `g_current_hr`, nen khong can tranh nhau doc cung mot queue ECG.

## Yeu cau code

- Chi xu ly window valid.
- Can giu refractory period de tranh dem mot QRS nhieu lan.
- Neu thay doi sample rate trong `ecg_common.h`, can kiem tra lai cac hang so RR va refractory.
