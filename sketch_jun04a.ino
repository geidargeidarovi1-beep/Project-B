#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "georgian_font.h"

#define TFT_CS 32
#define TFT_DC 33
#define TFT_RST 4

#define SS_PIN 16
#define RST_PIN 17
#define SCK_PIN 18
#define MOSI_PIN 23
#define MISO_PIN 19

#define SDA_PIN 21
#define SCL_PIN 22
#define PCF_ADDR 0x26

#define COIN_PIN 34
#define COIN_GAP_MS 500

#define OUT_PIN 13
#define OUT_PIN2 14
#define IN_PIN 27

MFRC522 mfrc522(SS_PIN, RST_PIN);
Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;
WebServer server(80);
Preferences prefs;
WiFiClientSecure secureClient;
PubSubClient mqttClient(secureClient);

const char keyMap[4][3] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

// PCF8574 pin eşlemesi
// Satırlar: P5, P0, P1, P3
// Sütunlar: P4, P6, P2
const uint8_t rowPins[4] = {5, 0, 1, 3};
const uint8_t colPins[3] = {4, 6, 2};

const char* mqtt_server = "83820ec0bcb54f2881a8900404db6e3c.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "esp32_user";
const char* mqtt_pass = "Admin1234!";

enum AppState {
  STATE_BOOT,
  STATE_MAIN,
  STATE_WAIT_PIN,
  STATE_CHECKING_SERVER,
  STATE_READY_VEND,
  STATE_CARD_DEPOSIT_AMT,
  STATE_CARD_DEPOSIT_SCAN,
  STATE_COIN_DEPOSIT,
  STATE_WAIT_CONFIG_PIN,
  STATE_AP_MODE
};

AppState currentState = STATE_BOOT;

String storedSSID = "";
String storedPASS = "";
String currentUID = "";
String enteredPin = "";
String depositAmountStr = "";
float vendingBalance = 0.0;

bool apMode = false;
bool relayActive = false;
unsigned long relayStartTime = 0;
int activeRelayPin = -1;

char stableKey = 0;
char lastPhysicalKey = 0;
unsigned long keyChangedAt = 0;
unsigned long keyPressedAt = 0;
bool longPressDone = false;
bool keyWasReported = false;

bool dualPressing = false;
unsigned long dualPressStart = 0;

volatile uint32_t coinPulseCount = 0;
uint32_t lastCoinPulseCountSeen = 0;
unsigned long coinLastChangeAt = 0;
bool coinWaiting = false;
float totalCredit = 0.0;

int spinnerAngle = 0;
unsigned long lastSpinnerUpdate = 0;

void clearArea(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, ST77XX_BLACK);
}

void showMessage(const String &msg, uint16_t color = ST77XX_CYAN) {
  clearArea(10, 280, 220, 30);
  u8g2Fonts.setForegroundColor(color);
  u8g2Fonts.setCursor(10, 305);
  u8g2Fonts.print(msg);
}

void updateSpinner(int cx, int cy) {
  if (millis() - lastSpinnerUpdate > 60) {
    lastSpinnerUpdate = millis();

    int oldX = cx + cos((spinnerAngle - 45) * PI / 180.0) * 30;
    int oldY = cy + sin((spinnerAngle - 45) * PI / 180.0) * 30;
    tft.fillCircle(oldX, oldY, 5, ST77XX_BLACK);

    int newX = cx + cos(spinnerAngle * PI / 180.0) * 30;
    int newY = cy + sin(spinnerAngle * PI / 180.0) * 30;
    tft.fillCircle(newX, newY, 5, ST77XX_GREEN);

    spinnerAngle = (spinnerAngle + 45) % 360;
  }
}

void drawMainScreen() {
  currentState = STATE_MAIN;
  tft.fillScreen(ST77XX_BLACK);

  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setCursor(10, 25); u8g2Fonts.print("--- მთავარი მენიუ ---");
  u8g2Fonts.setCursor(10, 65); u8g2Fonts.print("ბარათის დასკანერება");
  u8g2Fonts.setCursor(10, 105); u8g2Fonts.print("[*] 3წმ = მონეტის რეჟიმი");
  u8g2Fonts.setCursor(10, 145); u8g2Fonts.print("[#] 3წმ = ბარათის რეჟიმი");
  showMessage("მზად არის");
}

