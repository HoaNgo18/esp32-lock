// Thư viện
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

// Cấu hình WiFi & MQTT
const char *ssid = "sushi_trash";
const char *password = "12345677";

// --- BẠN ĐIỀN NỐT IP MÁY TÍNH VÀO ĐÂY ---
const char *mqttServer = "192.168.1.7"; 
const int mqttPort = 1883; // Cổng 1883 (TCP thường)
const char *username = "";
const char *pass = "";

#define LOCK_ID 2

// MQTT topics
const char *mqttCmdTopic = "lock/cmd";
const char *mqttLogTopic = "lock/log";

// TLS client cho MQTT 
WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;

// Phần cứng
#define I2C_ADDR 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
const int PIN_SDA = 21;
const int PIN_SCL = 22;
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Keypad
const byte ROWS = 4, COLS = 3;
byte rowPins[ROWS] = {26, 27, 14, 12};
byte colPins[COLS] = {32, 33, 25};
char keys[ROWS][COLS] = {
    {'#', '0', '*'},
    {'9', '8', '7'},
    {'6', '5', '4'},
    {'3', '2', '1'}};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

#define LED_RED 13
#define LED_YELLOW 15
#define LED_GREEN 4

// Mật khẩu master
const char *MASTER_PASS = "180204";

// Trạng thái
String line = "";
const uint8_t MAXLEN = 20;
bool unlocked = false;
unsigned long errorTime = 0;
bool showingError = false;
unsigned long notificationTime = 0;
String notificationMsg = "";
unsigned long unlockTime = 0;
const unsigned long AUTO_LOCK_DURATION = 5000; // 5 giây
uint8_t failCount = 0;
unsigned long lockoutTime = 0;
const uint8_t MAX_FAIL_ATTEMPTS = 5;
const unsigned long LOCKOUT_DURATION = 30000; // 30 giây

// Prototype
void publishLog(String user, String action);
void callback(char *topic, byte *payload, unsigned int length);
void setupWifi();
void reconnect();
void setLEDs(bool r, bool y, bool g);
void drawScreen(const char *status = "", const char *line2 = "");
void checkPassword();
void handleUnlock(String user, bool isOTP);
void handleFailedAttempt();

// Gửi log MQTT
void publishLog(String user, String action)
{
  // Dùng JsonDocument thay cho StaticJsonDocument (ArduinoJson v7)
  JsonDocument doc;
  doc["user"] = user;
  doc["action"] = action;
  doc["lock_id"] = LOCK_ID; // Thêm lock_id để phân biệt nhiều khóa

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  if (client.publish(mqttLogTopic, jsonBuffer))
  {
    Serial.printf("Log sent: %s - %s\n", user.c_str(), action.c_str());
  }
  else
  {
    Serial.println("Failed to send log");
  }
}

// Xử lý lệnh MQTT
void callback(char *topic, byte *payload, unsigned int length)
{
  Serial.println("Received MQTT command");

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error)
  {
    Serial.println("JSON parse error");
    notificationMsg = "JSON Error!";
    notificationTime = millis();
    return;
  }

  const char *command = doc["command"];

  // REMOTE OPEN
  if (strcmp(command, "remote_open") == 0)
  {
    String reqUser = doc["user"] | "Admin";
    handleUnlock(reqUser, false);
    Serial.println("Remote open success");
  }
  // ADD USER 
  else if (strcmp(command, "add_user") == 0)
  {
    const char *user_pass = doc["pass"];
    const char *user_name = doc["username"];

    // Validate
    if (!user_pass || !user_name || strlen(user_pass) != 6)
    {
      Serial.println("Invalid user data");
      notificationMsg = "Invalid Data!";
      notificationTime = millis();
      return;
    }

    // Kiểm tra trùng với master pass
    if (strcmp(user_pass, MASTER_PASS) == 0)
    {
      Serial.println("Cannot use master password");
      notificationMsg = "Cannot Use Master!";
      notificationTime = millis();
      return;
    }

    preferences.begin("users", false);
    preferences.putString(user_pass, user_name);
    preferences.end();

    Serial.printf("User added: %s (%s)\n", user_name, user_pass);
    notificationMsg = String("Added: ") + String(user_name);
    publishLog("System", String("User added: ") + String(user_name));
    notificationTime = millis();
  }
  // DELETE USER 
  else if (strcmp(command, "del_user") == 0)
  {
    const char *user_pass = doc["pass"];

    preferences.begin("users", false);
    if (preferences.isKey(user_pass))
    {
      String deletedUser = preferences.getString(user_pass, "Unknown");
      preferences.remove(user_pass);
      preferences.end();

      Serial.printf("User deleted: %s\n", deletedUser.c_str());
      notificationMsg = String("Deleted: ") + deletedUser;
      publishLog("System", String("User deleted: ") + deletedUser);
    }
    else
    {
      preferences.end();
      Serial.println("User not found");
      notificationMsg = "User Not Found!";
    }
    notificationTime = millis();
  }
  // ADD OTP 
  else if (strcmp(command, "add_otp") == 0)
  {
    const char *otp_code = doc["pass"];

    // Validate
    if (!otp_code || strlen(otp_code) != 6)
    {
      Serial.println("Invalid OTP length");
      notificationMsg = "OTP must be 6 digits!";
      notificationTime = millis();
      return;
    }

    preferences.begin("otps", false);
    preferences.putString(otp_code, "OTP");
    preferences.end();

    Serial.printf("OTP created: %s\n", otp_code);
    notificationMsg = String("OTP: ") + String(otp_code);
    publishLog("Admin", "created_otp");
    notificationTime = millis();
  }
  else
  {
    Serial.printf("Unknown command: %s\n", command);
    notificationMsg = "Unknown Cmd!";
    notificationTime = millis();
  }
}

