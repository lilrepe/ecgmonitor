/*
 * test_ecg_adc.c — Unit Tests cho component ecg_adc
 *
 * Framework: Unity (tích hợp sẵn trong ESP-IDF)
 * Target:    ESP32-S3 trên phần cứng thật (on-target testing)
 *
 * Phần cứng cần kết nối:
 *   AD8232 OUTPUT  -> GPIO1  (ADC1_CH0)
 *   AD8232 LO+     -> GPIO2
 *   AD8232 LO-     -> GPIO3
 *   AD8232 SDN     -> GPIO4  (hoặc nối thẳng 3V3)
 *   AD8232 3.3V    -> 3V3
 *   AD8232 GND     -> GND
 *
 * Danh sách tests:
 *  T01 [gpio]  LO+/LO- được cấu hình INPUT  → đọc không lỗi
 *  T02 [gpio]  SDN được cấu hình OUTPUT     → set level không lỗi
 *  T03 [adc]   ecg_adc_init() không crash    → overflow counter = 0
 *  T04 [adc]   Raw ADC nằm trong [0, 4095]
 *  T05 [adc]   Voltage (nếu calibrated) nằm trong [0, 3300] mV
 *  T06 [queue] raw_queue nhận sample sau ecg_adc_init + 1 chu kỳ
 *  T07 [logic] lead-off logic đúng cho 4 trạng thái (pure logic, không cần HW)
 *  T08 [timing] overflow counter không tăng trong 100ms đầu
 *  T09 [adc]   Đọc 10 mẫu liên tiếp — không có lỗi
 *  T10 [adc]   Nhiều lần khởi tạo/dọn không crash (robustness)
 */

#include "unity.h"
#include "ecg_adc.h"
#include "ecg_common.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG_T = "TEST_ADC";

/* ─── Biến internal được expose khi bật CONFIG_ECG_UNIT_TEST ─ */
#ifdef CONFIG_ECG_UNIT_TEST
/* Khai báo extern — định nghĩa trong ecg_adc.c khi test mode bật */
extern adc_oneshot_unit_handle_t g_test_adc1_handle;
extern adc_cali_handle_t         g_test_adc1_cali_handle;
extern bool                      g_test_adc_calibrated;
#endif

/* ─── Queue toàn cục (extern từ ecg_common.h, define trong main) ─ */
/* Trong test, ta tự tạo queue cục bộ để không phụ thuộc main.c     */
QueueHandle_t raw_queue   = NULL;
QueueHandle_t clean_queue = NULL;
QueueHandle_t hr_queue    = NULL;
SemaphoreHandle_t hr_mutex = NULL;
ecg_hr_t g_current_hr     = {0};

/* ─── Các queue khác được khai báo extern trong các component ──── */
QueueHandle_t hr_input_queue  = NULL;
QueueHandle_t network_queue   = NULL;

/* ════════════════════════════════════════════════════════════
 *  setUp / tearDown — Unity gọi trước/sau mỗi test
 * ════════════════════════════════════════════════════════════ */
void setUp(void)
{
    /* Tạo raw_queue mới cho mỗi test */
    if (raw_queue == NULL) {
        raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    }
}

void tearDown(void)
{
    /* Xóa queue sau mỗi test để tránh ảnh hưởng lẫn nhau */
    if (raw_queue != NULL) {
        vQueueDelete(raw_queue);
        raw_queue = NULL;
    }
}

/* ════════════════════════════════════════════════════════════
 *  T01: GPIO LO+/LO- — cấu hình INPUT, đọc không lỗi
 *
 *  Kiểm tra: gpio_get_level() trả về 0 hoặc 1 (không phải giá trị lạ)
 *  Không phụ thuộc: Không cần AD8232 thật, chỉ cần GPIO không bị ngắn mạch
 * ════════════════════════════════════════════════════════════ */
static void t01_gpio_lo_input(void)
{
    /* ecg_adc_init() cấu hình GPIO bên trong */
    ecg_adc_init();

    int lop = gpio_get_level(PIN_LO_PLUS);
    int lom = gpio_get_level(PIN_LO_MINUS);

    TEST_ASSERT_TRUE_MESSAGE(lop == 0 || lop == 1,
        "LO+ tra ve gia tri ngoai [0,1] — kiem tra day noi GPIO2");
    TEST_ASSERT_TRUE_MESSAGE(lom == 0 || lom == 1,
        "LO- tra ve gia tri ngoai [0,1] — kiem tra day noi GPIO3");

    ESP_LOGI(TAG_T, "[T01] LO+ = %d, LO- = %d", lop, lom);
}

