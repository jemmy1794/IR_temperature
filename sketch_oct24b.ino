// -----------------------------------------------------------
// LU90614 + Wi-Fi + MQTT example (ESP32, Serial2 RX16/TX17)
// Simplified and comments translated to English
// -----------------------------------------------------------
#include <LittleFS.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <time.h>

#define CONFIG_PATH "/config.json"

// Default configuration (used when config file is missing or invalid)
// Default SSID and password are empty so device quickly falls back to AP config mode.
const char* DEFAULT_WIFI_SSID = "";
const char* DEFAULT_WIFI_PASSWORD = "";
const char* DEFAULT_MQTT_BROKER = "192.168.4.100";

const int MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "ESP32_LU90614";
const char* MQTT_TOPIC_BASE = "yofa-temp/";
String GATEWAY_ID = "";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Configuration struct
struct Config {
  char ssid[33];
  char password[65];
  char mqtt_ip[16];
};
Config currentConfig;

// LU90614 UART settings
HardwareSerial lu90614_Serial(2);
const int RX_PIN = 16;
const int TX_PIN = 17;
const long BAUD_RATE = 9600;

// LU90614 commands & packet
const byte TEMP_MODE_HEADER = 0xFA;
const byte BODY_MODE_P2 = 0xC5;
const byte BODY_MODE_P3 = 0xBF;
const byte MATERIAL_MODE_P2 = 0xC6;
const byte MATERIAL_MODE_P3 = 0xC0;
const byte START_MEASUREMENT = 0xFA;
const byte START_MEASUREMENT_CA = 0xCA;
const byte START_MEASUREMENT_C4 = 0xC4;
const byte HEADER_RETURN = 0xFE;
const byte INSTRUCTION_BODY = 0xAC;
const byte INSTRUCTION_MATERIAL = 0xAA;
const int PACKET_SIZE = 9;
const float MAX_VALID_TEMP = 150.0;

enum MeasurementMode { BodyMode, MaterialMode };
#define STATIC_MODE_SELECTION BodyMode

// NTP
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

// Fixed BLE-like data block used in MQTT payload
const byte FIXED_BLE_DATA_BYTES[31] = {
  0x02,0x01,0x06,0x03,0x02,0x50,0xC0,0x17,0xFF,0x50,0x54,0x13,
  // Serial number (bytes 13-23): "2025ABC0001"
  0x32,0x30,0x32,0x35,0x41,0x42,0x43,0x30,0x30,0x30,0x31,
  0xFF,0xFF,0xFF,
  0x0F,0xDB, // battery placeholder
  0x0A,0xAC, // temp placeholder
  0x1B
};
const int BLE_DATA_SIZE = 31;
const float MOCK_VOLTAGE = 4.000;

// Function prototypes
void connectWiFi();
void connectMQTT();
void setAndMeasureTemperature(MeasurementMode mode);
void readAndProcessTemperature(MeasurementMode requestedMode);
bool verifyChecksum(const byte* packet, int size);
void initLocalTime();
long long getEpochMillis();
String bytesToHexString(const byte* buffer, int length);
void publish_structured_json(float temp, MeasurementMode requestedMode);

// Web server for configuration
WebServer server(80);
void initFileSystem();
void saveConfiguration();
void loadConfiguration();
void handleRoot();
void handleSave();
void startAPConfigPortal();

// -------- File system and config management --------
void initFileSystem() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS init failed");
  } else {
    Serial.println("LittleFS mounted");
  }
}

void saveConfiguration() {
  StaticJsonDocument<256> doc;
  doc["ssid"] = currentConfig.ssid;
  doc["password"] = currentConfig.password;
  doc["mqtt_ip"] = currentConfig.mqtt_ip;

  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println("Cannot open config file for writing");
    return;
  }
  if (serializeJson(doc, file) == 0) {
    Serial.println("Failed to write config");
  } else {
    Serial.println("Config saved");
  }
  file.close();
}

void loadConfiguration() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    // Use defaults (empty) so device will enter AP config
    strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
    strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
    strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
    Serial.println("Config file not found, using defaults");
    return;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println("Cannot open config file, using defaults");
    strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
    strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
    strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.printf("JSON parse failed: %s\n", error.c_str());
    strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
    strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
    strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
    return;
  }

  strlcpy(currentConfig.ssid, doc["ssid"] | "", sizeof(currentConfig.ssid));
  strlcpy(currentConfig.password, doc["password"] | "", sizeof(currentConfig.password));
  strlcpy(currentConfig.mqtt_ip, doc["mqtt_ip"] | DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));

  Serial.println("Config loaded");
  Serial.printf("SSID: %s\n", currentConfig.ssid);
  Serial.printf("MQTT IP: %s\n", currentConfig.mqtt_ip);
}

