# Phân Tích Cấu Trúc Project & Đánh Giá Chi Tiết (Critique)

Tài liệu này phân tích cấu trúc thư mục của dự án **ECG Monitor (ESP32-S3 + AD8232)** và chỉ ra các điểm lỗi logic cực kỳ nghiêm trọng, các lỗi thiết kế hệ thống ("điểm bất ổn") hiện có trong mã nguồn.

---

## 1. Giải Thích Cấu Trúc Thư Mục (Folder Structure)

Dự án được tổ chức theo chuẩn **ESP-IDF** (Espressif IoT Development Framework) sử dụng hệ thống build **CMake**:

*   **`.vscode/`**: Chứa các cấu hình riêng cho VS Code (phím tắt, đường dẫn compiler, cấu hình debug).
*   **`ecg_monitor/`** (Root directory chứa code):
    *   **`CMakeLists.txt`**: File cấu hình CMake gốc của dự án, khai báo phiên bản CMake tối thiểu và load các công cụ của ESP-IDF.
    *   **`sdkconfig.defaults`**: Chứa các thiết lập mặc định cấu hình sẵn cho SDK (FreeRTOS Hz, WiFi, HTTP Server, Stack size, v.v.). Khi build, ESP-IDF sẽ đọc file này để tạo ra file cấu hình `sdkconfig`.
    *   **`main/`**: Thư mục chính chứa chương trình khởi chạy.
        *   `CMakeLists.txt`: Cấu hình build cho thư mục `main`.
        *   `Kconfig.projbuild`: Định nghĩa menu cấu hình trực quan (khi gõ `idf.py menuconfig`) giúp người dùng nhập WiFi SSID, mật khẩu và bật/tắt chế độ monitor mà không cần sửa code.
        *   `main.c`: Điểm vào chính của chương trình (`app_main`). Nó chịu trách nhiệm khởi tạo các tài nguyên dùng chung (FreeRTOS Queue, Mutex) và spawn (tạo) các FreeRTOS Tasks để chạy song song.
    *   **`components/`**: Chứa các module (thư viện con) được module hóa độc lập:
        *   `ecg_common/`: Chứa header chung `ecg_common.h` định nghĩa các kiểu dữ liệu, hằng số cấu hình hệ thống (Sample rate, GPIOs, Stack sizes) và các extern variable (Queue handles).
        *   `ecg_adc/`: Xử lý cấu hình GPIO, bộ chuyển đổi ADC oneshot để lấy mẫu từ cảm biến AD8232.
        *   `ecg_inference/`: Nơi chạy mô hình AI lọc nhiễu (dùng TensorFlow Lite Micro). Hiện tại là khung xương (skeleton).
        *   `ecg_hr_detect/`: Thực hiện thuật toán phát hiện đỉnh R (Pan-Tompkins) để tính nhịp tim (BPM).
        *   `ecg_display/`: Điều khiển màn hình hiển thị OLED SSD1306 qua I2C để vẽ sóng điện tim thời gian thực và thông số BPM.
        *   `ecg_network/`: Thiết lập kết nối WiFi và WebSocket Server để truyền dữ liệu lên Web Dashboard.
    *   **`web/`**:
        *   `index.html`: Web Dashboard phía Client nhận dữ liệu từ ESP32 qua WebSocket và vẽ đồ thị ECG thời gian thực bằng HTML5 Canvas.

---

## 2. Chỉ Ra Những Điểm "Bất Ổn" (Bugs & Thiết Kế Lỗi)

Qua việc review chi tiết mã nguồn hiện tại, hệ thống này **không thể chạy đúng** do chứa các bug logic nghiêm trọng ở hầu hết các component. Dưới đây là danh sách chi tiết các điểm bất ổn từ hệ thống đến chi tiết từng dòng code:

### 2.1. Lạm dụng cấu hình FreeRTOS & lỗi Timing lấy mẫu (`sdkconfig.defaults` & `ecg_adc.c`)
*   **Vấn đề**: Để đạt tần số lấy mẫu $500\text{ Hz}$ (chu kỳ $2\text{ ms}$), tác giả đã cấu hình `CONFIG_FREERTOS_HZ=1000` trong `sdkconfig.defaults` để đổi tick-rate của hệ điều hành từ $100\text{ Hz}$ lên $1000\text{ Hz}$. Việc này nhằm ép `vTaskDelay(1)` thành $1\text{ ms}$ để lập trình vòng lặp lấy mẫu thủ công bằng cách delay từng tick:
    ```c
    if (now < next_sample) {
        vTaskDelay(1);
        continue;
    }
    ```
