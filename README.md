# ESP32 MQTT Smart Lock

Hệ thống khóa thông minh sử dụng **ESP32**, cho phép mở khóa bằng mật khẩu, quản lý người dùng từ xa qua Web Admin và theo dõi nhật ký hoạt động qua giao thức **MQTT**.

## Phần cứng & Sơ đồ nối dây (Pinout)

**Linh kiện:** ESP32, Keypad 4x3, Màn hình OLED (I2C), 3 đèn LED.

| Thiết bị | Chân ESP32 (GPIO) | Ghi chú |
| :--- | :--- | :--- |
| **OLED SSD1306** | SDA: 21, SCL: 22 | Giao tiếp I2C |
| **Keypad (Hàng)** | 26, 27, 14, 12 | R1, R2, R3, R4 |
| **Keypad (Cột)** | 32, 33, 25 | C1, C2, C3 |
| **LED Trạng thái** | Đỏ: 13, Vàng: 15, Xanh: 4 | |

## Cài đặt

### 1. Nạp code cho ESP32 (Firmware)
* **Thư viện cần cài (Arduino IDE):** `PubSubClient`, `ArduinoJson`, `Keypad`, `Adafruit SSD1306`, `Adafruit GFX`.
* **Cấu hình:** Mở file `device_v5.ino`, chỉnh sửa thông tin `ssid`, `password` (WiFi) và `mqttServer`.
* Nạp code vào board ESP32.

### 2. Chạy Web Admin
* Mở file `script.js`, cập nhật thông tin MQTT Broker (lưu ý dùng Port WebSocket, ví dụ 8884).
* Chạy file `index.html` để sử dụng bảng điều khiển.

## Tính năng chính
* **Quản lý từ xa:** Thêm/Xóa người dùng và mật khẩu qua giao diện Web.
* **Nhật ký (Log):** Xem lịch sử ra/vào theo thời gian thực.
* **Bảo mật:** Mật khẩu người dùng được lưu trong bộ nhớ Flash (không mất khi tắt nguồn).
* **Thao tác:**
    * Nhập mật khẩu + phím `#` để mở.
    * Nhấn `*` để xóa nhập liệu hoặc khóa lại.
    * **Master Password mặc định:** 180204.