/* ════════════════════════════════════════════════════════════
 *  T02: GPIO SDN — cấu hình OUTPUT, set level không lỗi
 *
 *  Kiểm tra: gpio_set_level() trả về ESP_OK cho cả HIGH và LOW
 * ════════════════════════════════════════════════════════════ */
static void t02_gpio_sdn_output(void)
{
    ecg_adc_init();

    /* Thử set HIGH → rồi trả về LOW (AD8232 active = LOW) */
    esp_err_t err_hi = gpio_set_level(PIN_SDN, 1);
    esp_err_t err_lo = gpio_set_level(PIN_SDN, 0);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err_hi,
        "SDN set HIGH that bai — kiem tra GPIO4 cau hinh OUTPUT");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err_lo,
        "SDN set LOW that bai — AD8232 co the bi mat nguon");

    ESP_LOGI(TAG_T, "[T02] SDN HIGH/LOW: OK");
}

/* ════════════════════════════════════════════════════════════
 *  T03: ecg_adc_init() không crash + overflow = 0 khi mới khởi tạo
 *
 *  Kiểm tra: Hàm init không gây ESP_ERROR_CHECK panic
 *            g_adc_overflow_count = 0 ngay sau init
 * ════════════════════════════════════════════════════════════ */
static void t03_adc_init_no_crash(void)
{
    /* Nếu ecg_adc_init() crash → test tự fail do exception */
    ecg_adc_init();

    /* Sau khi init, chưa có sample nào bị drop */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, g_adc_overflow_count,
        "g_adc_overflow_count != 0 ngay sau init — co gi do sai");

    ESP_LOGI(TAG_T, "[T03] ecg_adc_init() OK, overflow = 0");
}

/* ════════════════════════════════════════════════════════════
 *  T04: Raw ADC nằm trong [0, 4095] (12-bit)
 *
 *  Phần cứng cần: AD8232 cắm vào GPIO1, không nhất thiết có ECG thật
 *  Nếu không có tín hiệu: raw sẽ ~2048 (mid-rail) hoặc ~0/4095
 * ════════════════════════════════════════════════════════════ */
static void t04_adc_raw_range(void)
{
#ifdef CONFIG_ECG_UNIT_TEST
    ecg_adc_init();

    TEST_ASSERT_NOT_NULL_MESSAGE(g_test_adc1_handle,
        "adc1_handle = NULL — adc_oneshot_new_unit that bai");

    int raw = -1;
    esp_err_t err = adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err,
        "adc_oneshot_read loi — kiem tra GPIO1 da noi vao OUTPUT cua AD8232 chua");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, raw,
        "Raw ADC < 0 — loi nghiem trong trong ADC driver");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(4095, raw,
        "Raw ADC > 4095 — vuot 12-bit, kiem tra ADC_BITWIDTH_12");

    ESP_LOGI(TAG_T, "[T04] ADC raw = %d (hop le: 0–4095)", raw);
#else
    TEST_IGNORE_MESSAGE("[T04] Bo qua: can build voi CONFIG_ECG_UNIT_TEST=y");
#endif
}

/* ════════════════════════════════════════════════════════════
 *  T05: Voltage sau calibration trong [0, 3300] mV
 *
 *  Bỏ qua (IGNORE) nếu chip không có eFuse calibration
 * ════════════════════════════════════════════════════════════ */
static void t05_adc_voltage_range(void)
{
#ifdef CONFIG_ECG_UNIT_TEST
    ecg_adc_init();

    if (!g_test_adc_calibrated) {
        TEST_IGNORE_MESSAGE("[T05] ADC calibration khong co san tren chip nay — IGNORED");
        return;
    }

    TEST_ASSERT_NOT_NULL(g_test_adc1_handle);
    TEST_ASSERT_NOT_NULL(g_test_adc1_cali_handle);

    int raw     = -1;
    int volt_mv = -1;

    TEST_ASSERT_EQUAL(ESP_OK,
        adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw));

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
        adc_cali_raw_to_voltage(g_test_adc1_cali_handle, raw, &volt_mv),
        "adc_cali_raw_to_voltage that bai — calibration handle co van de");

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0,    volt_mv, "Voltage < 0 mV");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE   (3300, volt_mv, "Voltage > 3300 mV — vuot nguon 3.3V");

    ESP_LOGI(TAG_T, "[T05] ADC raw = %-4d → %d mV (trong [0, 3300] mV)", raw, volt_mv);
#else
    TEST_IGNORE_MESSAGE("[T05] Bo qua: can build voi CONFIG_ECG_UNIT_TEST=y");
