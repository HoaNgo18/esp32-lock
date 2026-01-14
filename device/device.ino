
// THƯ VIỆN
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <WiFiUdp.h> // [NEW] UDP Library

// CẤU HÌNH WIFI & MQTT
const char *ssid = "sushi_trash";
const char *password = "12345677";

// MQTT Broker (Tự động tìm qua UDP)
char mqttServer[20] = ""; // Để trống, sẽ tìm qua UDP
const int mqttPort = 1883;

const char *username = "";
const char *pass = "";

#define LOCK_ID 2  // ID định danh cho khóa này

// MQTT Topics (Được khởi tạo trong setup() với LOCK_ID)
char mqttCmdTopic[32];  // lock/{LOCK_ID}/cmd
char mqttLogTopic[32];  // lock/{LOCK_ID}/log

//  CẤU HÌNH PHẦN CỨNG
// --- OLED Display (I2C) ---
#define I2C_ADDR 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
const int PIN_SDA = 21;
const int PIN_SCL = 22;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Keypad 4x3 ---
const byte ROWS = 4, COLS = 3;
byte rowPins[ROWS] = { 26, 27, 14, 12 };
byte colPins[COLS] = { 32, 33, 25 };
char keys[ROWS][COLS] = {
  { '#', '0', '*' },
  { '9', '8', '7' },
  { '6', '5', '4' },
  { '3', '2', '1' }
};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- LED Trạng thái ---
#define LED_RED 13     // Đỏ: Lỗi/Sai mật khẩu
#define LED_YELLOW 15  // Vàng: Chờ/Bình thường
#define LED_GREEN 4    // Xanh: Mở khóa

//  BIẾN TOÀN CỤC
// --- Kết nối ---
WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;  // Lưu trữ Flash (NVS)

// --- Mật khẩu ---
const char *MASTER_PASS = "180204";  // Mật khẩu chủ (không thể xóa)
String line = "";                    // Chuỗi nhập từ keypad
const uint8_t MAXLEN = 20;           // Giới hạn độ dài nhập

// --- Trạng thái khóa ---
bool unlocked = false;                          // Trạng thái mở khóa
unsigned long unlockTime = 0;                   // Thời điểm mở khóa
const unsigned long AUTO_LOCK_DURATION = 5000;  // Tự động khóa sau 5 giây

// --- Xử lý lỗi ---
uint8_t failCount = 0;                         // Số lần nhập sai
const uint8_t MAX_FAIL_ATTEMPTS = 5;           // Số lần sai tối đa
unsigned long lockoutTime = 0;                 // Thời điểm bắt đầu khóa tạm thời
const unsigned long LOCKOUT_DURATION = 30000;  // Khóa tạm thời 30 giây

bool showingError = false;    // Đang hiển thị lỗi
unsigned long errorTime = 0;  // Thời điểm bắt đầu hiển thị lỗi

// --- Thông báo MQTT ---
String notificationMsg = "";         // Nội dung thông báo
unsigned long notificationTime = 0;  // Thời điểm bắt đầu hiển thị thông báo

// --- Kết nối MQTT (Non-blocking) ---
unsigned long lastMqttReconnectAttempt = 0;  // Thời điểm thử kết nối MQTT cuối

// KHAI BÁO HÀM
void publishLog(String user, String action);
void callback(char *topic, byte *payload, unsigned int length);
void setupWifi();
void reconnect();
void setLEDs(bool r, bool y, bool g);
void drawScreen(const char *status = "", const char *line2 = "");
void checkPassword();
void handleUnlock(String user, bool isOTP);
void handleFailedAttempt();

// UDP Discovery
WiFiUDP udp;
const int UDP_PORT = 12345;

