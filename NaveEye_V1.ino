/*
 * ESP32-S3-Tiny AI Assistant
 * Audio buffered in PSRAM (up to ~8s at 16kHz mono 16-bit)
 * AudioFileSourceBuffer wraps HTTP stream to prevent stutter/early-stop on TTS playback
 *
 * Board: ESP32S3 Dev Module
 * PSRAM: QSPI PSRAM (R2 variant)
 * Partition: Huge APP (3MB No OTA/1MB SPIFFS)
 * USB CDC On Boot: Enabled
 *
 * PIN MAPPING:
 *  NeoPixel  -> 38
 *  Button1   -> 1   (hold=record while held, dbl=continuous)
 *  Button2   -> 2   (hold=teach face, tap=recognize face)
 *  TTS_CTRL  -> 3   (LOW=not playing, FLOATING=playing)
 *  MIC WS    -> 4
 *  MIC SCK   -> 5
 *  MIC SD    -> 6
 *  SPK DOUT  -> 18
 *  SPK BCLK  -> 17
 *  SPK LRC   -> 16
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s_std.h>
#include <Adafruit_NeoPixel.h>
#include "AudioGeneratorMP3.h"
#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioOutput.h"

const char* ssid       = "YOUR_WIFI_SSID";
const char* password   = "YOUR_WIFI_PASSWORD";
const char* serverIP   = "YOUR_SERVER_LAN_IP";
const int   serverPort = 8080;

#define BUTTON_PIN    1
#define BUTTON2_PIN   2
#define TTS_CTRL_PIN  3
#define NEOPIXEL_PIN  38
#define I2S_MIC_WS    4
#define I2S_MIC_SCK   5
#define I2S_MIC_SD    6
#define I2S_SPK_DOUT  18
#define I2S_SPK_BCLK  17
#define I2S_SPK_LRC   16

#define SAMPLE_RATE   16000
#define SPK_RATE      24000
#define NUMPIXELS     1
#define SOS_HOLD_MS   1500
#define MAX_REC_BYTES (SAMPLE_RATE * 2 * 8)

// TTS buffering — absorbs WiFi jitter so decoder never starves mid-sentence
#define TTS_PREBUFFER_BYTES      (32 * 1024)
#define TTS_LOOP_FAIL_LIMIT      8
#define TTS_I2S_WRITE_TIMEOUT_MS 300  // was 100 — too tight, caused write drops under load

#define BATT_ADC_PIN      12
#define BATT_R1           100000.0f
#define BATT_R2           100000.0f
#define BATT_ADC_REF      3.3f
#define BATT_ADC_RES      4095.0f
#define BATT_MAX          4.2f
#define BATT_MIN          3.0f
#define BATT_CALIBRATION  1.240f
#define BATT_LOW_THRESH   25

Adafruit_NeoPixel pixels(NUMPIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
WiFiClient        tcpClient;
i2s_chan_handle_t mic_handle     = NULL;
i2s_chan_handle_t spk_handle     = NULL;
String            tts_url_global = "";
bool              isTTSPlaying   = false;

static int32_t  dma32[512];
static int16_t  dma16[256];

static uint8_t* recBuf   = nullptr;
static uint32_t recBytes = 0;

void setTTSPlaying(bool playing) {
    isTTSPlaying = playing;
    if (playing) {
        pinMode(TTS_CTRL_PIN, INPUT);
        digitalWrite(TTS_CTRL_PIN, LOW);
    } else {
        pinMode(TTS_CTRL_PIN, OUTPUT);
        digitalWrite(TTS_CTRL_PIN, LOW);
    }
}

void initTTSControlPin() {
    pinMode(TTS_CTRL_PIN, OUTPUT);
    digitalWrite(TTS_CTRL_PIN, LOW);
    isTTSPlaying = false;
}

class NewI2SOutput : public AudioOutput {
public:
    bool begin() override { return true; }
    bool ConsumeSample(int16_t s[2]) override {
        if (!spk_handle) return false;
        int16_t buf[2] = { s[LEFTCHANNEL], s[LEFTCHANNEL] };
        size_t w = 0;
        // Longer timeout: too-tight timeout drops samples under I2S contention,
        // decoder reads that as output failure and aborts playback.
        i2s_channel_write(spk_handle, buf, 4, &w, pdMS_TO_TICKS(TTS_I2S_WRITE_TIMEOUT_MS));
        return w == 4;
    }
    bool stop() override { return true; }
};

void setLED(uint8_t r, uint8_t g, uint8_t b) {
    pixels.setPixelColor(0, pixels.Color(r, g, b));
    pixels.show();
}
void ledIdle()       { setLED(0, 0, 20); }
void ledRecording()  { setLED(255, 0, 0); }
void ledProcessing() { setLED(255, 140, 0); }
void ledSpeaking()   { setLED(0, 255, 0); }
void ledError()      { setLED(255, 0, 0); }

void ledSOS() {
    for (int i = 0; i < 6; i++) {
        setLED(255, 0, 0); delay(120);
        pixels.clear(); pixels.show(); delay(80);
    }
    setLED(255, 0, 0);
}

void startupAnimation() {
    for (int j = 0; j < 256; j++) {
        pixels.setPixelColor(0, pixels.gamma32(pixels.ColorHSV(j * 256, 255, 50)));
        pixels.show(); delay(3);
    }
    for (int i = 0; i < 2; i++) {
        pixels.setPixelColor(0, pixels.Color(255, 255, 255));
        pixels.show(); delay(100);
        pixels.clear(); pixels.show(); delay(100);
    }
}

void connectWiFi() {
    Serial.print("WiFi connecting");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println("\nWiFi: " + WiFi.localIP().toString());
}

bool connectServer() {
    Serial.print("Connecting to server");
    int retry = 0;
    while (!tcpClient.connect(serverIP, serverPort)) {
        delay(1000); Serial.print(".");
        if (++retry >= 15) { Serial.println("\nFailed"); return false; }
    }
    tcpClient.setNoDelay(true);
    Serial.println("\nServer connected");
    return true;
}

void releaseMic() {
    if (mic_handle) {
        i2s_channel_disable(mic_handle);
        i2s_del_channel(mic_handle);
        mic_handle = NULL;
        delay(50);
    }
}

void setupMic() {
    releaseMic();
    delay(100);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &mic_handle) != ESP_OK) {
        Serial.println("Mic channel init failed"); return;
    }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_MIC_SCK,
            .ws   = (gpio_num_t)I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_MIC_SD,
            .invert_flags = { false, false, false }
        },
    };
    sc.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_channel_init_std_mode(mic_handle, &sc);
    i2s_channel_enable(mic_handle);
    Serial.println("Mic ready");
}

void releaseSpeaker() {
    if (spk_handle) {
        i2s_channel_disable(spk_handle);
        i2s_del_channel(spk_handle);
        spk_handle = NULL;
        delay(50);
    }
}

bool setupSpeaker() {
    releaseSpeaker();
    delay(100);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, &spk_handle, NULL) != ESP_OK) {
        Serial.println("Speaker channel init failed");
        spk_handle = NULL; return false;
    }
    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_SPK_BCLK,
            .ws   = (gpio_num_t)I2S_SPK_LRC,
            .dout = (gpio_num_t)I2S_SPK_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false }
        },
    };
    i2s_channel_init_std_mode(spk_handle, &sc);
    i2s_channel_enable(spk_handle);
    Serial.println("Speaker ready");
    return true;
}

void buildWAVHeader(uint8_t* dst, uint32_t dataBytes) {
    uint32_t sr = SAMPLE_RATE, br = SAMPLE_RATE * 2, cs = 36 + dataBytes;
    uint16_t ch = 1, bps = 16, ba = 2, af = 1;
    uint32_t sc1 = 16;
    memcpy(dst,      "RIFF", 4); memcpy(dst + 4,  &cs,       4);
    memcpy(dst + 8,  "WAVE", 4); memcpy(dst + 12, "fmt ",    4);
    memcpy(dst + 16, &sc1,   4); memcpy(dst + 20, &af,       2);
    memcpy(dst + 22, &ch,    2); memcpy(dst + 24, &sr,       4);
    memcpy(dst + 28, &br,    4); memcpy(dst + 32, &ba,       2);
    memcpy(dst + 34, &bps,   2); memcpy(dst + 36, "data",    4);
    memcpy(dst + 40, &dataBytes, 4);
}

void recordAudio() {
    if (!recBuf) { Serial.println("No PSRAM buffer!"); return; }
    recBytes = 0;
    uint8_t* pcmStart = recBuf + 44;
    size_t br;
    ledRecording();
    Serial.println("Recording...");
    unsigned long recStart = millis();
    while (digitalRead(BUTTON_PIN) == LOW && (millis() - recStart) < 8000) {
        if (i2s_channel_read(mic_handle, dma32, sizeof(dma32), &br, 100) == ESP_OK && br > 0) {
            int frames = br / 8;
            for (int i = 0; i < frames; i++)
                dma16[i] = (int16_t)(dma32[i * 2] >> 16);
            uint32_t bytesToWrite = frames * 2;
            if (recBytes + bytesToWrite > MAX_REC_BYTES)
                bytesToWrite = MAX_REC_BYTES - recBytes;
            if (bytesToWrite == 0) break;
            memcpy(pcmStart + recBytes, dma16, bytesToWrite);
            recBytes += bytesToWrite;
        }
    }
    buildWAVHeader(recBuf, recBytes);
    Serial.printf("Recorded: %.1fs (%u bytes)\n", recBytes / 32000.0, recBytes);
}

void recordAudioBtn2() {
    if (!recBuf) { Serial.println("No PSRAM buffer!"); return; }
    recBytes = 0;
    uint8_t* pcmStart = recBuf + 44;
    size_t br;
    ledRecording();
    Serial.println("Btn2 recording...");
    unsigned long recStart = millis();
    while (digitalRead(BUTTON2_PIN) == LOW && (millis() - recStart) < 8000) {
        if (i2s_channel_read(mic_handle, dma32, sizeof(dma32), &br, 100) == ESP_OK && br > 0) {
            int frames = br / 8;
            for (int i = 0; i < frames; i++)
                dma16[i] = (int16_t)(dma32[i * 2] >> 16);
            uint32_t bytesToWrite = frames * 2;
            if (recBytes + bytesToWrite > MAX_REC_BYTES)
                bytesToWrite = MAX_REC_BYTES - recBytes;
            if (bytesToWrite == 0) break;
            memcpy(pcmStart + recBytes, dma16, bytesToWrite);
            recBytes += bytesToWrite;
        }
    }
    buildWAVHeader(recBuf, recBytes);
    Serial.printf("Btn2 recorded: %.1fs (%u bytes)\n", recBytes / 32000.0, recBytes);
}

void sendJSON(const char* type) {
    StaticJsonDocument<64> doc;
    doc["type"] = type;
    String js; serializeJson(doc, js);
    uint32_t len = js.length();
    uint8_t lb[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)len
    };
    tcpClient.write(lb, 4);
    tcpClient.print(js);
}

void sendAudio(const char* msgType) {
    if (!recBuf || recBytes == 0) { Serial.println("No audio to send"); return; }
    uint32_t totalSize = 44 + recBytes;
    Serial.printf("Sending %u bytes as '%s'...\n", totalSize, msgType);

    StaticJsonDocument<128> doc;
    doc["type"]        = msgType;
    doc["size"]        = totalSize;
    doc["sample_rate"] = SAMPLE_RATE;
    String js; serializeJson(doc, js);
    uint32_t len = js.length();
    uint8_t lb[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)len
    };
    tcpClient.write(lb, 4);
    tcpClient.print(js);
    delay(50);

    uint32_t sent = 0;
    unsigned long t0 = millis();
    while (sent < totalSize) {
        if (millis() - t0 > 30000) { Serial.println("Send timeout"); break; }
        size_t n = tcpClient.write(recBuf + sent, totalSize - sent);
        if (n > 0) sent += n; else delay(1);
    }
    Serial.printf("Sent: %u / %u bytes\n", sent, totalSize);
}

String receiveText() {
    Serial.println("Waiting for response...");
    ledProcessing();
    unsigned long timeout = millis() + 60000;
    while (tcpClient.available() < 4 && millis() < timeout) delay(10);
    if (tcpClient.available() < 4) { Serial.println("Timeout"); return ""; }

    uint8_t lb[4];
    tcpClient.readBytes(lb, 4);
    uint32_t msgLen = ((uint32_t)lb[0] << 24) | ((uint32_t)lb[1] << 16) |
                      ((uint32_t)lb[2] << 8)  | lb[3];
    if (msgLen == 0 || msgLen > 50000) { Serial.println("Bad length"); return ""; }

    char* buf = new char[msgLen + 1];
    size_t got = 0;
    timeout = millis() + 10000;
    while (got < msgLen && millis() < timeout) {
        if (tcpClient.available()) buf[got++] = tcpClient.read();
        else delay(1);
    }
    buf[got] = '\0';

    String text = "";
    DynamicJsonDocument jdoc(4096);
    if (deserializeJson(jdoc, buf) == DeserializationError::Ok) {
        String type = jdoc["type"].as<String>();
        if (type == "gemini_response") {
            text           = jdoc["text"].as<String>();
            tts_url_global = jdoc["tts_url"].as<String>();
            Serial.println("Response: " + text);
        } else if (type == "error") {
            Serial.println("Server error: " + jdoc["message"].as<String>());
        }
    }
    delete[] buf;
    return text;
}

void streamAndPlayTTS(const String& text) {
    String url = tts_url_global;
    if (url.length() == 0) {
        String encoded = "";
        for (int i = 0; i < (int)text.length(); i++) {
            char c = text[i];
            if (c == ' ') encoded += '+';
            else if (isalnum(c)) encoded += c;
            else { char h[4]; sprintf(h, "%%%02X", (uint8_t)c); encoded += h; }
        }
        if (encoded.length() > 200) encoded = encoded.substring(0, 200);
        url = "http://translate.google.com/translate_tts?ie=UTF-8&q=" + encoded + "&tl=en&client=tw-ob";
    }
    Serial.println("TTS: " + url);

    setTTSPlaying(true);
    ledSpeaking();
    releaseMic();

    if (!setupSpeaker()) {
        Serial.println("Speaker setup failed");
        setupMic(); ledIdle(); setTTSPlaying(false); return;
    }

    AudioFileSourceHTTPStream* src  = new AudioFileSourceHTTPStream(url.c_str());
    // Buffer absorbs WiFi jitter — without it, a brief stall starves the decoder
    // mid-sentence (heard as a stutter) and AudioGeneratorMP3 aborts.
    AudioFileSourceBuffer*     buff = new AudioFileSourceBuffer(src, TTS_PREBUFFER_BYTES);
    NewI2SOutput*               out = new NewI2SOutput();
    AudioGeneratorMP3*          mp3 = new AudioGeneratorMP3();

    if (!mp3->begin(buff, out)) {
        Serial.println("MP3 failed");
        delete mp3; delete out; delete buff; delete src;
        releaseSpeaker(); setupMic(); ledIdle(); setTTSPlaying(false); return;
    }

    // Retry transient loop() failures before aborting — a brief underrun is
    // recoverable once the buffer refills.
    int failCount = 0;
    while (mp3->isRunning()) {
        if (mp3->loop()) {
            failCount = 0;
        } else {
            failCount++;
            if (failCount >= TTS_LOOP_FAIL_LIMIT) {
                Serial.println("TTS: too many consecutive failures, stopping");
                mp3->stop();
                break;
            }
            delay(5);
        }
    }

    delete mp3; delete out; delete buff; delete src;
    releaseSpeaker();
    delay(100);
    setupMic();
    ledIdle();
    sendJSON("tts_done");
    Serial.println("TTS done");
    setTTSPlaying(false);
}

int readBatteryPercent() {
    long sum = 0;
    for (int i = 0; i < 10; i++) { sum += analogRead(BATT_ADC_PIN); delay(10); }
    float vPin = (sum / 10.0f / BATT_ADC_RES) * BATT_ADC_REF * BATT_CALIBRATION;
    float vBat = vPin * ((BATT_R1 + BATT_R2) / BATT_R2);
    vBat = constrain(vBat, BATT_MIN, BATT_MAX);
    return (int)(((vBat - BATT_MIN) / (BATT_MAX - BATT_MIN)) * 100.0f);
}

void sendBatteryStatus() {
    int pct = readBatteryPercent();
    Serial.printf("Battery: %d%%\n", pct);

    StaticJsonDocument<128> doc;
    doc["type"]    = "battery_status";
    doc["percent"] = pct;
    String js; serializeJson(doc, js);
    uint32_t len = js.length();
    uint8_t lb[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)len
    };
    tcpClient.write(lb, 4);
    tcpClient.print(js);

    String text = receiveText();
    if (text.length() > 0) streamAndPlayTTS(text);
}

void sendSOS() {
    Serial.println("[SOS] Triggered!");
    ledSOS();
    if (!tcpClient.connected()) {
        if (!connectServer()) { ledError(); delay(2000); ledIdle(); return; }
    }
    StaticJsonDocument<128> doc;
    doc["type"]      = "sos";
    doc["device_id"] = "Nova-ESP32S3Tiny";
    String js; serializeJson(doc, js);
    uint32_t len = js.length();
    uint8_t lb[4] = {
        (uint8_t)(len >> 24), (uint8_t)(len >> 16),
        (uint8_t)(len >> 8),  (uint8_t)len
    };
    tcpClient.write(lb, 4);
    tcpClient.print(js);
    String reply = receiveText();
    if (reply.length() > 0) {
        streamAndPlayTTS(reply);
    } else {
        tts_url_global = "";
        streamAndPlayTTS("S O S sent. Help is on the way.");
    }
}

void handleButton2() {
    if (digitalRead(BUTTON2_PIN) == HIGH) return;

    unsigned long pressStart = millis();
    while (digitalRead(BUTTON2_PIN) == LOW && millis() - pressStart < 500) delay(5);

    if (digitalRead(BUTTON2_PIN) == LOW) {
        Serial.println("Btn2 hold — teach face");
        setLED(255, 0, 255);
        recordAudioBtn2();
        ledProcessing();
        sendAudio("teach_face");
        String text = receiveText();
        if (text.length() > 0) streamAndPlayTTS(text);
        else ledIdle();
    } else {
        Serial.println("Btn2 tap — recognize face");
        ledProcessing();
        StaticJsonDocument<64> doc;
        doc["type"] = "recognize_face";
        String js; serializeJson(doc, js);
        uint32_t len = js.length();
        uint8_t lb[4] = {
            (uint8_t)(len >> 24), (uint8_t)(len >> 16),
            (uint8_t)(len >> 8),  (uint8_t)len
        };
        tcpClient.write(lb, 4);
        tcpClient.print(js);
        String text = receiveText();
        if (text.length() > 0) streamAndPlayTTS(text);
        else ledIdle();
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\nESP32-S3-Tiny AI Assistant");

    pixels.begin();
    pixels.clear();
    pixels.show();

    pinMode(BUTTON_PIN,  INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);
    initTTSControlPin();

    recBuf = (uint8_t*)ps_malloc(MAX_REC_BYTES + 44);
    if (!recBuf) {
        Serial.println("PSRAM alloc failed! Enable PSRAM in Tools.");
        ledError();
    } else {
        Serial.printf("PSRAM buffer: %u bytes at %p\n", MAX_REC_BYTES + 44, recBuf);
    }

    startupAnimation();
    connectWiFi();
    connectServer();
    setupMic();
    ledIdle();

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    sendBatteryStatus();

    Serial.println("Ready.");
    Serial.println("Btn1: hold=record | double press=continuous");
    Serial.println("Btn2: hold=teach face | press=recognize face");
    Serial.println("Btn1+Btn2 hold 1.5s = SOS");

    xTaskCreatePinnedToCore(
        [](void*) { while (true) { ::loop(); vTaskDelay(1); } },
        "mainTask", 16384, NULL, 1, NULL, 0
    );
}

void loop() {
    if (xTaskGetCurrentTaskHandle() == xTaskGetHandle("loopTask")) {
        vTaskDelete(NULL); return;
    }

    if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }
    if (!tcpClient.connected())         { connectServer(); return; }

    if (tcpClient.available() >= 4) {
        String text = receiveText();
        if (text.length() > 0) streamAndPlayTTS(text);
        return;
    }

    if (digitalRead(BUTTON_PIN) == LOW && digitalRead(BUTTON2_PIN) == LOW) {
        unsigned long bothStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW && digitalRead(BUTTON2_PIN) == LOW) {
            if (millis() - bothStart >= SOS_HOLD_MS) {
                sendSOS();
                while (digitalRead(BUTTON_PIN) == LOW || digitalRead(BUTTON2_PIN) == LOW) delay(10);
                return;
            }
            delay(5);
        }
    }

    handleButton2();

    if (digitalRead(BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(BUTTON_PIN) == LOW && millis() - pressStart < 500) delay(5);

        if (digitalRead(BUTTON_PIN) == LOW) {
            recordAudio();
            ledProcessing();
            sendAudio("audio_start");
            String text = receiveText();
            if (text.length() == 0) {
                ledError(); delay(2000); setupMic(); ledIdle();
            } else {
                streamAndPlayTTS(text);
            }
            if (!tcpClient.connected()) connectServer();
        } else {
            unsigned long waitStart = millis();
            bool secondPress = false;
            while (millis() - waitStart < 400) {
                if (digitalRead(BUTTON_PIN) == LOW) {
                    secondPress = true;
                    while (digitalRead(BUTTON_PIN) == LOW) delay(5);
                    break;
                }
                delay(5);
            }
            if (secondPress) {
                Serial.println("Btn1 double press — continuous toggle");
                StaticJsonDocument<64> doc;
                doc["type"]       = "button_press";
                doc["press_type"] = "double";
                String js; serializeJson(doc, js);
                uint32_t len = js.length();
                uint8_t lb[4] = {
                    (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                    (uint8_t)(len >> 8),  (uint8_t)len
                };
                tcpClient.write(lb, 4);
                tcpClient.print(js);
            }
        }
    }

    static unsigned long lastBattCheck       = 0;
    static bool          battWarnedThisCycle = false;
    if (millis() - lastBattCheck > 300000UL) {
        lastBattCheck = millis();
        int pct = readBatteryPercent();
        if (pct < BATT_LOW_THRESH && !battWarnedThisCycle) {
            battWarnedThisCycle = true;
            StaticJsonDocument<128> doc;
            doc["type"]    = "battery_status";
            doc["percent"] = pct;
            String js; serializeJson(doc, js);
            uint32_t len = js.length();
            uint8_t lb[4] = {
                (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                (uint8_t)(len >> 8),  (uint8_t)len
            };
            tcpClient.write(lb, 4);
            tcpClient.print(js);
            String text = receiveText();
            if (text.length() > 0) streamAndPlayTTS(text);
        } else if (pct >= BATT_LOW_THRESH) {
            battWarnedThisCycle = false;
        }
    }

    delay(10);
}