// Kết nối WiFi
void setupWifi()
{
  delay(10);
  Serial.print("Connecting to WiFi");
  drawScreen("Connecting WiFi...");

  WiFi.begin(ssid, password);

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40)
  {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("\nWiFi connection failed");
    drawScreen("WiFi Failed!", "Restarting...");
    delay(2000);
    // ESP.restart(); // Tạm tắt restart để debug
  } else {
    Serial.println("\nWiFi connected");
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    drawScreen("WiFi Connected!", WiFi.localIP().toString().c_str());
    delay(1500);
  }
}

void reconnect()
{
  if (WiFi.status() != WL_CONNECTED) return;

  while (!client.connected())
  {
    Serial.print("Connecting to MQTT...");
    drawScreen("MQTT Connecting...");

    String cid = String("esp32-lock-") + LOCK_ID;
    if (client.connect(cid.c_str(), username, pass))
    {
      Serial.println("OK");
      drawScreen("MQTT Connected!");
      delay(1000);

      client.subscribe(mqttCmdTopic);
      Serial.printf("Subscribed: %s\n", mqttCmdTopic);
    }
    else
    {
      Serial.printf("Failed, rc=%d. Retry in 5s\n", client.state());
      drawScreen("MQTT Failed", String(client.state()).c_str());
      delay(5000);
    }
  }
}

void setLEDs(bool r, bool y, bool g)
{
  digitalWrite(LED_RED, r ? HIGH : LOW);
  digitalWrite(LED_YELLOW, y ? HIGH : LOW);
  digitalWrite(LED_GREEN, g ? HIGH : LOW);
}

void drawScreen(const char *status, const char *line2)
{
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  oled.setCursor(0, 0);
  oled.println(F("Enter password:"));

  oled.setCursor(0, 16);
  oled.print(F("Pass: "));
  
  // Chỉ hiển thị tối đa 12 ký tự cuối để tránh tràn màn hình
  int startIdx = (line.length() > 12) ? (line.length() - 12) : 0;
  for (uint8_t i = startIdx; i < line.length(); ++i)
  {
    oled.print('*');
  }

  if (status && status[0])
  {
    oled.setCursor(0, 32);
    oled.println(status);
  }
  if (line2 && line2[0])
  {
    oled.setCursor(0, 42);
    oled.println(line2);
  }
  oled.display();
}

// Hàm xử lý khi mở khóa thành công
void handleUnlock(String user, bool isOTP) {
  unlocked = true;
  unlockTime = millis();
  failCount = 0; // Reset fail count khi remote open
  setLEDs(0, 0, 1);
  
  if (isOTP)
    drawScreen("OTP UNLOCK", "Auto-lock 5s");
  else
    drawScreen("UNLOCKED", user.c_str());
    
  publishLog(user, isOTP ? "otp_used" : "unlocked");
  Serial.printf("Unlocked by: %s\n", user.c_str());
}

// Hàm xử lý khi nhập sai
void handleFailedAttempt() {
  failCount++;
  showingError = true;
  errorTime = millis();
  setLEDs(1, 0, 0); // Đèn đỏ

  char errMsg[32];
  snprintf(errMsg, sizeof(errMsg), "Wrong! (%d/%d)", failCount, MAX_FAIL_ATTEMPTS);
  drawScreen(errMsg);

  publishLog("Unknown", "failed_attempt");
  Serial.printf("Failed attempt #%d\n", failCount);

  // Trigger lockout nếu sai quá nhiều
  if (failCount >= MAX_FAIL_ATTEMPTS)
  {
    lockoutTime = millis();
    drawScreen("TOO MANY FAILS!", "Locked 30s");
    publishLog("System", "lockout_triggered");
  }
}