*   **Cái ngu/Bất ổn**: 
    1. **Tốn tài nguyên hệ thống**: Tăng Tick Rate lên $1000\text{ Hz}$ làm CPU của ESP32 liên tục phải xử lý ngắt scheduler (1000 lần/giây), tăng overhead context-switch vô ích, làm thiết bị tốn pin hơn và giảm hiệu năng tổng thể.
    2. **Jitter & Sai lệch tần số lấy mẫu**: `vTaskDelay(1)` block task ít nhất $1\text{ ms}$, nhưng tùy thuộc vào thời điểm gọi và độ ưu tiên của các task khác, nó có thể kéo dài hơn $2\text{ ms}$.
    3. **Lỗi "Catch-up" dồn dập**: Dòng code `next_sample += ECG_PERIOD_US;` sẽ cộng dồn mốc thời gian mục tiêu. Nếu vì lý do nào đó (WiFi hoạt động, chạy inference nặng) mà task ADC bị trễ quá $2\text{ ms}$, lúc này `now` sẽ lớn hơn `next_sample` liên tục trong vài chu kỳ. Task sẽ chạy vòng lặp đọc ADC liên tục mà không hề delay (`now < next_sample` trả về `false`). Kết quả là ADC bị đọc dồn dập (back-to-back) trong vài micro-giây để bù thời gian, tạo ra dữ liệu ECG bị méo mó nghiêm trọng nhưng lại mang nhãn timestamp tuyến tính giả tạo!
*   **Giải pháp đúng**: Sử dụng driver **ADC Continuous mode với DMA** (ESP32-S3 hỗ trợ phần cứng tự động lấy mẫu và đẩy qua DMA chuyển vào RAM) hoặc sử dụng **Hardware Timer ISR** (hoặc `esp_timer` callback chuyên dụng) để trigger lấy mẫu ở mức Driver phần cứng, đảm bảo độ chính xác cực cao ($0\%$ jitter) mà không cần can thiệp đổi Tick Rate của OS.