void findBroker() {
  Serial.println("\n→ Searching for Broker (UDP)...");
  drawScreen("Searching Hub...", "Wait for PC...");
  
  udp.begin(UDP_PORT);
  
  bool found = false;
  unsigned long startTime = millis();
  
  while (!found) {
    // Timeout check (30 seconds) -> Fallback to default Hotspot IP
    if (millis() - startTime > 15000) {
       Serial.println("UDP Timeout. Using default IP.");
       sprintf(mqttServer, "192.168.137.1");
       found = true;
       drawScreen("Timeout!", "Using Default IP");
       delay(2000);
       break;
    }

    // Manual Skip Check (Press # to skip)
    char k = keypad.getKey();
    if (k == '#') {
       Serial.println("Manual Skip. Using default IP.");
       sprintf(mqttServer, "192.168.137.1");
       found = true;
       drawScreen("Skipped!", "Using Default IP");
       delay(2000);
       break;
    }

    int packetSize = udp.parsePacket();
    if (packetSize) {
      char packetBuffer[255];
      int len = udp.read(packetBuffer, 255);
      if (len > 0) packetBuffer[len] = 0;
      
      String msg = String(packetBuffer);
      Serial.printf("UDP Received: %s from %s\n", msg.c_str(), udp.remoteIP().toString().c_str());
      
      if (msg.startsWith("ESP32_LOCK_BROKER_HERE")) {
        IPAddress brokerIP = udp.remoteIP();
        sprintf(mqttServer, "%s", brokerIP.toString().c_str());
        found = true;
        
        Serial.printf("✓ Broker Found: %s\n", mqttServer);
        drawScreen("Hub Found!", mqttServer);
        delay(2000);
      }
    }
    delay(100);
  }
  udp.stop();

}

// MQTT - GỬI LOG
// Gửi thông tin log hoạt động lên MQTT broker dạng JSON
void publishLog(String user, String action) {
  JsonDocument doc;
  doc["user"] = user;
  doc["action"] = action;
  doc["lock_id"] = LOCK_ID;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  if (client.publish(mqttLogTopic, jsonBuffer)) {
    Serial.printf("Log sent: %s - %s\n", user.c_str(), action.c_str());
  } else {
    Serial.println("Failed to send log");
  }
}

// MQTT - CALLBACK XỬ LÝ LỆNH
// Nhận và xử lý các lệnh điều khiển từ MQTT broker
void callback(char *topic, byte *payload, unsigned int length) {
  Serial.println("MQTT command received");

  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.println("JSON parse error");
    notificationMsg = "JSON Error!";
    notificationTime = millis();
    return;
  }

  const char *command = doc["command"];

  // REMOTE OPEN
  // Mở khóa từ xa qua MQTT
  if (strcmp(command, "remote_open") == 0) {
    String reqUser = doc["user"] | "Admin";
    handleUnlock(reqUser, false);
    Serial.println("Remote open success");
  }

  // ADD USER
  // Thêm người dùng mới vào hệ thống
  else if (strcmp(command, "add_user") == 0) {
    const char *user_pass = doc["pass"];
    const char *user_name = doc["username"];

    // Kiểm tra tính hợp lệ
    if (!user_pass || !user_name || strlen(user_pass) != 6) {
      Serial.println("✗ Invalid user data");
      notificationMsg = "Invalid Data!";
      notificationTime = millis();
      return;
    }

    // Không cho phép trùng với master password
    if (strcmp(user_pass, MASTER_PASS) == 0) {
      Serial.println("Cannot use master password");
      notificationMsg = "Cannot Use Master!";
      notificationTime = millis();
      return;
    }

    // Lưu vào Flash
    preferences.begin("users", false);
    preferences.putString(user_pass, user_name);
    preferences.end();

    Serial.printf("✓ User added: %s (%s)\n", user_name, user_pass);
    notificationMsg = String("Added: ") + String(user_name);
    publishLog("System", String("User added: ") + String(user_name));
    notificationTime = millis();
  }

  // DELETE USER
  // Xóa người dùng khỏi hệ thống
  else if (strcmp(command, "del_user") == 0) {
    const char *user_pass = doc["pass"];

    preferences.begin("users", false);
    if (preferences.isKey(user_pass)) {
      String deletedUser = preferences.getString(user_pass, "Unknown");
      preferences.remove(user_pass);
      preferences.end();

      Serial.printf("User deleted: %s\n", deletedUser.c_str());
      notificationMsg = String("Deleted: ") + deletedUser;
      publishLog("System", String("User deleted: ") + deletedUser);
    } else {
      preferences.end();
      Serial.println("User not found");
      notificationMsg = "User Not Found!";
    }
    notificationTime = millis();
  }

  // ADD OTP
  // Tạo mã OTP sử dụng một lần
  else if (strcmp(command, "add_otp") == 0) {
    const char *otp_code = doc["pass"];

    // Kiểm tra độ dài OTP
    if (!otp_code || strlen(otp_code) != 6) {
      Serial.println("Invalid OTP length");
      notificationMsg = "OTP must be 6 digits!";
      notificationTime = millis();
      return;
    }

    // Lưu OTP vào Flash
    preferences.begin("otps", false);
    preferences.putString(otp_code, "OTP");
    preferences.end();

    Serial.printf("OTP created: %s\n", otp_code);
    notificationMsg = String("OTP: ") + String(otp_code);
    publishLog("Admin", "created_otp");
    notificationTime = millis();
  }

  else {
    Serial.printf("Unknown command: %s\n", command);
    notificationMsg = "Unknown Cmd!";
    notificationTime = millis();
  }
}

