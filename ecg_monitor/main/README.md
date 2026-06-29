# Main module

`main.c` la diem dieu phoi cua project. File nay tao queue/mutex dung chung, khoi tao cac component, sau do spawn cac FreeRTOS task.

## Luong logic

1. `app_main()` in thong tin firmware va doc cau hinh build.
2. Neu `CONFIG_ECG_SERIAL_DEBUG=y`, project chi tao `raw_queue`, khoi tao ADC va chay task debug UART.
3. Neu chay full mode, project tao:
   - `raw_queue`: ADC day sample tho vao.
   - `clean_queue`: denoise task day window da loc cho OLED.
   - `hr_input_queue`: ban sao window da loc cho module tinh nhip tim.
   - `network_queue`: ban sao window da loc cho module WebSocket.
   - `hr_queue` va `hr_mutex`: chia se BPM moi nhat.
4. Khoi tao hardware/module theo thu tu: ADC, denoise, HR, display, network.
5. Spawn task theo priority/core da khai bao trong `ecg_common.h`.

## Tuong tac voi project

`main.c` khong xu ly ECG truc tiep. No chi giu vai tro wiring: noi cac queue giua ADC, denoise, HR, display va network. Khi them module moi, nen tao queue/callback o day thay vi de cac component include lan nhau.

## Yeu cau cau hinh

- Chon che do loc nhieu trong `idf.py menuconfig -> ECG Monitor Configuration -> ECG denoise backend`.
- Mac dinh la `Classic DSP filters`.
- `AI denoise placeholder` chi la khung de gan model sau nay.