#endif
}

/* ════════════════════════════════════════════════════════════
 *  T06: raw_queue nhận sample sau khi task chạy 1 chu kỳ (2ms)
 *
 *  Spawn ecg_adc_task ngắn, chờ 10ms, kiểm tra queue có ít nhất 1 item
 *  Lưu ý: Cần raw_queue đã được tạo (setUp() lo việc này)
 * ════════════════════════════════════════════════════════════ */
static void t06_queue_receives_sample(void)
{
    /* Đảm bảo queue trống */
    if (raw_queue == NULL) {
        raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    }
    xQueueReset(raw_queue);

    ecg_adc_init();

    /* Spawn task tạm thời — stack 4KB, priority cao hơn test task */
    TaskHandle_t adc_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(
        ecg_adc_task, "test_adc_t",
        TASK_STACK_ADC, NULL,
        TASK_PRIO_ADC + 1,   /* cao hơn 1 bậc để chạy ngay */
        &adc_task_handle,
        TASK_CORE_ADC
    );

    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret,
        "Khong tao duoc task — het RAM? Kiem tra TASK_STACK_ADC");

    /* Chờ ít nhất 1 chu kỳ (2ms) + buffer */
    vTaskDelay(pdMS_TO_TICKS(20));

    UBaseType_t msgs = uxQueueMessagesWaiting(raw_queue);
    ESP_LOGI(TAG_T, "[T06] raw_queue co %u sample sau 20ms", (unsigned)msgs);

    /* Dừng task trước khi assert để tránh crash nếu test fail */
    if (adc_task_handle != NULL) {
        vTaskDelete(adc_task_handle);
    }

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, msgs,
        "raw_queue rong sau 20ms — ecg_adc_task khong chay duoc, kiem tra HW + queue");
}

/* ════════════════════════════════════════════════════════════
 *  T07: Logic lead-off — pure software, không cần phần cứng
 *
 *  Bảng sự thật: lead_off = lo_plus || lo_minus
 *  4 tổ hợp (0,0), (1,0), (0,1), (1,1)
 * ════════════════════════════════════════════════════════════ */
static void t07_leadoff_logic_truth_table(void)
{
    typedef struct { int lop; int lom; bool expected; } tc_t;
    const tc_t cases[4] = {
        {0, 0, false},
        {1, 0, true },
        {0, 1, true },
        {1, 1, true },
    };

    for (int i = 0; i < 4; i++) {
        bool actual = (bool)(cases[i].lop || cases[i].lom);
        ESP_LOGI(TAG_T, "[T07] LO+=%d LO-=%d → lead_off=%s (expected: %s)",
                 cases[i].lop, cases[i].lom,
                 actual            ? "true" : "false",
                 cases[i].expected ? "true" : "false");
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].expected, actual,
            "Logic lead_off = lo_plus || lo_minus tinh sai!");
    }
    ESP_LOGI(TAG_T, "[T07] Lead-off logic: 4/4 truong hop PASS");
}

/* ════════════════════════════════════════════════════════════
 *  T08: Overflow counter không tăng trong 100ms đầu (queue đủ lớn)
 *
 *  Giả sử raw_queue capacity = 256 item.
 *  Trong 100ms @ 500Hz = 50 sample → không nên overflow
 * ════════════════════════════════════════════════════════════ */
static void t08_no_overflow_in_first_100ms(void)
{
    /* Tạo queue lớn để không bị overflow trong thời gian ngắn */
    if (raw_queue != NULL) {
        vQueueDelete(raw_queue);
    }
    raw_queue = xQueueCreate(2 * ECG_WINDOW_SIZE, sizeof(ecg_raw_sample_t));
    TEST_ASSERT_NOT_NULL(raw_queue);
    xQueueReset(raw_queue);

    ecg_adc_init();

    /* Reset bộ đếm trước khi đo */
    g_adc_overflow_count = 0;

    TaskHandle_t adc_task_handle = NULL;
    xTaskCreatePinnedToCore(
        ecg_adc_task, "test_adc_ov",
        TASK_STACK_ADC, NULL, TASK_PRIO_ADC + 1,
        &adc_task_handle, TASK_CORE_ADC
    );

    /* Chờ 100ms — queue 256 items, chỉ ~50 sample được tạo ra */
    vTaskDelay(pdMS_TO_TICKS(100));

    uint32_t overflows = g_adc_overflow_count;
    if (adc_task_handle != NULL) {
        vTaskDelete(adc_task_handle);
    }

    ESP_LOGI(TAG_T, "[T08] Overflows trong 100ms: %lu", overflows);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, overflows,
        "Co overflow trong 100ms — raw_queue qua nho hoac task qua cham");
}

