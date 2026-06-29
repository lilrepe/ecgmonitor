/*
 * ecg_serial_debug.h — Serial Debug Task
 *
 * Task luôn được compile và link.
 * Chỉ được gọi từ main.c khi CONFIG_ECG_SERIAL_DEBUG=y.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FreeRTOS task: đọc raw_queue, in dữ liệu ECG ra UART.
 *
 * Stack:  4096 bytes
 * Core:   1 (tránh tranh Core 0 với ADC task)
 * Prio:   2 (thấp hơn ADC task)
 *
 * Output (tab-separated, dễ paste vào Excel/Python/MATLAB):
 *   t_ms  raw   mv    lo
 *   0     2048  1650  0
 *   2     2051  1652  0
 */
void ecg_serial_debug_task(void *pvParameters);

#ifdef __cplusplus
}
#endif
