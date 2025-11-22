/*
 * SMART LOCK — WiFi + MQTT + Preferences (TLS 8883)
 * - Nhận lệnh add/del user qua MQTT
 * - Gửi log mở/khóa qua MQTT
 */

#include <WiFi.h>
#include <WiFiClientSecure.h> // Dùng TLS cho MQTT 8883
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

// Cấu hình WiFi & MQTT
const char* ssid = "sushi_trash";
const char* password = "12345677";
const char* mqttServer = "e8d92a81786b4564b07b2759213912d0.s1.eu.hivemq.cloud";
const int mqttPort = 8883;
const char* username = "esp32_user";
const char* pass = "Password123";

#define LOCK_ID 2

// MQTT topics (WebAdmin -> lock) và (lock -> WebAdmin)
const char* mqttCmdTopic = "lock/cmd";
const char* mqttLogTopic = "lock/log";

// TLS client cho MQTT
WiFiClientSecure secureClient;
PubSubClient client(secureClient);
Preferences preferences; // Lưu user vào flash (persist)

// Phần cứng
#define I2C_ADDR      0x3C
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
const int PIN_SDA = 21;
const int PIN_SCL = 22;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Keypad
const byte ROWS = 4, COLS = 3;
byte rowPins[ROWS] = { 26, 27, 14, 12 };
byte colPins[COLS] = { 32, 33, 25 };
char keys[ROWS][COLS] = {
  {'#','0','*'},
  {'9','8','7'},
  {'6','5','4'},
  {'3','2','1'}
};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define LED_RED    13
#define LED_YELLOW 15
#define LED_GREEN  4

// Mật khẩu master
const char* MASTER_PASS = "180204";

// Trạng thái
String line = "";
const uint8_t MAXLEN = 6;
bool unlocked = false;
unsigned long errorTime = 0;
bool showingError = false;
unsigned long notificationTime = 0;
String notificationMsg = "";

// Prototype
void publishLog(String user, String action);
void callback(char* topic, byte* payload, unsigned int length);
void setupWifi();
void reconnect();
void setLEDs(bool r, bool y, bool g);
void drawScreen(const char* status = "", const char* line2 = "");
void checkPassword();

// Gửi log MQTT
void publishLog(String user, String action) {
  StaticJsonDocument<200> doc;
  doc["user"] = user;
  doc["action"] = action;

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  if (client.publish(mqttLogTopic, jsonBuffer)) {
    Serial.printf("Log sent: %s - %s\n", user.c_str(), action.c_str());
  } else {
    Serial.println("Failed to send log");
  }
}

// Xử lý lệnh MQTT; không dùng delay() trong callback
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Nhan lenh MQTT: ");

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.println("JSON error");
    notificationMsg = "JSON Error!";
    notificationTime = millis();
    return;
  }

  const char* command = doc["command"];  // "add_user" / "del_user"
  const char* user_pass = doc["pass"];

  if (strcmp(command, "add_user") == 0) {
    const char* user_name = doc["name"];
    // Lưu bền vào flash
    preferences.begin("users", false);
    preferences.putString(user_pass, user_name);
    preferences.end();

    Serial.printf("Da them user: %s (pass: %s)\n", user_name, user_pass);
    notificationMsg = String("User Added: ") + user_name;
    notificationTime = millis();

  } else if (strcmp(command, "del_user") == 0) {
    preferences.begin("users", false);
    if (preferences.isKey(user_pass)) {
      String deletedUser = preferences.getString(user_pass, "Unknown");
      preferences.remove(user_pass);
      Serial.printf("Da xoa user: %s (pass: %s)\n", deletedUser.c_str(), user_pass);
      notificationMsg = String("User Deleted: ") + deletedUser;
    } else {
      Serial.printf("User pass %s khong ton tai\n", user_pass);
      notificationMsg = "User Not Found!";
    }
    preferences.end();
    notificationTime = millis();

  } else {
    Serial.printf("Lenh khong hop le: %s\n", command);
    notificationMsg = "Invalid Command!";
    notificationTime = millis();
  }
}