// -------- Simple web config portal --------
void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain", "index.html not found");
    return;
  }
  String html = file.readString();
  file.close();
  html.replace("%SSID%", currentConfig.ssid);
  html.replace("%MQTT%", currentConfig.mqtt_ip);
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("mqtt")) {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("password");
    String newMQTT = server.arg("mqtt");

    strlcpy(currentConfig.ssid, newSSID.c_str(), sizeof(currentConfig.ssid));
    strlcpy(currentConfig.password, newPass.c_str(), sizeof(currentConfig.password));
    strlcpy(currentConfig.mqtt_ip, newMQTT.c_str(), sizeof(currentConfig.mqtt_ip));

    saveConfiguration();

    File file = LittleFS.open("/success.html", "r");
    if (!file) {
      server.send(200, "text/plain", "Configuration saved. Restarting...");
      delay(1500);
      ESP.restart();
      return;
    }
    String html = file.readString();
    file.close();
    html.replace("%SSID%", newSSID);
    html.replace("%MQTT%", newMQTT);
    server.send(200, "text/html", html);
    delay(1500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing parameters");
  }
}

void startAPConfigPortal() {
  Serial.println("Starting AP config portal");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_ConfigPortal", "00000000");
  Serial.println("Connect to SSID: ESP32_ConfigPortal, PW: 00000000");
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  while (true) {
    server.handleClient();
    delay(10);
  }
}

// -------- Setup & loop --------
void setup() {
  Serial.begin(115200);
  Serial.println("Starting");

  initFileSystem();
  loadConfiguration();

  lu90614_Serial.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);

  connectWiFi();
  initLocalTime();

  // Build gateway ID from MAC
  byte mac[6];
  WiFi.macAddress(mac);
  GATEWAY_ID = "";
  for (int i = 0; i < 6; i++) {
    if (mac[i] < 16) GATEWAY_ID += "0";
    GATEWAY_ID += String(mac[i], HEX);
  }
  GATEWAY_ID.toUpperCase();

  mqttClient.setServer(currentConfig.mqtt_ip, MQTT_PORT);
  connectMQTT();

  Serial.println("Ready");
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  setAndMeasureTemperature(STATIC_MODE_SELECTION);
  delay(1000);
}

// -------- Wi-Fi and MQTT --------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  Serial.printf("Connecting to Wi-Fi: %s\n", currentConfig.ssid);
  WiFi.begin(currentConfig.ssid, currentConfig.password);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++retry > 30) { // ~15 seconds
      Serial.println("\nWi-Fi failed, opening AP config portal");
      startAPConfigPortal(); // blocks
    }
  }
  Serial.println("\nWi-Fi connected");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  int retryCount = 0;
  const int maxRetries = 3;
  while (!mqttClient.connected() && retryCount < maxRetries) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected");
      return;
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(", retrying...");
      retryCount++;
      delay(2000);
    }
  }
  if (!mqttClient.connected()) {
    Serial.println("MQTT connect failed, starting AP config");
    startAPConfigPortal();
  }
}