// WIFI - KẾT NỐI
void setupWifi() {
  delay(10);
  Serial.println("\n→ Connecting to WiFi...");
  drawScreen("Connecting WiFi...");

  WiFi.begin(ssid, password);

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed");
    drawScreen("WiFi Failed!", "Check config");
    delay(2000);
  } else {
    Serial.println("\nWiFi connected");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    drawScreen("WiFi Connected!", WiFi.localIP().toString().c_str());
    delay(1500);
  }
}

// MQTT - KẾT NỐI (NON-BLOCKING)
// Kết nối MQTT không chặn vòng lặp chính, chỉ thử kết nối lại mỗi 5 giây một lần
void reconnect() {
  // Nếu đã kết nối rồi thì không làm gì
  if (client.connected()) return;

  // Kiểm tra mỗi 5 giây (Non-blocking)
  unsigned long now = millis();
  if (now - lastMqttReconnectAttempt > 5000) {
    lastMqttReconnectAttempt = now;

    Serial.print("→ Attempting MQTT connection...");

    // Thử kết nối MỘT LẦN, không dùng while
    String cid = String("esp32-lock-") + LOCK_ID;
    if (client.connect(cid.c_str(), username, pass)) {
      Serial.println(" connected");
      client.subscribe(mqttCmdTopic);
      publishLog("System", "Device Online / Reconnected");
    } else {
      Serial.print(" failed, rc=");
      Serial.println(client.state());
    }
  }
}

// LED - ĐIỀU KHIỂN
// Bật/tắt các LED trạng thái
void setLEDs(bool r, bool y, bool g) {
  digitalWrite(LED_RED, r ? HIGH : LOW);
  digitalWrite(LED_YELLOW, y ? HIGH : LOW);
  digitalWrite(LED_GREEN, g ? HIGH : LOW);
}

// OLED - HIỂN THỊ
// Vẽ màn hình với mật khẩu ẩn và thông tin trạng thái
void drawScreen(const char *status, const char *line2) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Dòng 1: Tiêu đề
  oled.setCursor(0, 0);
  oled.println(F("Enter password:"));

  // Dòng 2: Hiển thị mật khẩu dạng dấu *
  oled.setCursor(0, 16);
  oled.print(F("Pass: "));

  // Chỉ hiển thị tối đa 12 ký tự cuối để tránh tràn màn hình
  int startIdx = (line.length() > 12) ? (line.length() - 12) : 0;
  for (uint8_t i = startIdx; i < line.length(); ++i) {
    oled.print('*');
  }

  // Dòng 3: Thông tin trạng thái
  if (status && status[0]) {
    oled.setCursor(0, 32);
    oled.println(status);
  }

  // Dòng 4: Thông tin bổ sung
  if (line2 && line2[0]) {
    oled.setCursor(0, 42);
    oled.println(line2);
  }

  oled.display();
}

// XỬ LÝ MỞ KHÓA THÀNH CÔNG
void handleUnlock(String user, bool isOTP) {
  unlocked = true;
  unlockTime = millis();
  failCount = 0;     // Reset số lần nhập sai
  setLEDs(0, 0, 1);  // LED xanh

  if (isOTP)
    drawScreen("OTP UNLOCK", "Auto-lock 5s");
  else
    drawScreen("UNLOCKED", user.c_str());

  publishLog(user, isOTP ? "otp_used" : "unlocked");
  Serial.printf("Unlocked by: %s\n", user.c_str());
}

// XỬ LÝ NHẬP SAI MẬT KHẨU
void handleFailedAttempt() {
  failCount++;
  showingError = true;
  errorTime = millis();
  setLEDs(1, 0, 0);  // LED đỏ

  char errMsg[32];
  snprintf(errMsg, sizeof(errMsg), "Wrong! (%d/%d)", failCount, MAX_FAIL_ATTEMPTS);
  drawScreen(errMsg);

  publishLog("Unknown", "failed_attempt");
  Serial.printf("Failed attempt #%d\n", failCount);

  // Kích hoạt chế độ khóa tạm thời nếu sai quá nhiều
  if (failCount >= MAX_FAIL_ATTEMPTS) {
    lockoutTime = millis();
    drawScreen("TOO MANY FAILS!", "Locked 30s");
    publishLog("System", "lockout_triggered");
  }
}