void showPinScreen() {
  tft.fillScreen(ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setCursor(10, 35); u8g2Fonts.print("შეიყვანეთ PIN:");
  tft.fillRect(10, 60, 220, 35, ST77XX_BLACK);
}

void showConfigPinScreen() {
  tft.fillScreen(ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
  u8g2Fonts.setCursor(10, 35); u8g2Fonts.print("WIFI CONFIG PIN:");
  tft.fillRect(10, 60, 220, 35, ST77XX_BLACK);
}

void showLoadingScreen(const String &title) {
  tft.fillScreen(ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
  u8g2Fonts.setCursor(20, 55);
  u8g2Fonts.print(title);
}

void showVendReadyScreen(float bal) {
  tft.fillScreen(ST77XX_BLACK);

  u8g2Fonts.setForegroundColor(ST77XX_GREEN);
  u8g2Fonts.setCursor(10, 35); u8g2Fonts.print("წვდომა დადასტურებულია");
  u8g2Fonts.setCursor(10, 75); u8g2Fonts.print(String("ბალანსი: ") + String(bal) + " GEL");

  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setCursor(10, 135); u8g2Fonts.print("დააჭირეთ ღილაკს");
  u8g2Fonts.setCursor(10, 165); u8g2Fonts.print("2 GEL ჩამოსაჭრელად.");
  u8g2Fonts.setCursor(10, 215); u8g2Fonts.print("გაუქმება: *");
}

void pcfWrite(uint8_t data) {
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(data);
  Wire.endTransmission();
}

uint8_t pcfRead() {
  Wire.requestFrom((uint8_t)PCF_ADDR, (uint8_t)1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

char scanKeypadRaw() {
  for (uint8_t c = 0; c < 3; c++) {
    uint8_t out = 0xFF;
    out &= ~(1 << colPins[c]);
    pcfWrite(out);
    delayMicroseconds(100);

    uint8_t in = pcfRead();

    for (uint8_t r = 0; r < 4; r++) {
      if (!(in & (1 << rowPins[r]))) {
        pcfWrite(0xFF);
        return keyMap[r][c];
      }
    }
  }

  pcfWrite(0xFF);
  return 0;
}

bool checkDualPress() {
  bool starP = false;
  bool hashP = false;

  uint8_t out0 = 0xFF & ~(1 << colPins[0]);
  pcfWrite(out0);
  delayMicroseconds(100);
  if (!(pcfRead() & (1 << rowPins[3]))) starP = true;

  uint8_t out2 = 0xFF & ~(1 << colPins[2]);
  pcfWrite(out2);
  delayMicroseconds(100);
  if (!(pcfRead() & (1 << rowPins[3]))) hashP = true;

  pcfWrite(0xFF);
  return (starP && hashP);
}

void startAPPortal() {
  if (apMode) return;

  currentState = STATE_AP_MODE;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32-SETUP");
  apMode = true;

  server.on("/", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String page = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"></head><body>";
    page += "<h2>WiFi Ayarları</h2><form action=\"/save\" method=\"POST\">";
    page += "<label>Ağ Seçin:</label><br><select name=\"ssid\">";
    
    if (n == 0) {
      page += "<option value=\"\">ქსელები ვერ მოიძებნა</option>";
    } else {
      for (int i = 0; i < n; ++i) {
        page += "<option value=\"" + WiFi.SSID(i) + "\">" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
    }
    
    page += "</select><br><br>";
    page += "<label>Manual SSID (gizli ağ için):</label><br>";
    page += "<input type=\"text\" name=\"manual_ssid\" placeholder=\"SSID\"><br><br>";
    page += "<label>Parola:</label><br>";
    page += "<input type=\"password\" name=\"pass\" placeholder=\"Şifre\"><br><br>";
    page += "<button type=\"submit\">შენახვა და გადატვირთვა (Kaydet)</button>";
    page += "</form></body></html>";

    server.send(200, "text/html; charset=utf-8", page);
  });

  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    if (server.arg("manual_ssid").length() > 0) {
      newSSID = server.arg("manual_ssid");
    }

    prefs.begin("net", false);
    prefs.putString("ssid", newSSID);
    prefs.putString("pass", server.arg("pass"));
    prefs.end();

    server.send(200, "text/html; charset=utf-8", "წარმატებით შეინახა! ESP32 გადაიტვირთება...");
    delay(1500);
    ESP.restart();
  });

  server.begin();

  tft.fillScreen(ST77XX_BLACK);
  u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
  u8g2Fonts.setCursor(10, 35); u8g2Fonts.print("WIFI პარამეტრები");
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setCursor(10, 75); u8g2Fonts.print("დაუკავშირდით:");
  u8g2Fonts.setCursor(10, 105); u8g2Fonts.setForegroundColor(ST77XX_CYAN);
  u8g2Fonts.print("ESP32-SETUP");
  u8g2Fonts.setForegroundColor(ST77XX_WHITE);
  u8g2Fonts.setCursor(10, 155); u8g2Fonts.print("IP: 192.168.4.1");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) return;

  if (currentState == STATE_CHECKING_SERVER) {
    if (doc["success"]) {
      vendingBalance = doc["balance"] | 0.0;
      String role = doc["role"] | "user";

      if (role == "admin") {
        tft.fillScreen(ST77XX_BLACK);
        u8g2Fonts.setForegroundColor(ST77XX_GREEN);
        u8g2Fonts.setCursor(10, 65); u8g2Fonts.print("ADMIN ACCESS");

        digitalWrite(OUT_PIN, LOW);
        delay(100);
        digitalWrite(OUT_PIN, HIGH);

        delay(1000);

        digitalWrite(OUT_PIN2, LOW);
        delay(100);
        digitalWrite(OUT_PIN2, HIGH);

        drawMainScreen();
      } else {
        currentState = STATE_READY_VEND;
        showVendReadyScreen(vendingBalance);
      }
    } else {
      tft.fillScreen(ST77XX_BLACK);
      u8g2Fonts.setForegroundColor(ST77XX_RED);
      u8g2Fonts.setCursor(10, 65);  u8g2Fonts.print("შეცდომა:");
      u8g2Fonts.setCursor(10, 105); u8g2Fonts.print(String(doc["message"] | "Bilinmeyen hata"));
      delay(3000);
      drawMainScreen();
    }
  }
}

void connectWiFiAndMQTT() {
  Serial.println("WiFi basliyor");
  Serial.print("SSID=");
  Serial.println(storedSSID);

  showLoadingScreen("WiFi კავშირი...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID.c_str(), storedPASS.c_str());

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    updateSpinner(120, 160);
    delay(10);

    if (millis() - startAttempt > 30000) {
      Serial.println("WiFi timeout");
      currentState = STATE_WAIT_CONFIG_PIN;
      enteredPin = "";
      showConfigPinScreen();
      return;
    }
  }

  Serial.println("WiFi baglandi");
  Serial.print("IP=");
  Serial.println(WiFi.localIP());

  showLoadingScreen("სერვერის კავშირი...");
  secureClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  unsigned long mqttStart = millis();
  while (!mqttClient.connected()) {
    updateSpinner(120, 160);

    if (mqttClient.connect("ESP32_Device", mqtt_user, mqtt_pass)) {
      mqttClient.subscribe("rfid/response");
      Serial.println("MQTT baglandi");
      break;
    } else {
      delay(500);
    }

    if (millis() - mqttStart > 20000) {
      Serial.println("MQTT timeout");
      showLoadingScreen("MQTT hata");
      delay(1500);
      currentState = STATE_MAIN;
      return;
    }
  }
}

void IRAM_ATTR coinPulseISR() {
  static unsigned long lastInterrupt = 0;
  unsigned long now = millis();
  
  if (now - lastInterrupt > 45) {
    coinPulseCount++;
    lastInterrupt = now;
  }
}

void setup() {
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  pcfWrite(0xFF);

  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, HIGH);

  pinMode(OUT_PIN2, OUTPUT);
  digitalWrite(OUT_PIN2, HIGH);

  pinMode(IN_PIN, INPUT_PULLUP);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN);
  mfrc522.PCD_Init();

  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulseISR, FALLING);

  tft.init(240, 320);
  tft.setRotation(0);

  u8g2Fonts.begin(tft);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setFont(my_georgian_font);
  u8g2Fonts.setBackgroundColor(ST77XX_BLACK);

  prefs.begin("net", true);
  storedSSID = prefs.getString("ssid", "");
  storedPASS = prefs.getString("pass", "");
  prefs.end();

  connectWiFiAndMQTT();

  if (currentState != STATE_WAIT_CONFIG_PIN) {
    drawMainScreen();
  }
}

