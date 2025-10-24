// -----------------------------------------------------------
// LU90614 + Wi-Fi + MQTT 整合範例 (ESP32, Serial2 RX16/TX17)
// -----------------------------------------------------------
// 功能:
// 1. 自動連線到 Wi-Fi (從 LittleFS 或 Web 配置載入)
// 2. Wi-Fi 連線失敗時，自動啟動 Web 配置門戶 (Config Portal) 供手機設定。
// 3. 自動連線到 MQTT Broker (從 LittleFS 或 Web 配置載入)
// 4. 定期測量 LU90614 體溫 / 物體溫度
// 5. 發佈到 MQTT topic: "sensor/temperature"
// 6. 發佈的訊息格式為結構化 JSON
// -----------------------------------------------------------
#include <LittleFS.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h> 
#include <ArduinoJson.h> 
#include <time.h>      

#define CONFIG_PATH "/config.json"

// ==========================================================
// 預設配置值 (用於首次啟動或配置檔缺失/損壞時)
// 🚨 預設 SSID 和 Password 設為空字串，以確保連線失敗並快速進入 Web 配置模式。
// ==========================================================
const char* DEFAULT_WIFI_SSID = ""; 
const char* DEFAULT_WIFI_PASSWORD = "";
const char* DEFAULT_MQTT_BROKER = "192.168.4.100"; // 預設的 MQTT 伺服器 IP

// ==========================================================
// MQTT Broker / Client 設定
// ==========================================================
const int MQTT_PORT = 1883;
const char* MQTT_CLIENT_ID = "ESP32_LU90614";
const char* MQTT_TOPIC_BASE = "yofa-temp/"; 
String GATEWAY_ID = ""; 

// MQTT Client 初始化
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ==========================================================
// 配置結構體與實例
// ==========================================================
struct Config {
    char ssid[33];      
    char password[65];  
    char mqtt_ip[16];   
};
Config currentConfig; // 全域配置實例

// ==========================================================
// LU90614 UART 設定
// ==========================================================
HardwareSerial lu90614_Serial(2);
const int RX_PIN = 16;
const int TX_PIN = 17;
const long BAUD_RATE = 9600;

// LU90614 指令集
const byte TEMP_MODE_HEADER = 0XFA;
const byte BODY_MODE_P2 = 0XC5; 
const byte BODY_MODE_P3 = 0XBF;
const byte MATERIAL_MODE_P2 = 0XC6; 
const byte MATERIAL_MODE_P3 = 0XC0;
const byte START_MEASUREMENT = 0XFA; 
const byte START_MEASUREMENT_CA = 0XCA;
const byte START_MEASUREMENT_C4 = 0XC4;
const byte HEADER_RETURN = 0XFE;
const byte INSTRUCTION_BODY = 0XAC;
const byte INSTRUCTION_MATERIAL = 0XAA;
const int PACKET_SIZE = 9;

const float MAX_VALID_TEMP = 150.0; 
enum MeasurementMode { BodyMode, MaterialMode };
#define STATIC_MODE_SELECTION BodyMode

// ==========================================================
// NTP 時間設定
// ==========================================================
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 8 * 3600; 
const int   DAYLIGHT_OFFSET_SEC = 0; 

// ==========================================================
// 固定的 31 位元組原始數據 (與 BLE 協議相關)
// ==========================================================
const byte FIXED_BLE_DATA_BYTES[31] = {
    0x02, 0x01, 0x06, 0x03, 0x02, 0x50, 0xC0, 0x17, 0xFF, 0x50, 0x54, 0x13, 
    // Byte 13-23 (Index 12-22): Serial Number (自創: "2025ABC0001")
    0x32, 0x30, 0x32, 0x35, 0x41, 0x42, 0x43, 0x30, 0x30, 0x30, 0x31,
    0xFF, 0xFF, 0xFF,                                                      
    0x0F, 0xDB,  // Bytes 27-28 (Battery Voltage - Placeholder) 
    0x0A, 0xAC,  // Bytes 29-30 (Temperature - Placeholder) 
    0x1B
};
const int BLE_DATA_SIZE = 31;
const float MOCK_VOLTAGE = 4.000; 

// ==========================================================
// 函數原型
// ==========================================================
void connectWiFi();
void connectMQTT();
void setAndMeasureTemperature(MeasurementMode mode);
void readAndProcessTemperature(MeasurementMode requestedMode);
bool verifyChecksum(const byte* packet, int size);
void initLocalTime();
long long getEpochMillis();
String bytesToHexString(const byte* buffer, int length); 
void publish_structured_json(float temp, MeasurementMode requestedMode, const byte* rawPacket, int rawSize);

// --- Web 配置相關函式 ---
WebServer server(80);
void initFileSystem();
void saveConfiguration();
void loadConfiguration();
void handleRoot();
void handleSave();
void startAPConfigPortal();

