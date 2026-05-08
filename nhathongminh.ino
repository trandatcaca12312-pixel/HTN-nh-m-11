#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <ESP32Servo.h> 
#include "time.h"

// --- Pin Definitions ---
#define SS_PIN 17          
#define RST_PIN 22
#define PIR_PIN 13
#define DHTPIN 21          
#define GAS_PIN 34
#define RELAY_DOOR 16      
#define SERVO_PIN 4      
#define LED_PIN 14
#define FAN_PIN 26         
#define BUZZER_PIN 25      

// --- Cấu hình ---
const char* ssid = "DESKTOP-T6EL1QE 8451";
const char* password = "7[89Z20z";
const char* mqtt_server = "broker.emqx.io";
const int GAS_THRESHOLD = 2000;

DHT dht(DHTPIN, DHT22);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo doorServo;
WiFiClient espClient;
PubSubClient client(espClient);

struct SensorData { float temp, humi; int gas; };
QueueHandle_t sensorQueue, securityQueue;

bool isArmed = false;
unsigned long doorTimer = 0;
bool isDoorOpen = false; 

// --- Hàm điều khiển Cửa ---
void controlDoor(bool open) {
  if (open) {
    if (!isDoorOpen) { 
      isDoorOpen = true;
      doorServo.attach(SERVO_PIN); 
      digitalWrite(RELAY_DOOR, HIGH);
      doorServo.write(90);        
      client.publish("home/door/status", "OPEN");
      doorTimer = millis();       
      Serial.println("Cửa mở...");
    }
  } else {
    if (isDoorOpen) { 
      isDoorOpen = false;
      digitalWrite(RELAY_DOOR, LOW);
      doorServo.write(0);         
      client.publish("home/door/status", "CLOSED");
      Serial.println("Cửa đóng...");
      
      vTaskDelay(pdMS_TO_TICKS(600)); 
      doorServo.detach(); 
    }
  }
}

// --- Xử lý lệnh từ MQTT ---
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  String strTopic = String(topic);

  if (strTopic == "home/door/control" && msg == "OPEN") {
    controlDoor(true);
  } else if (strTopic == "home/led/control") {
    digitalWrite(LED_PIN, (msg == "ON") ? HIGH : LOW);
  } else if (strTopic == "home/fan/control") {
    digitalWrite(FAN_PIN, (msg == "ON") ? HIGH : LOW);
  } else if (strTopic == "home/security/mode") {
    isArmed = (msg == "ARM_ON");
    if (!isArmed) digitalWrite(BUZZER_PIN, LOW);
  } else if (strTopic == "home/buzzer/control" && msg == "OFF") {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// --- Task Mạng (Core 0) ---
void Task_Network(void *pvParameters) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.begin(ssid, password);
      vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
      if (!client.connected()) {
        if (client.connect("ESP32_SmartHome_Final")) {
          client.subscribe("home/+/control");
          client.subscribe("home/security/mode");
        }
      }
      if (client.connected()) {
        client.loop();
        SensorData receivedData;
        if (xQueueReceive(sensorQueue, &receivedData, 0) == pdPASS) {
          client.publish("home/sensor/temp", String(receivedData.temp).c_str());
          client.publish("home/sensor/humi", String(receivedData.humi).c_str());
          client.publish("home/sensor/gas", String(receivedData.gas).c_str());
        }
        char* secMsg;
        if (xQueueReceive(securityQueue, &secMsg, 0) == pdPASS) {
          client.publish("home/notification", secMsg);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// --- Task Phần cứng (Core 1) ---
void Task_Hardware(void *pvParameters) {
  unsigned long lastRead = 0;
  for (;;) {
    // 1. Quét RFID
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      controlDoor(true);
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
    }

    // 2. Tự động đóng cửa sau 3 giây 
    if (isDoorOpen && (millis() - doorTimer > 3000)) {
      controlDoor(false);
      doorTimer = 0; 
    }

    // 3. PIR (Báo động đột nhập)
    if (digitalRead(PIR_PIN) == HIGH && isArmed) {
      digitalWrite(BUZZER_PIN, HIGH);
      char* m = "INTRUDER ALERT!";
      xQueueSend(securityQueue, &m, 0);
      vTaskDelay(pdMS_TO_TICKS(2000));
    }

    // 4. Đọc cảm biến Gas & DHT
    if (millis() - lastRead > 10000) {
      SensorData sData;
      sData.temp = dht.readTemperature();
      sData.humi = dht.readHumidity();
      sData.gas = analogRead(GAS_PIN);

      // --- PHẦN MỚI: Logic xử lý Gas tự động tắt còi ---
      if (sData.gas > GAS_THRESHOLD) {
        digitalWrite(BUZZER_PIN, HIGH);
        char* g = "GAS LEAKAGE!";
        xQueueSend(securityQueue, &g, 0);
      } else {
        // Tự động tắt còi nếu không còn gas và chế độ an ninh đang tắt
        if (!isArmed) {
          digitalWrite(BUZZER_PIN, LOW);
        }
      }
      

      xQueueSend(sensorQueue, &sData, 0);
      lastRead = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_DOOR, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);

  // Khởi tạo Servo ban đầu ở trạng thái đóng
  doorServo.setPeriodHertz(50); 
  doorServo.attach(SERVO_PIN);
  doorServo.write(0); 
  vTaskDelay(pdMS_TO_TICKS(500));
  doorServo.detach(); 

  SPI.begin();
  mfrc522.PCD_Init();
  dht.begin();

  sensorQueue = xQueueCreate(10, sizeof(SensorData));
  securityQueue = xQueueCreate(10, sizeof(char*));

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  xTaskCreatePinnedToCore(Task_Network, "Net", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_Hardware, "Hw", 4096, NULL, 3, NULL, 1);
  
  Serial.println("Hệ thống đã sẵn sàng!");
}

void loop() {
  // FreeRTOS xử lý mọi thứ trong Tasks, loop để trống.
}