// ==========================================
// LOGIC CHECK PASSWORD (VIRTUAL PASSWORD MODE)
// ==========================================
void checkPassword()
{
  String input = line;
  int len = input.length();
  const int PASS_LEN = 6; // Độ dài quy định của mật khẩu thật

  // 1. Kiểm tra độ dài tối thiểu
  if (len < PASS_LEN)
  {
    handleFailedAttempt();
    return;
  }

  bool matchFound = false;
  String foundUser = "";
  bool isOTP = false;

  // ----------------------------------------
  // THUẬT TOÁN CỬA SỔ TRƯỢT (SLIDING WINDOW)
  // ----------------------------------------
  
  // A. KIỂM TRA MASTER PASS & USERS
  preferences.begin("users", true); 

  for (int i = 0; i <= len - PASS_LEN; i++)
  {
    String segment = input.substring(i, i + PASS_LEN);

    // 1. Check Master
    if (segment == MASTER_PASS)
    {
      matchFound = true;
      foundUser = "MASTER";
      break; 
    }

    // 2. Check Users (Trong bộ nhớ Flash)
    if (preferences.isKey(segment.c_str()))
    {
      foundUser = preferences.getString(segment.c_str(), "Unknown");
      matchFound = true;
      break;
    }
  }
  preferences.end(); 

  // B. KIỂM TRA OTP (Nếu chưa tìm thấy User/Master)
  if (!matchFound)
  {
    preferences.begin("otps", false); 
    for (int i = 0; i <= len - PASS_LEN; i++)
    {
      String segment = input.substring(i, i + PASS_LEN);

      if (preferences.isKey(segment.c_str()))
      {
        foundUser = "GUEST (OTP)";
        matchFound = true;
        isOTP = true;
        
        // Xóa OTP ngay lập tức
        preferences.remove(segment.c_str());
        Serial.println("OTP used and deleted");
        break;
      }
    }
    preferences.end();
  }

  // ----------------------------------------
  // XỬ LÝ KẾT QUẢ
  // ----------------------------------------
  if (matchFound)
  {
    handleUnlock(foundUser, isOTP);
  }
  else
  {
    handleFailedAttempt();
  }

  // Reset chuỗi nhập sau khi kiểm tra
  line = "";
}

// SETUP
void setup()
{
  Serial.begin(115200);
  Serial.println("\n=== SMART LOCK STARTING ===");

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, I2C_ADDR))
  {
    Serial.println(F("OLED init failed - check I2C"));
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

  drawScreen("*=clear, #=OK");
  Serial.println("READY");
}

// LOOP
void loop()
{
  // === MQTT NOTIFICATION ===
  if (notificationTime > 0 && millis() - notificationTime < 2000)
  {
    drawScreen(notificationMsg.c_str());
    return;
  }
  else if (notificationTime > 0)
  {
    notificationTime = 0;
    drawScreen("*=clear, #=OK");
  }

  // === MAINTAIN MQTT CONNECTION ===
  if (!client.connected())
  {
    reconnect();
  }
  client.loop();

  // === LOCKOUT CHECK ===
  if (lockoutTime > 0)
  {
    if (millis() - lockoutTime < LOCKOUT_DURATION)
    {
      unsigned long remaining = (LOCKOUT_DURATION - (millis() - lockoutTime)) / 1000;
      char msg[32];
      snprintf(msg, sizeof(msg), "Locked: %lus", remaining);
      drawScreen("SYSTEM LOCKOUT", msg);
      delay(200); // Giảm delay để nút bấm nhạy hơn nếu cần thoát
      return;
    }
    else
    {
      lockoutTime = 0;
      failCount = 0;
      drawScreen("Lockout ended", "*=clear, #=OK");
      delay(2000);
    }
  }

  // === ERROR TIMEOUT ===
  if (showingError && millis() - errorTime >= 3000)
  {
    showingError = false;
    line = "";
    setLEDs(0, 1, 0);
    drawScreen("*=clear, #=OK");
    return;
  }

  // UNLOCKED STATE
  if (unlocked)
  {
    // Check auto-lock timeout
    if (unlockTime > 0 && millis() - unlockTime >= AUTO_LOCK_DURATION)
    {
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

    // Display countdown
    if (unlockTime > 0)
    {
      unsigned long remaining = (AUTO_LOCK_DURATION - (millis() - unlockTime)) / 1000;
      char msg[32];
      snprintf(msg, sizeof(msg), "Locking in %lus", remaining + 1);
      drawScreen("UNLOCKED", msg);
    }

    // Check manual lock
    char k = keypad.getKey();
    if (k == '*')
    {
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

  // === KEYPAD INPUT (LOCKED STATE) ===
  char k = keypad.getKey();
  if (!k)
    return;

  if (k == '*')
  {
    line = "";
    drawScreen("Cleared");
    delay(500);
    drawScreen("*=clear, #=OK");
    Serial.println("Input cleared");
  }
  else if (k == '#')
  {
    if (line.length() > 0)
    {
      Serial.printf("Checking password: %s\n", line.c_str());
      checkPassword();
    }
    else
    {
      drawScreen("Enter password!");
      delay(1000);
      drawScreen("*=clear, #=OK");
    }
  }
  else
  {
    if (line.length() < MAXLEN)
    {
      line += k;
      Serial.printf("Key: %c (Length: %d)\n", k, line.length());
      drawScreen();
    }
    else
    {
      drawScreen("Max length!", "Press # to check");
      delay(1000);
      drawScreen();
    }
  }
}