// ======================================================
// 檔案系統與設定管理
// ======================================================

/**
 * @brief 初始化 LittleFS
 */
void initFileSystem() {
    if (!LittleFS.begin(true)) {
        Serial.println("❌ LittleFS 初始化失敗！");
    } else {
        Serial.println("✅ LittleFS 已啟動。");
    }
}

/**
 * @brief 儲存設定到 LittleFS
 */
void saveConfiguration() {
    Serial.println("💾 正在儲存設定到 LittleFS...");
    
    StaticJsonDocument<256> doc;
    doc["ssid"] = currentConfig.ssid;
    doc["password"] = currentConfig.password;
    doc["mqtt_ip"] = currentConfig.mqtt_ip;

    File file = LittleFS.open(CONFIG_PATH, "w");
    if (!file) {
        Serial.println("❌ 無法開啟設定檔進行寫入！");
        return;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("❌ 設定寫入失敗！");
    } else {
        Serial.println("✅ 設定已成功寫入 LittleFS。");
    }

    file.close();
}


/**
 * @brief 從 LittleFS 載入設定
 */
void loadConfiguration() {
    if (!LittleFS.exists(CONFIG_PATH)) {
        Serial.println("⚠️ 未找到設定檔，使用預設空值。設備將自動進入 Web 配置模式。");
        // 🚨 安全原則：僅設定變數，不寫入檔案，避免檔案系統被不必要的寫入操作破壞。
        strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
        strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
        strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
        return; 
    }

    File file = LittleFS.open(CONFIG_PATH, "r");
    if (!file) {
        Serial.println("❌ 無法開啟設定檔！使用預設值。");
        // 🚨 安全原則：無法開啟時，使用預設值並退出，不執行寫入。
        strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
        strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
        strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
        return; 
    }

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error) {
        Serial.printf("❌ JSON 解析失敗: %s。使用預設空值並進入配置模式。\n", error.c_str());
        // 🚨 安全原則：如果解析失敗，使用預設值並退出，不寫入檔案，避免破壞原始配置。
        strlcpy(currentConfig.ssid, DEFAULT_WIFI_SSID, sizeof(currentConfig.ssid));
        strlcpy(currentConfig.password, DEFAULT_WIFI_PASSWORD, sizeof(currentConfig.password));
        strlcpy(currentConfig.mqtt_ip, DEFAULT_MQTT_BROKER, sizeof(currentConfig.mqtt_ip));
        return; 
    }

    // 將 JSON 內容載入結構
    strlcpy(currentConfig.ssid, doc["ssid"] | "", sizeof(currentConfig.ssid));
    strlcpy(currentConfig.password, doc["password"] | "", sizeof(currentConfig.password));
    strlcpy(currentConfig.mqtt_ip, doc["mqtt_ip"] | "", sizeof(currentConfig.mqtt_ip));

    Serial.println("✅ 成功從 LittleFS 載入設定：");
    Serial.printf("SSID: %s\n", currentConfig.ssid);
    Serial.printf("MQTT IP: %s\n", currentConfig.mqtt_ip);
}


// ==========================================================
// Web 配置門戶 (Config Portal) - 使用本地 Tailwind CSS
// ==========================================================

void handleRoot() {
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
        server.send(500, "text/plain", "⚠️ 找不到 index.html");
        return;
    }

    String html = file.readString();
    file.close();

    // 取代占位符（動態顯示目前設定）
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
            server.send(500, "text/plain", "⚠️ 找不到 success.html");
            return;
        }

        String html = file.readString();
        file.close();

        html.replace("%SSID%", newSSID);
        html.replace("%MQTT%", newMQTT);

        server.send(200, "text/html", html);
        delay(2000);
        ESP.restart();
    } else {
        server.send(400, "text/plain", "❌ 缺少必要的配置參數！");
    }
}

/**
 * @brief 啟動 AP 模式和 Web Server，進入配置門戶
 */
