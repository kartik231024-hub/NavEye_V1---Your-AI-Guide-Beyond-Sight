#include "esp_camera.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

const char* ssid       = "Jai_shree_ram_2.4G";
const char* password   = "ak@123456";
const char* serverIP   = "192.168.1.16";
const int   serverPort = 8081;

#define LED_PIN 4
#define LDR_PIN 13

WiFiClient client;
bool continuousMode       = false;
unsigned long lastCapture = 0;
int captureInterval       = 5000;
bool justConnected        = false;

bool readExact(uint8_t* buf, size_t len, uint32_t timeoutMs = 5000) {
    uint32_t start = millis();
    size_t got = 0;
    while (got < len) {
        if (millis() - start > timeoutMs) return false;
        int avail = client.available();
        if (avail > 0) {
            size_t chunk = min((size_t)avail, len - got);
            client.readBytes(buf + got, chunk);
            got += chunk;
        } else {
            delay(1);
        }
    }
    return true;
}

void sendMessage(const String& jsonStr) {
    if (!client.connected()) return;
    uint32_t len = jsonStr.length();
    uint8_t lb[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)(len)
    };
    client.write(lb, 4);
    client.print(jsonStr);
}

void connectToWiFi() {
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid, password);
    Serial.print("WiFi");
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500); Serial.print(".");
        if (++retry > 40) { Serial.println("WiFi failed"); return; }
    }
    Serial.println("\n" + WiFi.localIP().toString());
}

void connectToServer() {
    Serial.print("Server");
    int retry = 0;
    while (!client.connect(serverIP, serverPort)) {
        delay(1000); Serial.print(".");
        if (++retry >= 15) { Serial.println("\nFailed"); return; }
    }
    client.setNoDelay(true);
    client.setTimeout(30000);
    delay(500);
    justConnected = true;
    Serial.println("\nConnected");
}

void initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size   = FRAMESIZE_QVGA;
        config.jpeg_quality = 15;
        config.fb_count     = 1;
    } else {
        config.frame_size   = FRAMESIZE_QQVGA;
        config.jpeg_quality = 20;
        config.fb_count     = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) { Serial.printf("Camera init failed: 0x%x\n", err); return; }

    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_ae_level(s, 0);
    }
    Serial.println("Camera OK");
}

void initLED() {
    ledcSetup(1, 5000, 8);
    ledcAttachPin(LED_PIN, 1);
    ledcWrite(1, 0);
    pinMode(LDR_PIN, INPUT);
}

void handleLDR() {
    if (digitalRead(LDR_PIN) == HIGH) {
        ledcWrite(1, 128); // light detected → LED 50%
    } else {
        ledcWrite(1, 0);   // dark → LED off
    }
}

void captureAndSend() {
    if (!client.connected()) return;

    for (int i = 0; i < 2; i++) {
        camera_fb_t* tmp = esp_camera_fb_get();
        if (tmp) esp_camera_fb_return(tmp);
        delay(150);
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("Capture failed"); return; }

    uint32_t jpegSize = fb->len;
    Serial.printf("Cap: %u B\n", jpegSize);

    StaticJsonDocument<128> doc;
    doc["type"] = "image_start";
    doc["size"] = jpegSize;
    String jsonStr;
    serializeJson(doc, jsonStr);
    sendMessage(jsonStr);

    uint8_t ack[4];
    if (!readExact(ack, 4, 5000)) {
        Serial.println("No ACK, aborting");
        esp_camera_fb_return(fb);
        return;
    }

    uint8_t* ptr       = fb->buf;
    uint32_t remaining = jpegSize;
    uint32_t sent      = 0;
    unsigned long t0   = millis();

    while (remaining > 0) {
        if (millis() - t0 > 15000) { Serial.println("Send timeout"); break; }
        size_t chunk   = min((uint32_t)2048, remaining);
        size_t written = client.write(ptr, chunk);
        if (written == 0) { delay(5); continue; }
        ptr += written; sent += written; remaining -= written;
    }

    esp_camera_fb_return(fb);
    Serial.printf("Sent %u/%u in %lums\n", sent, jpegSize, millis() - t0);
}

void handleCommand(const String& jsonStr) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) return;
    const char* type = doc["type"];

    if (strcmp(type, "capture_image") == 0) {
        captureAndSend();
    } else if (strcmp(type, "start_continuous") == 0) {
        captureInterval = (doc["interval"] | 5) * 1000;
        continuousMode  = true;
        lastCapture     = 0;
        Serial.printf("Continuous: %ds\n", captureInterval / 1000);
    } else if (strcmp(type, "stop_continuous") == 0) {
        continuousMode = false;
        Serial.println("Continuous off");
    }
}

void receiveMessages() {
    if (!client.connected() || client.available() < 4) return;

    uint8_t lb[4];
    if (!readExact(lb, 4, 2000)) return;

    uint32_t msgLen = ((uint32_t)lb[0] << 24) | ((uint32_t)lb[1] << 16) |
                      ((uint32_t)lb[2] << 8)  |  (uint32_t)lb[3];

    if (msgLen == 0 || msgLen > 8192) {
        while (client.available()) client.read();
        return;
    }

    char* buf = new char[msgLen + 1];
    if (!readExact((uint8_t*)buf, msgLen, 3000)) { delete[] buf; return; }
    buf[msgLen] = '\0';
    handleCommand(String(buf));
    delete[] buf;
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.begin(115200);
    delay(500);
    initCamera();
    initLED();
    connectToWiFi();
    connectToServer();
    Serial.println("Ready");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        connectToWiFi();
        return;
    }
    if (!client.connected()) {
        Serial.println("Reconnecting...");
        client.stop();
        delay(3000);
        connectToServer();
        return;
    }

    if (justConnected) {
        justConnected = false;
        delay(500);
        return;
    }

    receiveMessages();

    if (continuousMode && millis() - lastCapture >= (uint32_t)captureInterval) {
        lastCapture = millis();
        captureAndSend();
    }

    handleLDR();

    static unsigned long lastPing = 0;
    if (millis() - lastPing >= 5000) {
        lastPing = millis();
        sendMessage("{\"type\":\"ping\"}");
    }

    delay(5);
}