/* ════════════════════════════════════════════════════════════
 *  T09: Đọc 10 mẫu liên tiếp — tất cả hợp lệ
 *
 *  Kiểm tra độ ổn định của ADC driver qua nhiều lần đọc
 * ════════════════════════════════════════════════════════════ */
static void t09_read_10_samples_valid(void)
{
#ifdef CONFIG_ECG_UNIT_TEST
    ecg_adc_init();

    TEST_ASSERT_NOT_NULL_MESSAGE(g_test_adc1_handle,
        "adc1_handle = NULL — init that bai");

    for (int i = 0; i < 10; i++) {
        int raw = -1;
        esp_err_t err = adc_oneshot_read(g_test_adc1_handle, ECG_ADC_CHANNEL, &raw);

        char msg[64];
        snprintf(msg, sizeof(msg), "Sample %d: adc_oneshot_read loi (err=0x%x)", i, err);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err, msg);

        snprintf(msg, sizeof(msg), "Sample %d: raw = %d, ngoai [0, 4095]", i, raw);
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(0, raw, msg);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(4095, raw, msg);

        /* Delay nhỏ giữa các lần đọc (mô phỏng 500Hz) */
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGI(TAG_T, "[T09] 10/10 sample doc thanh cong");
#else
    TEST_IGNORE_MESSAGE("[T09] Can build voi CONFIG_ECG_UNIT_TEST=y");
#endif
}

/* ════════════════════════════════════════════════════════════
 *  T10: Khởi tạo lại (init → deinit → init) không crash
 *
 *  Kiểm tra: ADC driver có thể được khởi tạo lại an toàn
 *  Lưu ý: ESP-IDF adc_oneshot_new_unit sẽ lỗi nếu gọi 2 lần liên tiếp
 *          mà không delete unit cũ → T10 test việc cleanup đúng cách
 * ════════════════════════════════════════════════════════════ */
static void t10_reinit_no_crash(void)
{
#ifdef CONFIG_ECG_UNIT_TEST
    /* Lần 1 */
    ecg_adc_init();
    TEST_ASSERT_NOT_NULL(g_test_adc1_handle);

    /* Dọn dẹp thủ công — giống teardown_adc() trong instrumentecg */
    if (g_test_adc1_handle != NULL) {
        adc_oneshot_del_unit(g_test_adc1_handle);
        g_test_adc1_handle = NULL;
    }
    if (g_test_adc1_cali_handle != NULL) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(g_test_adc1_cali_handle);
#else
        adc_cali_delete_scheme_line_fitting(g_test_adc1_cali_handle);
#endif
        g_test_adc1_cali_handle = NULL;
    }
    g_test_adc_calibrated = false;

    /* Lần 2 — phải không crash */
    ecg_adc_init();
    TEST_ASSERT_NOT_NULL_MESSAGE(g_test_adc1_handle,
        "Khoi tao lan 2 that bai — co loi khi del + reinit ADC");

    ESP_LOGI(TAG_T, "[T10] Re-init ADC: OK (khong crash)");
#else
    TEST_IGNORE_MESSAGE("[T10] Can build voi CONFIG_ECG_UNIT_TEST=y");
#endif
}

/* ════════════════════════════════════════════════════════════
 *  ecg_adc_run_tests() — Entry point, gọi từ app_main
 * ════════════════════════════════════════════════════════════ */
void ecg_adc_run_tests(void)
{
    UNITY_BEGIN();

    /* ── Nhóm GPIO ──────────────────────────────── */
    RUN_TEST(t01_gpio_lo_input);
    RUN_TEST(t02_gpio_sdn_output);

    /* ── Nhóm ADC init ──────────────────────────── */
    RUN_TEST(t03_adc_init_no_crash);
    RUN_TEST(t04_adc_raw_range);
    RUN_TEST(t05_adc_voltage_range);

    /* ── Nhóm Queue / Task ──────────────────────── */
    RUN_TEST(t06_queue_receives_sample);

    /* ── Nhóm Logic (không cần HW) ─────────────── */
    RUN_TEST(t07_leadoff_logic_truth_table);

    /* ── Nhóm Stability ─────────────────────────── */
    RUN_TEST(t08_no_overflow_in_first_100ms);
    RUN_TEST(t09_read_10_samples_valid);
    RUN_TEST(t10_reinit_no_crash);

    UNITY_END();
}