void startAPConfigPortal() {
    Serial.println("\n⚙️ Wi-Fi 連線失敗！啟動 Web 配置門戶模式 (AP Mode)");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP32_ConfigPortal", "00000000");
    
    Serial.println("請用手機連線 Wi-Fi 熱點:");
    Serial.println("  - SSID: ESP32_ConfigPortal");
    Serial.println("  - PASSWORD: 00000000");
    Serial.println("連線後，打開瀏覽器輸入: http://192.168.4.1");

    server.on("/", handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    Serial.println("WebServer 已啟動，等待配置...");

    while (true) {
        server.handleClient();
        delay(10);
    }
}

// ==========================================================
// Setup
// ==========================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\nLU90614 + MQTT 範例啟動中...");
    
    // 1. 初始化檔案系統並載入配置 (安全載入，失敗不儲存)
    initFileSystem(); 
    loadConfiguration(); // 載入配置到 currentConfig 

    // 2. 啟動 UART
    lu90614_Serial.begin(BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
    Serial.printf("Serial2 初始化完成 (RX=%d, TX=%d)\n", RX_PIN, TX_PIN);

    // 3. 連線 Wi-Fi (使用載入的配置值)
    connectWiFi();
    
    // 4. 初始化 NTP 時間服務 (在 Wi-Fi 連線成功後執行)
    initLocalTime();

    // 5. 獲取並設定 GATEWAY_ID (使用 MAC 地址)
    byte mac[6];
    WiFi.macAddress(mac);
    GATEWAY_ID = "";
    for(int i=0; i<6; i++){
        if (mac[i]<16) GATEWAY_ID += "0";
        GATEWAY_ID += String(mac[i], HEX);
    }
    GATEWAY_ID.toUpperCase();
    Serial.printf("Gateway ID (MAC): %s\n", GATEWAY_ID.c_str());

    // 6. 設定 MQTT (使用載入的配置 IP)
    mqttClient.setServer(currentConfig.mqtt_ip, MQTT_PORT);
    connectMQTT();

    Serial.println("系統初始化完成。");
    Serial.println("===============================");
}

// ==========================================================
// 主迴圈
// ==========================================================
void loop() {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    setAndMeasureTemperature(STATIC_MODE_SELECTION);
    delay(1000); 
}

// ==========================================================
// Wi-Fi 自動連線 (使用動態配置)
// ==========================================================
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    Serial.printf("連線至 Wi-Fi: %s\n", currentConfig.ssid);
    
    // 使用載入的配置值連線
    // 如果是空字串 (即無配置)，連線會迅速失敗，觸發 AP 模式。
    WiFi.begin(currentConfig.ssid, currentConfig.password);

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        // 連線超時（約 30 秒）後啟動 Web 配置門戶
        if (++retry > 30) { 
            Serial.println("\nWi-Fi 連線失敗，啟動 AP 模式供手機設定。");
            startAPConfigPortal(); // 阻塞，直到重啟
        }
    }
    Serial.println("\nWi-Fi 已連線！");
    Serial.print("IP 地址: ");
    Serial.println(WiFi.localIP());
}

// ==========================================================
// MQTT 自動連線 (使用動態配置)
// ==========================================================
void connectMQTT() {
    int retryCount = 0;
    const int maxRetries = 3;

    while (!mqttClient.connected() && retryCount < maxRetries) {
        Serial.print("Attempting MQTT connection...");
        
        // Attempt to connect
        if (mqttClient.connect(MQTT_CLIENT_ID)) {
            Serial.println("connected");
            return; // Exit the function if connected
        } else {
            Serial.print("failed, rc=");
            Serial.print(mqttClient.state());
            Serial.println(" try again in 3 seconds");
            retryCount++;
            delay(3000); // Wait 3 seconds before retrying
        }
    }

    // If all retries fail, start AP configuration
    if (!mqttClient.connected()) {
        Serial.println("Failed to connect to MQTT after 3 attempts. Starting AP configuration...");
        startAPConfigPortal();
    }
}

// ==========================================================
// NTP 時間同步
// ==========================================================
void initLocalTime() {
    Serial.println("正在設定系統時間...");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo, 5000)){
        Serial.println("無法取得時間，請檢查網路連線。時間戳將可能不準確。");
        return;
    }
    Serial.print("時間已同步: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
}

long long getEpochMillis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

String bytesToHexString(const byte* buffer, int length) {
    String result = "";
    result.reserve(length * 2 + 1); 
    for (int i = 0; i < length; i++) {
        char hex[3];
        sprintf(hex, "%02X", buffer[i]);
        result += hex;
    }
    return result;
}


// ==========================================================
// LU90614 - 模式設定與測量
// ==========================================================
void setAndMeasureTemperature(MeasurementMode mode) {
    byte p2, p3;
    const char* modeName;
    if (mode == BodyMode) {
        p2 = BODY_MODE_P2;
        p3 = BODY_MODE_P3;
        modeName = "體溫模式";
    } else {
        p2 = MATERIAL_MODE_P2;
        p3 = MATERIAL_MODE_P3;
        modeName = "物體模式";
    }

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
    Serial.printf("[警告] %s 測量超時\n", modeName);
}

// ==========================================================
// LU90614 - 檢查校驗和
// ==========================================================
bool verifyChecksum(const byte* packet, int size) {
    byte receivedChecksum = packet[size - 1]; 
    uint16_t sum = 0;
    for (int i = 0; i < size - 1; i++) sum += packet[i]; 
    
    byte calculatedChecksum = (byte)(sum & 0xFF);
    return (calculatedChecksum == receivedChecksum);
}

