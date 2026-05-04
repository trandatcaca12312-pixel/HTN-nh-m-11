#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include "time.h"

// --- Cấu hình Wi-Fi & MQTT ---
const char* ssid = "DESKTOP-T6EL1QE 8451";
const char* password = "7[89Z20z";
const char* mqtt_server = "broker.emqx.io";

// --- Cấu hình NTP ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

// --- Định nghĩa chân Pin  ---
#define SS_PIN 17          
#define RST_PIN 22
#define PIR_PIN 13
#define DHTPIN 21          
#define GAS_PIN 34
#define RELAY_DOOR 16      
#define LED_PIN 14
#define FAN_PIN 26         
#define BUZZER_PIN 25      
#define GAS_THRESHOLD 2000 

DHT dht(DHTPIN, DHT22);
MFRC522 mfrc522(SS_PIN, RST_PIN);
WiFiClient espClient;
PubSubClient client(espClient);

// --- Biến hệ thống ---
QueueHandle_t sensorQueue;
QueueHandle_t securityQueue;

// Thêm biến humi vào struct
struct SensorData { 
  float temp; 
  float humi; 
  int gas; 
};

bool isArmed = false; 
int schHour = -1, schMin = -1;
bool schActive = false;

// --- Xử lý lệnh từ MQTT ---
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String strTopic = String(topic);
  
  if (strTopic == "home/led/control") {
    digitalWrite(LED_PIN, (msg == "ON") ? HIGH : LOW);
  } 
  else if (strTopic == "home/fan/control") {
    digitalWrite(FAN_PIN, (msg == "ON") ? HIGH : LOW);
  }
  else if (strTopic == "home/security/mode") {
    isArmed = (msg == "ARM_ON"); 
    if (!isArmed) digitalWrite(BUZZER_PIN, LOW);
  }
  else if (strTopic == "home/buzzer/control") {
    if (msg == "OFF") digitalWrite(BUZZER_PIN, LOW); 
  }
  else if (strTopic == "home/timer/set") {
    schHour = msg.substring(0, 2).toInt();
    schMin = msg.substring(3, 5).toInt();
    schActive = true;
  }
}

// --- Task Mạng (Core 0) ---
void Task_Network(void *pvParameters) {
  SensorData receivedData;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
      vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
      if (!client.connected()) {
        String clientId = "ESP32-Master-" + String(random(0xffff), HEX);
        if (client.connect(clientId.c_str())) {
          client.subscribe("home/led/control");
          client.subscribe("home/fan/control");
          client.subscribe("home/timer/set");
          client.subscribe("home/security/mode");
          client.subscribe("home/buzzer/control");
          Serial.println("MQTT Connected");
        }
      }
      if (client.connected()) {
        client.loop();
        // Nhận dữ liệu từ Queue và gửi lên MQTT
        if (xQueueReceive(sensorQueue, &receivedData, 0) == pdPASS) {
          char t[8], h[8], g[8];
          dtostrf(receivedData.temp, 1, 2, t);
          dtostrf(receivedData.humi, 1, 2, h); // Chuyển đổi độ ẩm
          itoa(receivedData.gas, g, 10);
          
          client.publish("home/sensor/temp", t);
          client.publish("home/sensor/humi", h); // Publish độ ẩm lên topic riêng
          client.publish("home/sensor/gas", g);
        }
        
        char* securityMsg;
        if (xQueueReceive(securityQueue, &securityMsg, 0) == pdPASS) {
          if (String(securityMsg) == "TRIGGER_CAM") client.publish("home/camera/trigger", "CAPTURE");
          if (String(securityMsg) == "GAS_ALARM") client.publish("home/notification", "GAS_DANGER");
          if (String(securityMsg) == "INTRUDER") client.publish("home/notification", "INTRUDER_ALERT");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- Task Phần cứng (Core 1) ---
void Task_Hardware(void *pvParameters) {
  SensorData sData;
  unsigned long lastSensorRead = 0;
  struct tm timeinfo;

  for (;;) {
    // 1. Kiểm tra PIR
    if (digitalRead(PIR_PIN) == HIGH) {
      if (isArmed) {
        digitalWrite(BUZZER_PIN, HIGH);
        char* msg = "INTRUDER";
        xQueueSend(securityQueue, &msg, 0);
      }
      char* camMsg = "TRIGGER_CAM";
      xQueueSend(securityQueue, &camMsg, 0);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // 2. Kiểm tra RFID
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      digitalWrite(RELAY_DOOR, HIGH);
      digitalWrite(BUZZER_PIN, LOW); 
      vTaskDelay(pdMS_TO_TICKS(3000));
      digitalWrite(RELAY_DOOR, LOW);
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }

    // 3. Đọc cảm biến định kỳ 
    if (millis() - lastSensorRead > 10000) {
      float temp = dht.readTemperature();
      float humi = dht.readHumidity();
      int gas = analogRead(GAS_PIN);

      if (!isnan(temp) && !isnan(humi)) {
        sData.temp = temp;
        sData.humi = humi;
        sData.gas = gas;
        
        if (sData.gas > GAS_THRESHOLD) {
          digitalWrite(BUZZER_PIN, HIGH);
          char* gMsg = "GAS_ALARM";
          xQueueSend(securityQueue, &gMsg, 0);
        }
        xQueueSend(sensorQueue, &sData, 0);
      }
      lastSensorRead = millis();
    }

    // 4. Kiểm tra hẹn giờ
    if (schActive && getLocalTime(&timeinfo)) {
      if (timeinfo.tm_hour == schHour && timeinfo.tm_min == schMin) {
        digitalWrite(LED_PIN, LOW);
        digitalWrite(FAN_PIN, LOW);
        schActive = false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("System starting with Humidity Support...");

  pinMode(PIR_PIN, INPUT);
  pinMode(RELAY_DOOR, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(RELAY_DOOR, LOW);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  SPI.begin();
  mfrc522.PCD_Init();
  dht.begin();

  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  securityQueue = xQueueCreate(10, sizeof(char*));

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  xTaskCreatePinnedToCore(Task_Network, "Network", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_Hardware, "Hardware", 4096, NULL, 3, NULL, 1);
  
  Serial.println("System Ready!");
}

void loop() {}
