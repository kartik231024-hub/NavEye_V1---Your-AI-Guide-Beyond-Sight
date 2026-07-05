<div align="center">

# NavEye V1
### Your AI Guide Beyond Sight

An AI-powered wearable that gives blind and visually impaired users a voice-guided way to see the world — object detection, text reading, face recognition, and emergency alerts, all in one small device.

![NavEye V1](https://www.electronicwings.com/storage/ProjectSection/Projects/74706/naveyev1---your-ai-guide-beyond-sight/icon/imresizer-NAVeye.png)

**License:** Apache-2.0 · **Build time:** ~11 hrs · **Level:** Intermediate
[Full build log on ElectronicWings →](https://www.electronicwings.com/users/KartikKashyap/projects/6680/naveyev1---your-ai-guide-beyond-sight)

</div>

---

## What it does

NavEye is worn like a small pendant or clipped to clothing. Behind it sits a Python server running Google's Gemini model, so the device isn't just reacting to what's in front of it — it's reasoning about it, in plain spoken language, in real time.

- **Talk to it** — ask questions, get scenes described, translate text or speech on the fly
- **Walk with it** — continuous mode calls out obstacles and reads signs as you move, hands-free
- **Teach it faces** — say a name while it captures a photo, and it'll recognize that person from then on
- **Trust it in an emergency** — one press sends an SOS email with a photo to your emergency contacts, no phone needed
- **Let it watch your battery** — it announces charge on boot and warns before it dies

## How it's built

Two ESP32 boards do the physical work, and a PC handles the AI:

```
ESP32-S3-Tiny  (mic, speaker, buttons) ──┐
                                          │  Wi-Fi
ESP32-CAM      (OV2640 camera)     ──────┴──►  Python server ──► Gemini AI
```

- **ESP32-S3-Tiny** — captures voice via I2S mic (I2S_NUM_1), plays audio out via I2S amp (I2S_NUM_0), reads button input, talks to the server over Wi-Fi
- **ESP32-CAM** — streams camera frames to the server over a kept-alive TCP connection
- **Python server** — runs speech-to-text → Gemini (vision + language) → text-to-speech, and handles the face database and SOS email dispatch

Everything — both firmwares, the server, the circuit, and a custom 3D-printed enclosure with an embossed braille label — was designed and built solo.

## Hardware

| Component | Qty |
|---|---|
| ESP32-S3-Tiny | 1 |
| ESP32-CAM (AI Thinker, OV2640) | 1 |
| MAX98357A I2S mono amp | 1 |
| INMP441 I2S MEMS mic | 1 |
| TP4056 battery charger | 1 |
| MT3608 boost converter | 1 |
| 3W 4Ω speaker | 1 |
| Tactile switches | 2 |
| Slide switch | 1 |
| 100µF 25V capacitor | 1 |
| Perf board | 1 |
| 18650 Li-ion cell | 1 |

Enclosure printed in PETG on a Bambu Lab A1 — Body and Lid, braille "Naveye" embossed on top.

## Getting it running

### 1 — ESP32-S3 firmware (Arduino IDE)
1. Add board URL under Preferences: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Install **esp32 by Espressif Systems**, select board **Waveshare ESP32-S3-Zero**
3. Open `NaveEye_V1.ino` and set:
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   const char* serverIP = "YOUR_PC_LAN_IP";
   ```
4. Enable **USB CDC On Boot**, then upload

### 2 — ESP32-CAM firmware
1. Wire for flash mode (IO0 → GND, upload only), select board **AI Thinker ESP32-CAM**
2. Partition scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**, upload speed 115200
3. Upload `NaveEye_V1_CAM.ino`, press RESET when the IDE shows "Connecting...."
4. Disconnect IO0 from GND, reset again to boot normally

### 3 — Python server (Thonny)
```bash
pip install google-generativeai gtts requests
```
Set your Gemini key ([get one here](https://aistudio.google.com/apikey)) and SOS details:
```python
GEMINI_API_KEY     = "your_key"
SOS_GMAIL_SENDER   = "your_dedicated_gmail@gmail.com"
SOS_GMAIL_PASSWORD = "16-char app password"  # myaccount.google.com/apppasswords
SOS_FAMILY_EMAILS  = ["contact1@example.com"]
SOS_DEVICE_NAME    = "Naveye"
SOS_USER_NAME      = "Your Name"
```
Run the script — you should see a `NOVA AI SERVER` banner print with the server's IP and ports.

> Keep API keys and the Gmail app password out of version control — move them into a `.env` or `config.py` and add it to `.gitignore`.

## Credit

Designed and built by **Kartik Kashyap** — firmware, backend, circuit, and enclosure, end to end.