// ==========================================================
// LU90614 - 發佈 JSON 訊息
// ==========================================================
void publish_structured_json(float temp, MeasurementMode requestedMode, const byte* rawPacket, int rawSize) {
    String full_topic = String(MQTT_TOPIC_BASE) + GATEWAY_ID; 
    int rssi = WiFi.RSSI();
    
    byte dynamicBleData[BLE_DATA_SIZE];
    memcpy(dynamicBleData, FIXED_BLE_DATA_BYTES, BLE_DATA_SIZE);

    // --- 處理 Byte 27-28 (Battery Voltage) (Index 26-27) ---
    long voltageDec = (long)(MOCK_VOLTAGE * 1000.0);
    dynamicBleData[26] = (byte)((voltageDec >> 8) & 0xFF); 
    dynamicBleData[27] = (byte)(voltageDec & 0xFF);

    // --- 處理 Byte 29-30 (Temperature) (Index 28-29) ---
    long tempDec = (long)(temp * 100.0); 
    dynamicBleData[28] = (byte)((tempDec >> 8) & 0xFF); 
    dynamicBleData[29] = (byte)(tempDec & 0xFF);        
    
    String payload_raw_data = bytesToHexString(dynamicBleData, BLE_DATA_SIZE);

    // 4. 組裝 Payload
    String payload_str = "$GPRP";
    payload_str += ",";
    payload_str += GATEWAY_ID; 
    payload_str += ",";
    payload_str += GATEWAY_ID; 
    payload_str += ",";
    payload_str += String(rssi); 
    payload_str += ",";
    payload_str += payload_raw_data; 
    payload_str += "\r\n"; 

    // 5. 建立 JSON 文件
    const size_t capacity = JSON_OBJECT_SIZE(5) + payload_str.length() + 100;
    DynamicJsonDocument doc(capacity);
    
    doc["format"] = "string"; 
    doc["topic"] = full_topic; 
    doc["timestamp"] = getEpochMillis(); 
    doc["payload"] = payload_str; 
    doc["qos"] = 0; 

    char jsonBuffer[capacity]; 
    serializeJson(doc, jsonBuffer, capacity);

    Serial.printf("Serial Number (Bytes 13-23): 2025ABC0001\n");
    Serial.printf("Dynamic Voltage Bytes (27-28): 0x%02X%02X (%.3fV)\n", dynamicBleData[26], dynamicBleData[27], MOCK_VOLTAGE);
    Serial.printf("Dynamic Temp Bytes (29-30): 0x%02X%02X\n", dynamicBleData[28], dynamicBleData[29]);
    Serial.print("Publishing topic: ");
    Serial.println(full_topic);
    Serial.print("Message (JSON): ");
    Serial.println(jsonBuffer);

    if (mqttClient.publish(full_topic.c_str(), jsonBuffer)) {
        Serial.println("Publish success.");
    } else {
        Serial.println("Publish failed.");
    }
}


// ==========================================================
// LU90614 - 解析與送出 MQTT
// ==========================================================
void readAndProcessTemperature(MeasurementMode requestedMode) {
    while (lu90614_Serial.available() > 0 && lu90614_Serial.peek() != HEADER_RETURN) {
        lu90614_Serial.read(); 
    }
    
    if (lu90614_Serial.available() < PACKET_SIZE) {
        return;
    }
    
    byte packet[PACKET_SIZE];
    int bytesRead = lu90614_Serial.readBytes(packet, PACKET_SIZE);

    if (bytesRead != PACKET_SIZE) {
        Serial.println("資料長度錯誤");
        return;
    }

    if (!verifyChecksum(packet, PACKET_SIZE)) {
        Serial.println("校驗失敗, 丟棄資料");
        return;
    }

    byte instruction = packet[1];
    byte dataH = packet[2];
    byte dataL = packet[3];
    float temp = dataH + (float)dataL / 100.0;
    
    if (temp > MAX_VALID_TEMP) { 
        Serial.printf("[異常] 讀取到非物理範圍溫度 (%.2f°C)，超過上限 (%.1f°C)。\n", temp, MAX_VALID_TEMP);
        Serial.printf("Raw DataH: 0x%X, Raw DataL: 0x%X。已丟棄數據,不發佈。\n", dataH, dataL);
    } else {
        if ((requestedMode == BodyMode && instruction != INSTRUCTION_BODY) ||
            (requestedMode == MaterialMode && instruction != INSTRUCTION_MATERIAL)) {
            Serial.printf("[警告] 模式不符! 預期指令 0X%X,實際讀到 0X%X。\n", 
                         (requestedMode == BodyMode ? INSTRUCTION_BODY : INSTRUCTION_MATERIAL), instruction);
        }
        
        Serial.printf("成功讀取 UART 溫度: %.2f°C\n", temp);
        
        publish_structured_json(temp, requestedMode, packet, PACKET_SIZE);
    }
}
