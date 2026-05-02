#include "esp_camera.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>

// ==========================================
// 1. THÔNG TIN CẤU HÌNH
// ==========================================
const char* ssid = "DESKTOP-T6EL1QE 8451";
const char* password = "7[89Z20z";
const char* mqtt_server = "broker.emqx.io";

// Các Topic MQTT
const char* topic_trigger = "home/camera/trigger";
const char* topic_photo   = "home/camera/photo";
const char* topic_ip      = "home/camera/ip"; // Topic báo cáo IP

WiFiClient espClient;
PubSubClient client(espClient);
WebServer server(80);

// Pinout AI Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ==========================================
// 2. CÁC HÀM XỬ LÝ CAMERA
// ==========================================
void captureAndSendPhoto() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) return;

  if (client.beginPublish(topic_photo, fb->len, false)) {
    client.write(fb->buf, fb->len);
    client.endPublish();
    Serial.println("Photo sent to MQTT!");
  }
  esp_camera_fb_return(fb);
}

void handleStream() {
  WiFiClient client_web = server.client();
  String response = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client_web.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) continue;
    server.sendContent("--frame\r\nContent-Type: image/jpeg\r\n\r\n");
    client_web.write(fb->buf, fb->len);
    server.sendContent("\r\n");
    esp_camera_fb_return(fb);
    delay(40);
    if (!server.client().connected()) break;
  }
}

// ==========================================
// 3. MQTT & KẾT NỐI
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) message += (char)payload[i];
  
  if (String(topic) == topic_trigger && message == "CAPTURE") {
    captureAndSendPhoto();
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    String clientId = "ESP32CAM-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
      client.subscribe(topic_trigger);
      
      // Gửi IP của ESP32 lên MQTT kèm chế độ RETAIN để Web luôn nhận được
      String ipAddr = WiFi.localIP().toString();
      client.publish(topic_ip, ipAddr.c_str(), true); 
    } else {
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  client.setBufferSize(20480); // Tăng buffer cho ảnh

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if(psramFound()){
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 14;
    config.fb_count = 1;
  }

  esp_camera_init(&config);
  
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  server.on("/", handleStream);
  server.begin();
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  server.handleClient();
}