// -------- Time utilities --------
void initLocalTime() {
  Serial.println("Setting up time");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 3000)) {
    Serial.println("Failed to get NTP time");
  } else {
    Serial.printf("Time synced: %04d-%02d-%02d %02d:%02d:%02d\n",
      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
}

long long getEpochMillis() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

String bytesToHexString(const byte* buffer, int length) {
  String result;
  result.reserve(length * 2 + 1);
  for (int i = 0; i < length; i++) {
    char hex[3];
    sprintf(hex, "%02X", buffer[i]);
    result += hex;
  }
  return result;
}

// -------- LU90614: set mode, measure, parse and publish --------
void setAndMeasureTemperature(MeasurementMode mode) {
  byte p2 = (mode == BodyMode) ? BODY_MODE_P2 : MATERIAL_MODE_P2;
  byte p3 = (mode == BodyMode) ? BODY_MODE_P3 : MATERIAL_MODE_P3;

  lu90614_Serial.write(TEMP_MODE_HEADER);
  lu90614_Serial.write(p2);
  lu90614_Serial.write(p3);
  delay(50);
  lu90614_Serial.flush();

  lu90614_Serial.write(START_MEASUREMENT);
  lu90614_Serial.write(START_MEASUREMENT_CA);
  lu90614_Serial.write(START_MEASUREMENT_C4);

  unsigned long startTime = millis();
  const unsigned long TIMEOUT = 800;
  while (millis() - startTime < TIMEOUT) {
    if (lu90614_Serial.available() >= PACKET_SIZE) {
      readAndProcessTemperature(mode);
      return;
    }
  }
  Serial.println("Measurement timeout");
}

bool verifyChecksum(const byte* packet, int size) {
  byte receivedChecksum = packet[size - 1];
  uint16_t sum = 0;
  for (int i = 0; i < size - 1; i++) sum += packet[i];
  byte calculated = (byte)(sum & 0xFF);
  return calculated == receivedChecksum;
}

void publish_structured_json(float temp, MeasurementMode requestedMode) {
  String full_topic = String(MQTT_TOPIC_BASE) + GATEWAY_ID;
  int rssi = WiFi.RSSI();

  byte dynamicBleData[BLE_DATA_SIZE];
  memcpy(dynamicBleData, FIXED_BLE_DATA_BYTES, BLE_DATA_SIZE);

  long voltageDec = (long)(MOCK_VOLTAGE * 1000.0);
  dynamicBleData[26] = (byte)((voltageDec >> 8) & 0xFF);
  dynamicBleData[27] = (byte)(voltageDec & 0xFF);

  long tempDec = (long)(temp * 100.0);
  dynamicBleData[28] = (byte)((tempDec >> 8) & 0xFF);
  dynamicBleData[29] = (byte)(tempDec & 0xFF);

  String payload_raw_data = bytesToHexString(dynamicBleData, BLE_DATA_SIZE);

  String payload_str = "$GPRP," + GATEWAY_ID + "," + GATEWAY_ID + "," + String(rssi) + "," + payload_raw_data + "\r\n";

  DynamicJsonDocument doc(512);
  doc["format"] = "string";
  doc["topic"] = full_topic;
  doc["timestamp"] = getEpochMillis();
  doc["payload"] = payload_str;
  doc["qos"] = 0;

  String out;
  serializeJson(doc, out);

  Serial.printf("Serial Number: 2025ABC0001\n");
  Serial.printf("Voltage bytes: 0x%02X%02X (%.3fV)\n", dynamicBleData[26], dynamicBleData[27], MOCK_VOLTAGE);
  Serial.printf("Temp bytes: 0x%02X%02X\n", dynamicBleData[28], dynamicBleData[29]);
  Serial.print("Publishing to: ");
  Serial.println(full_topic);
  Serial.println(out);

  if (mqttClient.publish(full_topic.c_str(), out.c_str())) {
    Serial.println("Publish success");
  } else {
    Serial.println("Publish failed");
  }
}

void readAndProcessTemperature(MeasurementMode requestedMode) {
  // Skip until header byte
  while (lu90614_Serial.available() > 0 && lu90614_Serial.peek() != HEADER_RETURN) {
    lu90614_Serial.read();
  }

  if (lu90614_Serial.available() < PACKET_SIZE) return;

  byte packet[PACKET_SIZE];
  int bytesRead = lu90614_Serial.readBytes(packet, PACKET_SIZE);
  if (bytesRead != PACKET_SIZE) {
    Serial.println("Incorrect packet length");
    return;
  }

  if (!verifyChecksum(packet, PACKET_SIZE)) {
    Serial.println("Checksum failed");
    return;
  }

  byte instruction = packet[1];
  byte dataH = packet[2];
  byte dataL = packet[3];
  float temp = dataH + (float)dataL / 100.0;

  if (temp > MAX_VALID_TEMP) {
    Serial.printf("Invalid temperature (%.2f°C) > %.1f°C, discarding\n", temp, MAX_VALID_TEMP);
    return;
  }

  if ((requestedMode == BodyMode && instruction != INSTRUCTION_BODY) ||
      (requestedMode == MaterialMode && instruction != INSTRUCTION_MATERIAL)) {
    Serial.printf("Mode mismatch: expected 0x%X but got 0x%X\n",
      (requestedMode == BodyMode ? INSTRUCTION_BODY : INSTRUCTION_MATERIAL), instruction);
    // still publish the reading for visibility
  }

  Serial.printf("Temperature read: %.2f°C\n", temp);
  publish_structured_json(temp, requestedMode);
}