// KIỂM TRA MẬT KHẨU (VIRTUAL PASSWORD MODE)
// Sử dụng thuật toán cửa sổ trượt (sliding window) để tìm mật khẩu, ẩn trong chuỗi ký tự ngẫu nhiên
void checkPassword() {
  String input = line;
  int len = input.length();
  const int PASS_LEN = 6;  // Độ dài mật khẩu chuẩn

  // Kiểm tra độ dài tối thiểu
  if (len < PASS_LEN) {
    handleFailedAttempt();
    return;
  }

  bool matchFound = false;
  String foundUser = "";
  bool isOTP = false;

  // THUẬT TOÁN CỬA SỔ TRƯỢT (SLIDING WINDOW). Duyệt qua từng đoạn 6 ký tự liên tiếp trong chuỗi nhập
  // A. KIỂM TRA MASTER PASS & USERS
  preferences.begin("users", true);

  for (int i = 0; i <= len - PASS_LEN; i++) {
    String segment = input.substring(i, i + PASS_LEN);

    // 1. Kiểm tra Master Password
    if (segment == MASTER_PASS) {
      matchFound = true;
      foundUser = "MASTER";
      break;
    }

    // 2. Kiểm tra User Password (trong Flash)
    if (preferences.isKey(segment.c_str())) {
      foundUser = preferences.getString(segment.c_str(), "Unknown");
      matchFound = true;
      break;
    }
  }
  preferences.end();

  // B. KIỂM TRA OTP (nếu chưa tìm thấy User/Master)
  if (!matchFound) {
    preferences.begin("otps", false);
    for (int i = 0; i <= len - PASS_LEN; i++) {
      String segment = input.substring(i, i + PASS_LEN);

      if (preferences.isKey(segment.c_str())) {
        foundUser = "GUEST (OTP)";
        matchFound = true;
        isOTP = true;

        // 1. Xóa OTP trong Flash
        preferences.remove(segment.c_str());

        // 2. [QUAN TRỌNG] Báo cho Server biết để xóa trong Database
        // Backend đang lắng nghe cú pháp "otp_deleted:123456"
        publishLog("System", String("otp_deleted:") + segment);

        Serial.println("OTP used and deleted");
        break;
      }
    }
    preferences.end();
  }

  // XỬ LÝ KẾT QUẢ
  if (matchFound) {
    handleUnlock(foundUser, isOTP);
  } else {
    handleFailedAttempt();
  }

  // Reset chuỗi nhập sau khi kiểm tra
  line = "";
}

// SETUP - KHỞI TẠO HỆ THỐNG
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔═══════════════════════════════╗");
  Serial.println("║   SMART LOCK STARTING...      ║");
  Serial.println("╚═══════════════════════════════╝");

  // --- Khởi tạo OLED ---
  Wire.begin(PIN_SDA, PIN_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR)) {
    Serial.println(F("✗ OLED init failed - check I2C"));
    while (1) delay(10);
  }
  oled.setRotation(0);
  oled.clearDisplay();
  Serial.println("OLED initialized");

  // --- Khởi tạo LED ---
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  setLEDs(0, 1, 0);  // LED vàng mặc định
  Serial.println("LEDs initialized");

  // --- Kết nối WiFi ---
  setupWifi();

  // --- Tìm Broker ---
  findBroker(); // [NEW] Chờ tìm thấy IP máy tính

  // --- Cấu hình MQTT ---
  // Khởi tạo MQTT topics với LOCK_ID
  sprintf(mqttCmdTopic, "lock/%d/cmd", LOCK_ID);
  sprintf(mqttLogTopic, "lock/%d/log", LOCK_ID);
  Serial.printf("MQTT Topics: CMD=%s, LOG=%s\n", mqttCmdTopic, mqttLogTopic);
  
  client.setServer(mqttServer, mqttPort); // mqttServer giờ là biến char[] đã có IP
  client.setCallback(callback);

  // --- Sẵn sàng ---
  drawScreen("*=clear, #=OK");
  Serial.println("SYSTEM READY\n");
}