### 2.2. Lỗi rò rỉ bộ nhớ (Memory Leak) do tạo Queue trùng lặp
*   **Vấn đề**: 
    *   Trong [main.c](file:///d:/D_main_folder/ecg_monitor/ecg_monitor/main/main.c#L158), hàng đợi mạng được tạo ra:
        `network_queue = xQueueCreate(8, sizeof(ecg_clean_window_t));`
    *   Nhưng trong [ecg_network.c](file:///d:/D_main_folder/ecg_monitor/ecg_monitor/components/ecg_network/ecg_network.c#L234), bên trong hàm `ecg_network_init()`, biến này lại tiếp tục được khởi tạo lại:
        `network_queue = xQueueCreate(8, sizeof(ecg_clean_window_t));`
*   **Cái ngu**: Việc này ghi đè con trỏ `network_queue` cũ và gây rò rỉ vùng nhớ heap đã cấp phát cho Queue đầu tiên trong `main.c`.

### 2.3. Lỗi logic Reset Window gây tê liệt hệ thống vĩnh viễn (`ecg_inference.c`)
*   **Vấn đề**: Trong task xử lý inference:
    ```c
    /* Lưu timestamp đầu tiên */
    if (s_window_fill == 0) {
        window_start_ts      = raw.timestamp_us;
        window_has_lead_off  = false;
    }
    ...
    /* Dịch chuyển cửa sổ overlap 50% */
    memmove(s_window_buf, s_window_buf + ECG_WINDOW_STEP, ECG_WINDOW_OVERLAP * sizeof(float));
    s_window_fill = ECG_WINDOW_OVERLAP; // = 64
    ```
*   **Cái ngu/Bất ổn**:
    1. Sau cửa sổ đầu tiên, `s_window_fill` được gán lại bằng `ECG_WINDOW_OVERLAP` (tức là 64). Kể từ giây phút đó trở đi, `s_window_fill` chỉ dao động từ 64 đến 128 và **không bao giờ bằng 0 nữa**!
    2. Do đó, điều kiện `if (s_window_fill == 0)` **không bao giờ thỏa mãn lần thứ hai**.
    3. Hệ quả là `window_start_ts` không bao giờ được cập nhật lại (bị kẹt ở timestamp của giây đầu tiên mãi mãi).
    4. Tệ hơn nữa, nếu có bất kỳ một sự kiện bong điện cực nào xảy ra làm `raw.lead_off = true`, cờ `window_has_lead_off` sẽ chuyển sang `true` và **không bao giờ được reset về `false` nữa**. Toàn bộ các window phía sau sẽ bị gán nhãn `valid = false`, dẫn đến việc thuật toán đo nhịp tim (HR Detect) và truyền thông mạng (Network) bỏ qua hoàn toàn các gói dữ liệu này (hệ thống ngừng hoạt động hoàn toàn sau một lần lỏng dây đo!).
*   **Giải pháp đúng**: Phải dịch chuyển cả timestamp lưu trữ và trạng thái lead-off tương ứng của các sample cũ khi trượt cửa sổ, hoặc thiết lập lại logic gán `window_start_ts` và trạng thái lead-off dựa trên các mẫu thực sự nằm trong phạm vi cửa sổ trượt mới.

### 2.4. Lỗi lặp mẫu phá hỏng thuật toán Pan-Tompkins (`ecg_hr_detect.c`)
*   **Vấn đề**: Task phát hiện nhịp tim nhận các clean window có kích thước 128 mẫu với độ trượt 64 mẫu (tức là trùng lặp 50% dữ liệu). Trong code xử lý:
    ```c
    static bool pt_process_window(const ecg_clean_window_t *win, uint8_t *out_bpm) {
        ...
        for (int i = 0; i < win->n_samples; i++) { // n_samples = 128
            float mwi_out = pt_process_sample(win->cleaned[i], prev);
            ...
        }
    }
    ```
*   **Cái ngu**:
    1. Thuật toán Pan-Tompkins hoạt động dựa trên luồng dữ liệu liên tục trong thời gian thực (đạo hàm bậc 1, bình phương, tích phân cửa sổ trượt MWI).
    2. Việc duyệt qua toàn bộ 128 mẫu của từng window trùng lặp đồng nghĩa với việc các mẫu từ chỉ số `64..127` của window trước sẽ bị xử lý **lần thứ hai** dưới dạng các mẫu `0..63` của window tiếp theo!
    3. Trùng lặp mẫu liên tục làm cho luồng tín hiệu truyền vào bộ lọc đạo hàm bị méo mó, biến dạng cấu trúc thời gian của sóng điện tim, làm sai lệch nghiêm trọng ngưỡng thích nghi (`s_pt.threshold`) và gây tính toán sai lệch nhịp tim (BPM).
*   **Giải pháp đúng**: Task HR Detect chỉ được phép xử lý các mẫu mới chưa được xử lý của cửa sổ trượt (tức là từ chỉ số `ECG_WINDOW_OVERLAP` trở đi: `i = 64` đến `127`), hoặc nhận dữ liệu từ một hàng đợi mẫu tuần tự không trùng lặp.

### 2.5. Lỗi "Quên" gửi dữ liệu WebSocket Server (`ecg_network.c`)
*   **Vấn đề**: Trong hàm truyền dữ liệu điện tim `broadcast_ecg()`:
    ```c
    static void broadcast_ecg(const float *samples, int n, uint8_t bpm, bool lead_off) {
        if (g_ws_client_count == 0 || s_server == NULL) return;
        static char json_buf[1024];
        int pos = snprintf(json_buf, sizeof(json_buf), ...);
        ...
        httpd_ws_frame_t ws_pkt = {
            .type    = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)json_buf,
            .len     = (size_t)pos,
        };
        /* Broadcast tới tất cả client */
        /* TODO: Dùng httpd_ws_send_frame_async với task handle để không block */
        ESP_LOGD(TAG, "TX %d bytes to %d clients", pos, g_ws_client_count);
    }
    ```
*   **Cái ngu**: Hàm này định dạng chuỗi JSON rất chi tiết vào `json_buf` và gán cho `ws_pkt`, nhưng sau đó... **chỉ log debug ra console chứ không hề gọi bất kỳ hàm gửi dữ liệu nào**! Hàm truyền tin thực tế (`httpd_ws_send_frame` hoặc lưu danh sách socket descriptors để lặp gửi) hoàn toàn bị khuyết thiếu. Client Web sẽ không bao giờ nhận được bất cứ dữ liệu ECG nào.

### 2.6. Lỗi nuốt (Drop) dữ liệu khi gom gói mạng (`ecg_network.c`)
*   **Vấn đề**: Trong `ecg_network_task()`, dữ liệu được đọc ra khỏi queue mạng và gộp thành các lô có kích thước `NETWORK_BATCH_SIZE` (50 mẫu):
    ```c
    while (xQueueReceive(network_queue, &win, 0) == pdTRUE) {
        for (int i = 0; i < win.n_samples; i++) { // n_samples = 128
            ...
            if (batch_fill < NETWORK_BATCH_SIZE) {
                batch[batch_fill++] = win.cleaned[i];
            }
        }
    }
    ```
*   **Cái ngu**: Mỗi khi nhận được một `ecg_clean_window_t` (có 128 mẫu), vòng lặp chạy từ 0 đến 127. Khi `batch_fill` chạm mốc 50, điều kiện `batch_fill < NETWORK_BATCH_SIZE` bị sai. Từ mẫu thứ 50 đến mẫu thứ 127 của cửa sổ đó **bị bỏ qua hoàn toàn** mà không hề được đóng gói hay gửi đi. Toàn bộ phần dữ liệu sau của mỗi cửa sổ bị nuốt sạch, dẫn tới mất mát hơn $60\%$ tổng lượng dữ liệu điện tim!
*   **Giải pháp đúng**: Khi `batch_fill == NETWORK_BATCH_SIZE`, chương trình phải lập tức kích hoạt việc gửi gói tin hiện tại đi (hoặc đẩy vào hàng đợi gửi) rồi reset `batch_fill = 0` để tiếp tục nhét các mẫu còn lại vào lô tiếp theo.

### 2.7. Lỗi đóng băng màn hình hiển thị OLED (`ecg_display.c`)
*   **Vấn đề**: Đoạn code xử lý nhận dữ liệu để cập nhật sóng trên OLED:
    ```c
    for (int i = 0; i < win.n_samples && s_wave_write < WAVE_WIDTH; i += step) {
        ...
        s_wave[s_wave_write % WAVE_WIDTH] = y;
        s_wave_write++;
    }
    ```
*   **Cái ngu**: 
    1. Biến `s_wave_write` được khởi tạo bằng 0 và tăng lên sau mỗi mẫu ghi được.
    2. Điều kiện vòng lặp yêu cầu `s_wave_write < WAVE_WIDTH` (tức là `< 128`).
    3. Ngay sau khi vẽ hết 128 điểm đầu tiên trên màn hình (chưa tới 1 giây hoạt động), `s_wave_write` đạt giá trị 128. Kể từ đó, điều kiện `s_wave_write < WAVE_WIDTH` luôn sai, vòng lặp ngưng chạy vĩnh viễn và màn hình OLED bị đóng băng (treo cứng sóng ECG) mãi mãi!
*   **Giải pháp đúng**: Bỏ điều kiện `s_wave_write < WAVE_WIDTH` ra khỏi vòng lặp kiểm tra của `for`. Biến này cần được tăng vô hạn và dùng phép chia lấy dư `% WAVE_WIDTH` để ghi đè xoay vòng vào buffer, phục vụ hiệu ứng vẽ sóng cuốn chiếu (rolling/sweep).

### 2.8. Màn hình OLED bị trống trơn do thiếu hàm truyền dữ liệu thực tế (`ecg_display.c`)
*   **Vấn đề**: Trong hàm đẩy dữ liệu lên màn hình OLED `ssd1306_flush()`:
    ```c
    static void ssd1306_flush(void) {
        ssd1306_write_cmd(0x21); ssd1306_write_cmd(0); ssd1306_write_cmd(127);
        ssd1306_write_cmd(0x22); ssd1306_write_cmd(0); ssd1306_write_cmd(7);

        uint8_t header = 0x40;
        i2c_master_write_to_device(OLED_I2C_PORT, OLED_I2C_ADDR, &header, 1, pdMS_TO_TICKS(50));
    }
    ```
*   **Cái ngu**: Đoạn code này chỉ gửi lệnh thiết lập cột/trang, sau đó gửi một byte header điều khiển `0x40` để báo hiệu dữ liệu sắp truyền, nhưng tuyệt nhiên **không hề truyền mảng framebuffer `s_fb`**! Toàn bộ 1024 bytes dữ liệu hiển thị nằm chết dí trong RAM và màn hình OLED sẽ không hiển thị bất cứ thứ gì ngoài màn hình đen hoặc nhiễu rác vật lý lúc bật nguồn.

### 2.9. Phá hủy hiệu năng Web Frontend bằng cách đổi kích thước Canvas liên tục (`web/index.html`)
*   **Vấn đề**: Trong hàm vẽ đồ thị ECG `renderECG()` phía client (được gọi liên tục qua `requestAnimationFrame` ở tốc độ 60 hình/giây):
    ```javascript
    function renderECG() {
      const W = canvas.clientWidth;
      const H = canvas.clientHeight;
      canvas.width  = W;
      canvas.height = H;
      ...
    }
    ```
*   **Cái ngu**: Việc gán lại thuộc tính `canvas.width` và `canvas.height` ở mỗi frame vẽ sẽ xóa sạch trạng thái vẽ hiện tại của Canvas và ép trình duyệt phải thực hiện tính toán lại layout (Reflow) và vẽ lại toàn bộ giao diện (Repaint) từ đầu ở mức phần cứng. Điều này tàn phá hiệu năng CPU/GPU của máy tính/điện thoại truy cập dashboard, gây ra hiện tượng giật lag kinh hoàng và quá nhiệt thiết bị.
*   **Giải pháp đúng**: Chỉ thay đổi kích thước canvas khi kích thước cửa sổ thực sự thay đổi (sử dụng sự kiện `window.onresize` hoặc `ResizeObserver`), tránh gán đè thuộc tính kích thước canvas một cách vô điều kiện ở tần suất 60Hz.

### 2.10. Mâu thuẫn UX/UI khi kết hợp đồ thị cuộn và con trỏ quét (`web/index.html`)
*   **Vấn đề**: Cú pháp vẽ đồ thị của web:
    *   Sóng được vẽ cuộn dịch sang trái nhờ công thức dịch chỉ số: `idx = (writePos + i) % MAX_PTS` (mẫu mới nhất luôn nằm ở rìa phải màn hình).
    *   Nhưng code lại đồng thời vẽ thêm một vạch thẳng con trỏ di chuyển ngang màn hình: `cursorX = (writePos / MAX_PTS) * W` để chỉ ra vị trí ghi hiện tại.
*   **Cái ngu**: Đây là lỗi tư duy thiết kế đồ thị.
    *   Nếu đồ thị thuộc dạng **Cuộn trái (Scrolling)**: Các điểm dữ liệu tự động trượt về bên trái, điểm mới xuất hiện ở bên phải. Loại này không cần và không được có thanh quét dọc di chuyển ngang màn hình.
    *   Nếu đồ thị thuộc dạng **Quét điện tâm đồ truyền thống (Sweeping)**: Sóng đứng yên, một vạch quét chạy từ trái sang phải, ghi đè dữ liệu mới lên trên dữ liệu cũ. Loại này mới cần thanh quét dọc (`cursorX`) để biểu diễn ranh giới giữa dữ liệu mới tinh và dữ liệu cũ của chu kỳ trước.
    *   Việc trộn cả hai làm cho một vạch dọc chạy qua chạy lại một cách vô nghĩa trên một màn hình đồ thị đang cuộn liên tục sang bên trái, gây rối mắt và sai quy chuẩn y tế.

---

## 3. Kết Luận

Dự án hiện tại có một cấu trúc thư mục phân tách khá rõ ràng theo mô hình hướng đối tượng / module hóa của ESP-IDF. Tuy nhiên, chất lượng lập trình chi tiết của các file mã nguồn lại cực kỳ thấp, chứa đầy các bug logic chí mạng khiến cho dự án **hoàn toàn tê liệt ở mọi tính năng** (từ lấy mẫu ADC bị jitter, AI Inference bị kẹt cờ lỗi, đo nhịp tim sai thuật toán lọc, OLED không phát dữ liệu I2C, WebSocket Server không truyền tin, cho đến Web Frontend bị thắt nút cổ chai hiệu năng).

Cần phải tiến hành tái cấu trúc lớn (Refactoring) toàn bộ logic xử lý luồng dữ liệu của các Task để hệ thống có thể chạy ổn định.