// Kết nối WiFi (timeout 20s, rồi restart)
void setupWifi() {
  delay(10);
  Serial.print("Dang ket noi WiFi");
  drawScreen("Connecting WiFi...");

  WiFi.begin(ssid, password);

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nKhong ket noi duoc WiFi");
    drawScreen("WiFi Failed!", "Restarting...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("\nWiFi da ket noi");
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  drawScreen("WiFi Connected!", WiFi.localIP().toString().c_str());
  delay(1500);
}

// Kết nối lại MQTT; chú ý setInsecure() chỉ dùng thử nghiệm
void reconnect() {
  while (!client.connected()) {
    Serial.print("Dang ket noi MQTT...");
    drawScreen("MQTT Connecting...");

    // Chỉ dùng trong môi trường thử nghiệm; production hãy dùng CA/cert hợp lệ
    secureClient.setInsecure();

    String cid = String("esp32-lock-") + LOCK_ID;
    if (client.connect(cid.c_str(), username, pass)) {
      Serial.println("OK");
      drawScreen("MQTT Connected!");
      delay(1000);

      // Subscribe nhận lệnh
      client.subscribe(mqttCmdTopic);
      Serial.printf("Subscribed: %s\n", mqttCmdTopic);

    } else {
      Serial.printf("Loi, rc=%d. Thu lai sau 5s\n", client.state());
      drawScreen("MQTT Failed", String(client.state()).c_str());
      delay(5000);
    }
  }
}

void setLEDs(bool r, bool y, bool g) {
  digitalWrite(LED_RED, r ? HIGH : LOW);
  digitalWrite(LED_YELLOW, y ? HIGH : LOW);
  digitalWrite(LED_GREEN, g ? HIGH : LOW);
}

void drawScreen(const char* status, const char* line2) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.println(F("Nhap mat khau:"));

  oled.setCursor(0, 16);
  oled.print(F("Pass: "));
  for (uint8_t i = 0; i < line.length(); ++i) {
    oled.print('*');
  }

  if (status && status[0]) {
    oled.setCursor(0, 32);
    oled.println(status);
  }
  if (line2 && line2[0]) {
    oled.setCursor(0, 42);
    oled.println(line2);
  }
  oled.display();
}

// Kiểm tra mật khẩu: ưu tiên MASTER; nếu không thì tra Preferences
void checkPassword() {
  if (line == MASTER_PASS) {
    unlocked = true;
    setLEDs(0, 0, 1);
    drawScreen("MASTER UNLOCK");
    publishLog("MASTER", "unlocked");
    Serial.println("MASTER unlocked");
  } else {
    preferences.begin("users", true);
    bool userExists = preferences.isKey(line.c_str());

    if (userExists) {
      String userName = preferences.getString(line.c_str(), "Unknown");
      preferences.end();

      unlocked = true;
      setLEDs(0, 0, 1);
      drawScreen("USER UNLOCK", userName.c_str());
      publishLog(userName, "unlocked");
      Serial.printf("USER unlocked: %s\n", userName.c_str());
    } else {
      preferences.end();
      showingError = true;
      errorTime = millis();
      setLEDs(1, 0, 0);
      drawScreen("Wrong Password");
      Serial.println("Wrong password");
    }
  }
  line = "";
}

// SETUP & LOOP
void setup() {
  Serial.begin(115200);
  Serial.println("\nSMART LOCK STARTING");

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR)) {
    Serial.println(F("Loi OLED - Kiem tra I2C"));
    while (1) delay(10);
  }
  oled.setRotation(0);
  oled.clearDisplay();
  Serial.println("OLED OK");

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  setLEDs(0, 1, 0);
  Serial.println("LED OK");

  setupWifi();
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);

  // (Tùy chọn) Reset toàn bộ user đã lưu:
  /*
  preferences.begin("users", false);
  preferences.clear();
  preferences.end();
  Serial.println("Da xoa tat ca user");
  */

  drawScreen("*=clear, #=OK");
  Serial.println("READY");
}

void loop() {
  // Thông báo ngắn sau khi nhận lệnh MQTT
  if (notificationTime > 0 && millis() - notificationTime < 2000) {
    drawScreen(notificationMsg.c_str());
    return;
  } else if (notificationTime > 0) {
    notificationTime = 0;
    drawScreen("*=clear, #=OK");
  }

  // Duy trì kết nối MQTT; phải gọi client.loop() thường xuyên
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Hết trạng thái lỗi sau 3s
  if (showingError && millis() - errorTime >= 3000) {
    showingError = false;
    line = "";
    setLEDs(0, 1, 0);
    drawScreen("*=clear, #=OK");
    return;
  }

  // Đang mở khóa: nhấn * để khóa lại
  if (unlocked) {
    char k = keypad.getKey();
    if (k == '*') {
      unlocked = false;
      line = "";
      setLEDs(0, 1, 0);
      drawScreen("LOCKED", "*=clear, #=OK");
      publishLog("System", "locked");
      Serial.println("Locked");
    }
    return;
  }

  // Nhập phím
  char k = keypad.getKey();
  if (!k) return;

  if (k == '*') {
    line = "";
    drawScreen("Cleared");
    delay(500);
    drawScreen("*=clear, #=OK");
    Serial.println("Cleared");
  } else if (k == '#') {
    if (line.length() > 0) {
      Serial.printf("Checking password: %s\n", line.c_str());
      checkPassword();
    } else {
      drawScreen("Enter password!");
      delay(1000);
      drawScreen("*=clear, #=OK");
    }
  } else {
    if (line.length() < MAXLEN) {
      line += k;
      Serial.printf("Key: %c (Length: %d)\n", k, line.length());
    } else {
      drawScreen("Max length!", "Press # to check");
      delay(1000);
    }
    drawScreen();
  }
}
