# ESP32 MQTT Smart Lock (IoT System)

Hệ thống khóa thông minh IoT hoàn chỉnh, bao gồm thiết bị phần cứng **ESP32**, **Backend Server** (Node.js + MongoDB) để lưu trữ dữ liệu, và giao diện **Web Admin** quản lý từ xa qua giao thức MQTT.

## Cấu trúc dự án

* `backend/`: Server Node.js xử lý logic, kết nối database và MQTT.
* `frontend/`: Giao diện Web (HTML/CSS/JS) cho người quản trị.
* `device.ino`: Firmware nạp cho mạch ESP32.

## Phần cứng & Sơ đồ nối dây (Pinout)

**Linh kiện:** ESP32, Keypad 4x3, Màn hình OLED (I2C), 3 đèn LED.

| Thiết bị | Chân ESP32 (GPIO) | Ghi chú |
| :--- | :--- | :--- |
| **OLED SSD1306** | SDA: 21, SCL: 22 | Giao tiếp I2C |
| **Keypad (Hàng)** | 26, 27, 14, 12 | R1, R2, R3, R4 |
| **Keypad (Cột)** | 32, 33, 25 | C1, C2, C3 |
| **LED Trạng thái** | Đỏ: 13, Vàng: 15, Xanh: 4 | |

## Cài đặt & Triển khai

### 1. Yêu cầu phần mềm

* **Node.js** (v14 trở lên): [Tải về tại đây](https://nodejs.org/).
* **MongoDB Community Server**: [Tải về tại đây](https://www.mongodb.com/try/download/community).
* **Mosquitto MQTT Broker**: [Tải về tại đây](https://mosquitto.org/download/).
* **Arduino IDE** (để nạp code cho ESP32).

### 2. Khởi chạy database (MongoDB)
* Tạo connection mới
* Nếu cài đặt mặc định, MongoDB sẽ tự chạy ngầm (Service) tại cổng 27017

### 3. Mosquitto MQTT Broker
* Cài đặt cấu hình cho file `mosquitto.conf`:

```text
listener 1883
protocol mqtt
allow_anonymous true

listener 9001
protocol websockets
allow_anonymous true
```
* Mở cmd (run as administrator), trỏ vào thư mục cài đặt mosquitto và chạy lệnh:
```
.\mosquitto -c mosquitto.conf -v
```

### 4. Thiết lập Backend (Server)

Backend đóng vai trò trung gian lưu trữ dữ liệu User và Log.

1.  Truy cập thư mục backend:
    ```bash
    cd backend
    ```
2.  Cài đặt thư viện:
    ```bash
    npm install express mongoose mqtt cors body-parser
    ```
3.  Cấu hình file `server.js` (nếu cần đổi IP MQTT/MongoDB).
4.  Chạy server:
    ```bash
    node server.js
    ```
    *Server sẽ chạy tại: `http://localhost:3001`*

### 5. Nạp code cho ESP32 (Firmware)

* **Thư viện cần cài (Arduino IDE):** `PubSubClient`, `ArduinoJson`, `Keypad`, `Adafruit SSD1306`, `Adafruit GFX`.
* **Cấu hình:** Mở file `device.ino` và chỉnh sửa:
    * `ssid`, `password`: Thông tin WiFi.
    * `mqttServer`: Địa chỉ IP máy tính chạy Broker.
* Nạp code vào board ESP32.

### 6. Chạy Web Admin (Frontend)

* Mở file `frontend/script.js`.
* Cập nhật `MQTT_SERVER` (IP máy tính) và `API_URL` (ví dụ: `http://localhost:3001/api`).
* Mở file `frontend/index.html` trực tiếp trên trình duyệt để sử dụng.

## Tính năng chính

1.  **Quản lý tập trung:**
    * Dữ liệu Người dùng và Nhật ký (Log) được lưu trữ bền vững trong **MongoDB**.
    * Web Frontend tự động đồng bộ dữ liệu từ Database khi tải lại trang hoặc có sự kiện mới.

2.  **Điều khiển thời gian thực (Real-time):**
    * Thêm/Xóa người dùng
    * Mở khóa từ xa qua MQTT.
    * Trạng thái khóa cập nhật tức thì lên Web.

3.  **Bảo mật & Tiện ích:**
    * **Mã OTP:** Tạo mã dùng 1 lần cho khách. Hệ thống tự động xóa mã khỏi Database ngay khi ESP32 báo cáo đã sử dụng.
    * **Master Password:** `180204` (Mặc định cứng trong code).
    * Bảo vệ chống dò mã (Khóa tạm thời sau 5 lần nhập sai).
    * Tính năng mã ảo: Chèn đoạn password thật giữa 2 đoạn mã ngẫu nhiên để chống nhìn trộm