// LOOP - VÒNG LẶP CHÍNH
void loop() {
  // 1. ƯU TIÊN QUÉT PHÍM (Phản hồi ngay lập tức)
  char k = keypad.getKey();

  // 2. HIỂN THỊ THÔNG BÁO MQTT (2 giây)
  if (notificationTime > 0 && millis() - notificationTime < 2000) {
    drawScreen(notificationMsg.c_str());
    // Vẫn xử lý mạng trong lúc hiển thị thông báo
    if (WiFi.status() == WL_CONNECTED) {
      if (!client.connected()) {
        reconnect();
      } else {
        client.loop();
      }
    }
    return;
  } else if (notificationTime > 0) {
    notificationTime = 0;
    drawScreen("*=clear, #=OK");
  }

  // 3. XỬ LÝ MẠNG (Khi rảnh tay)
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      reconnect();  // Non-blocking reconnect
    } else {
      client.loop();  // Duy trì kết nối
    }
  }

  // 4. KIỂM TRA LOCKOUT (Khóa tạm thời 30s)
  if (lockoutTime > 0) {
    if (millis() - lockoutTime < LOCKOUT_DURATION) {
      unsigned long remaining = (LOCKOUT_DURATION - (millis() - lockoutTime)) / 1000;
      char msg[32];
      snprintf(msg, sizeof(msg), "Locked: %lus", remaining);
      drawScreen("SYSTEM LOCKOUT", msg);
      delay(200);  // Delay nhỏ để giảm CPU load
      return;
    } else {
      // Hết thời gian khóa
      lockoutTime = 0;
      failCount = 0;
      drawScreen("Lockout ended", "*=clear, #=OK");
      delay(2000);
    }
  }

  // 5. TIMEOUT HIỂN THỊ LỖI (3 giây)
  if (showingError && millis() - errorTime >= 3000) {
    showingError = false;
    line = "";
    setLEDs(0, 1, 0);
    drawScreen("*=clear, #=OK");
    return;
  }

  // 6. XỬ LÝ TRẠNG THÁI MỞ KHÓA
  if (unlocked) {
    // A. Kiểm tra auto-lock timeout (5 giây)
    if (unlockTime > 0 && millis() - unlockTime >= AUTO_LOCK_DURATION) {
      unlocked = false;
      unlockTime = 0;
      line = "";
      setLEDs(0, 1, 0);
      drawScreen("AUTO-LOCKED", "System locked");
      publishLog("System", "auto_locked");
      Serial.println("Auto-locked");
      delay(1500);
      drawScreen("*=clear, #=OK");
      return;
    }

    // B. Hiển thị đếm ngược auto-lock
    if (unlockTime > 0) {
      unsigned long remaining = (AUTO_LOCK_DURATION - (millis() - unlockTime)) / 1000;
      char msg[32];
      snprintf(msg, sizeof(msg), "Locking in %lus", remaining + 1);
      drawScreen("UNLOCKED", msg);
    }

    // C. Cho phép khóa thủ công bằng phím *
    if (k == '*') {
      unlocked = false;
      unlockTime = 0;
      line = "";
      setLEDs(0, 1, 0);
      drawScreen("LOCKED", "Press * to clear");
      publishLog("System", "manual_locked");
      Serial.println("Manual locked");
      delay(1500);
      drawScreen("*=clear, #=OK");
    }
    return;
  }

  // 7. XỬ LÝ NHẬP LIỆU TỪ KEYPAD (Trạng thái khóa)
  if (!k) return;  // Không có phím nào được nhấn

  if (k == '*') {
    // Xóa chuỗi nhập
    line = "";
    drawScreen("Cleared");
    delay(500);
    drawScreen("*=clear, #=OK");
    Serial.println("🗑 Input cleared");
  } else if (k == '#') {
    // Kiểm tra mật khẩu
    if (line.length() > 0) {
      Serial.printf("Checking password: %s\n", line.c_str());
      checkPassword();
    } else {
      drawScreen("Enter password!");
      delay(1000);
      drawScreen("*=clear, #=OK");
    }
  } else {
    // Thêm ký tự vào chuỗi nhập
    if (line.length() < MAXLEN) {
      line += k;
      Serial.printf("Key: %c (Length: %d)\n", k, line.length());
      drawScreen();
    } else {
      drawScreen("Max length!", "Press # to check");
      delay(1000);
      drawScreen();
    }
  }
}