void loop() {
  if (apMode) {
    server.handleClient();
    return;
  }

  mqttClient.loop();

  if (relayActive && (millis() - relayStartTime >= 100)) {
    digitalWrite(OUT_PIN, HIGH);
    digitalWrite(OUT_PIN2, HIGH);
    relayActive = false;
    drawMainScreen();
  }

  if (currentState == STATE_CHECKING_SERVER) {
    updateSpinner(120, 160);
  }

  if (currentState == STATE_MAIN || currentState == STATE_WAIT_PIN) {
    if (checkDualPress()) {
      if (!dualPressing) {
        dualPressing = true;
        dualPressStart = millis();
      } else if (millis() - dualPressStart >= 5000) {
        currentState = STATE_WAIT_CONFIG_PIN;
        enteredPin = "";
        dualPressing = false;
        showConfigPinScreen();
      }
      return;
    } else {
      dualPressing = false;
    }
  }

  if (currentState == STATE_READY_VEND && !relayActive) {
    uint8_t pcfIn = pcfRead();

    if (!(pcfIn & (1 << 7)) || (digitalRead(IN_PIN) == LOW)) {
      StaticJsonDocument<256> doc;
      doc["action"] = "deduct";
      doc["uid"] = currentUID;

      char buffer[256];
      serializeJson(doc, buffer);
      mqttClient.publish("rfid/request", buffer);

      activeRelayPin = (!(pcfIn & (1 << 7))) ? OUT_PIN : OUT_PIN2;
      digitalWrite(activeRelayPin, LOW);
      relayStartTime = millis();
      relayActive = true;

      showMessage("მიმდინარეობს...", ST77XX_GREEN);
    }
  }

  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uid = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
      uid += String(mfrc522.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();

    if (currentState == STATE_MAIN) {
      currentUID = uid;
      enteredPin = "";
      currentState = STATE_WAIT_PIN;
      showPinScreen();
    } else if (currentState == STATE_CARD_DEPOSIT_SCAN) {
      StaticJsonDocument<256> doc;
      doc["action"] = "card_deposit";
      doc["uid"] = uid;
      doc["amount"] = depositAmountStr.toInt();

      char buffer[256];
      serializeJson(doc, buffer);
      mqttClient.publish("rfid/request", buffer);

      tft.fillScreen(ST77XX_BLACK);
      u8g2Fonts.setForegroundColor(ST77XX_GREEN);
      u8g2Fonts.setCursor(10, 115);
      u8g2Fonts.print("დეპოზიტი გაგზავნილია!");
      delay(2000);
      drawMainScreen();
    }
  }

  char rawKey = scanKeypadRaw();

  if (rawKey != lastPhysicalKey) {
    lastPhysicalKey = rawKey;
    keyChangedAt = millis();
  }

  char debouncedKey = stableKey;
  if (millis() - keyChangedAt > 40) {
    debouncedKey = lastPhysicalKey;
  }

  if (debouncedKey != stableKey) {
    stableKey = debouncedKey;
    keyWasReported = false;
    longPressDone = false;
    if (stableKey != 0) keyPressedAt = millis();
  }

  if ((stableKey == '*' || stableKey == '#') && currentState == STATE_MAIN) {
    if (!longPressDone && (millis() - keyPressedAt >= 3000)) {
      longPressDone = true;

      if (stableKey == '#') {
        currentState = STATE_CARD_DEPOSIT_AMT;
        depositAmountStr = "";

        tft.fillScreen(ST77XX_BLACK);
        u8g2Fonts.setForegroundColor(ST77XX_CYAN);
        u8g2Fonts.setCursor(10, 35);
        u8g2Fonts.print("ბარათის დეპოზიტი");
        u8g2Fonts.setForegroundColor(ST77XX_WHITE);
        u8g2Fonts.setCursor(10, 75);
        u8g2Fonts.print("შეიყვანეთ თანხა:");
      } else if (stableKey == '*') {
        currentState = STATE_COIN_DEPOSIT;
        totalCredit = 0.0;

        tft.fillScreen(ST77XX_BLACK);
        u8g2Fonts.setForegroundColor(ST77XX_MAGENTA);
        u8g2Fonts.setCursor(10, 35);
        u8g2Fonts.print("მონეტის დეპოზიტი");
        u8g2Fonts.setForegroundColor(ST77XX_WHITE);
        u8g2Fonts.setCursor(10, 75);
        u8g2Fonts.print("ჩააგდეთ მონეტა...");
        u8g2Fonts.setCursor(10, 215);
        u8g2Fonts.print("დასრულება: #");
      }
    }
  }

  if (stableKey != 0 && !keyWasReported && !longPressDone) {
    keyWasReported = true;

    if (currentState == STATE_WAIT_PIN) {
      if (stableKey == '*') {
        drawMainScreen();
      } else if (stableKey == '#') {
        currentState = STATE_CHECKING_SERVER;
        showLoadingScreen("მოწმდება PIN...");

        StaticJsonDocument<256> doc;
        doc["action"] = "auth";
        doc["uid"] = currentUID;
        doc["pin"] = enteredPin;

        char buffer[256];
        serializeJson(doc, buffer);
        mqttClient.publish("rfid/request", buffer);
      } else {
        if (enteredPin.length() < 6) enteredPin += stableKey;
        clearArea(10, 60, 220, 35);
        u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
        u8g2Fonts.setCursor(10, 85);

        String stars = "";
        for (int i = 0; i < enteredPin.length(); i++) stars += "*";
        u8g2Fonts.print(stars);
      }
    }
    else if (currentState == STATE_WAIT_CONFIG_PIN) {
      if (stableKey == '*') {
        ESP.restart();
      } else if (stableKey == '#') {
        if (enteredPin == "1981") {
          startAPPortal();
        } else {
          tft.fillScreen(ST77XX_BLACK);
          u8g2Fonts.setForegroundColor(ST77XX_RED);
          u8g2Fonts.setCursor(10, 65);
          u8g2Fonts.print("არასწორი PIN!");
          delay(2000);
          ESP.restart();
        }
      } else {
        if (enteredPin.length() < 4) enteredPin += stableKey;
        clearArea(10, 60, 220, 35);
        u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
        u8g2Fonts.setCursor(10, 85);

        String stars = "";
        for (int i = 0; i < enteredPin.length(); i++) stars += "*";
        u8g2Fonts.print(stars);
      }
    }
    else if (currentState == STATE_CARD_DEPOSIT_AMT) {
      if (stableKey >= '0' && stableKey <= '9') {
        depositAmountStr += stableKey;
        clearArea(10, 100, 220, 30);
        u8g2Fonts.setCursor(10, 120);
        u8g2Fonts.print(depositAmountStr);
      } else if (stableKey == '#') {
        currentState = STATE_CARD_DEPOSIT_SCAN;
        u8g2Fonts.setForegroundColor(ST77XX_YELLOW);
        u8g2Fonts.setCursor(10, 165);
        u8g2Fonts.print("დაასკანერეთ ბარათი");
      } else if (stableKey == '*') {
        drawMainScreen();
      }
    }
    else if (currentState == STATE_COIN_DEPOSIT) {
      if (stableKey == '#') {
        StaticJsonDocument<256> doc;
        doc["action"] = "coin_deposit";
        doc["amount"] = totalCredit;

        char buffer[256];
        serializeJson(doc, buffer);
        mqttClient.publish("rfid/request", buffer);

        tft.fillScreen(ST77XX_BLACK);
        u8g2Fonts.setForegroundColor(ST77XX_GREEN);
        u8g2Fonts.setCursor(10, 115);
        u8g2Fonts.print("დეპოზიტი გაგზავნილია!");
        delay(2000);
        drawMainScreen();
      } else if (stableKey == '*') {
        drawMainScreen();
      }
    }
    else if (currentState == STATE_READY_VEND && stableKey == '*') {
      drawMainScreen();
    }
  }

  if (stableKey == 0) {
    keyWasReported = false;
  }

  if (currentState == STATE_COIN_DEPOSIT) {
    noInterrupts();
    uint32_t currentCount = coinPulseCount;
    interrupts();

    if (currentCount != lastCoinPulseCountSeen) {
      lastCoinPulseCountSeen = currentCount;
      coinLastChangeAt = millis();
      coinWaiting = true;
    }

    if (coinWaiting && currentCount > 0 && (millis() - coinLastChangeAt > COIN_GAP_MS)) {
      noInterrupts();
      uint32_t pulses = coinPulseCount;
      coinPulseCount = 0;
      interrupts();

      lastCoinPulseCountSeen = 0;
      coinWaiting = false;

      float value = 0;
      if (pulses == 2) value = 0.5;
      else if (pulses == 3) value = 1.0; // 
      else if (pulses == 4) value = 2.0;

      totalCredit += value;

      clearArea(10, 100, 220, 40);
      u8g2Fonts.setForegroundColor(ST77XX_GREEN);
      u8g2Fonts.setCursor(10, 125);
      u8g2Fonts.print(String("შემოსულია: ") + String(totalCredit) + " GEL");
    }
  }
}
