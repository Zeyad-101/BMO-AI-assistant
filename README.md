<div align="center">

<img src="bmo_photo.png" alt="BMO AI Assistant" width="400"/>

# BMO AI Assistant 🎮

**An ESP32-powered conversational AI voice assistant — because why buy a smart speaker when you can build one that looks like your favorite Adventure Time character?**

![ESP32](https://img.shields.io/badge/ESP32-Microcontroller-blue?style=flat-square)
![OpenAI](https://img.shields.io/badge/OpenAI-GPT--4o%20%7C%20TTS-412991?style=flat-square)
![Arduino](https://img.shields.io/badge/Arduino-Framework-00979D?style=flat-square)
![ECU](https://img.shields.io/badge/Egyptian%20Chinese%20University-May%202026-red?style=flat-square)

</div>

---

## What is this?

BMO AI Assistant is a handmade, cardboard-bodied AI voice assistant built on the ESP32 microcontroller. You type something into the Serial Monitor, and BMO talks back — using OpenAI's GPT-4o to think and their TTS engine to speak — while the little OLED screen on his face shows the response text. The whole thing runs on WiFi, fits on a breadboard, and costs a fraction of a commercial smart speaker.

Named after BMO from Adventure Time, obviously. He even has the face, the D-pad, and the colored shape buttons. We hand-painted everything.

---

## Repository Structure

```
BMO-AI-Assistant/
├── bmo_main/              ← Full version: ESP32 + display + speaker
│   └── bmo_main.ino
├── BMO(without display)/  ← Simpler version: no OLED, just voice output
│   └── bmo_no_display.ino
└── README.md
```

### Which sketch should you use?

| Sketch | Use it when... |
|---|---|
| `bmo_main` | You have the OLED display wired up (GPIO 8 & 9). This is the full experience. |
| `BMO(without display)` | You want to test or run BMO without the display — same voice pipeline, just no screen output. |

**We recommend uploading both.** Each is its own self-contained sketch. Start with `bmo_main` if you have all the hardware, or `BMO(without display)` to get the audio pipeline running first.

---

## How it works

The flow is pretty straightforward once you see it laid out:

```
You type in Serial Monitor
        ↓
  ESP32 reads via UART
        ↓
  HTTPS POST → OpenAI GPT-4o
        ↓
  Text response → OpenAI TTS
        ↓
  Audio buffered on ESP32
        ↓
  I2S out → MAX98357 amp → Speaker
        +
  I2C → OLED shows the text
```

End-to-end latency is typically **1–3 seconds**, depending on your WiFi and OpenAI's response time.

---

## Hardware

Here's everything you need to build your own BMO:

| Component | Role |
|---|---|
| ESP32 | The brain — handles WiFi, I2S, I2C, and UART all at once |
| MAX98357 (I2S Amp) | Converts the digital audio stream to speaker output |
| INMP441 (I2S Mic) | Microphone — wired in but used in the extended version |
| SSD1306 OLED (I2C) | BMO's face screen — shows response text |
| Small speaker | Makes BMO actually talk |
| Cardboard + blue paint | The body, arms, legs, and soul |

### Pin Connections

**MAX98357 (I2S Amplifier)**

| Pin | Connected To | What it does |
|---|---|---|
| VIN | 5V | Powers the amp |
| GND | GND | Ground |
| BCLK | GPIO 38 | Bit Clock |
| LRC | GPIO 39 | Left/Right Clock |
| DIN | GPIO 40 | Audio data from ESP32 |
| SD | 3.3V | Keeps the amp on |
| GAIN | GND | Sets 12dB gain |

**Display (I2C)**

| Pin | Connected To | What it does |
|---|---|---|
| VDD | 3.3V | Powers the display |
| GND | GND | Ground |
| SDA | GPIO 8 | Serial data |
| SCL | GPIO 9 | Serial clock |

**INMP441 (Microphone)**

| Pin | Connected To |
|---|---|
| WS | GPIO 15 |
| SCK | GPIO 16 |
| SD | GPIO 17 |

---

## Setup & Upload

### 1. Install dependencies

In Arduino IDE, install these libraries:
- `U8g2` or `Adafruit SSD1306` — for the OLED display
- `ESP32 Arduino Core` — make sure you have the latest version

### 2. Configure your credentials

Before uploading, open the sketch and fill in:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
// API key is stored in NVS — see the sketch for setup instructions
```

> **Security note:** The OpenAI API key is stored in ESP32 Non-Volatile Storage (NVS), not hardcoded in the sketch. This means it won't accidentally show up in your version history if you push to GitHub. Follow the comments in the code to write it to NVS once before your first run.

### 3. Select your board

In Arduino IDE:
- Board: `ESP32 Dev Module` (or whatever variant you have)
- Upload Speed: `115200`
- Port: whichever COM port your ESP32 shows up on

### 4. Upload & open Serial Monitor

Upload the sketch, open Serial Monitor at **115200 baud**, and type anything. BMO will respond.

---

## What BMO can (and can't) do right now

**Can do:**
- Answer any question you type
- Speak the response out loud through the speaker
- Show the text on the OLED screen at the same time
- Handle all of this over WiFi with secure HTTPS

**Current limitations:**
- Input is text-only through Serial Monitor — you need a USB connection while using it
- No persistent conversation memory — each message is its own independent exchange
- WiFi credentials need to be in the firmware before you flash

**Ideas for future versions:**
- Wake word detection so you can just say "Hey BMO"
- Physical button to trigger a prompt
- Conversation history stored on-device
- WiFi setup over Bluetooth so no hardcoding needed
- OTA firmware updates

---

## Team

This project was built by six engineering students at the **Egyptian Chinese University**, Faculty of Engineering & Technology — May 2026.

| Name | GitHub |
|---|---|
| Zeyad Waleed Amin | [@Zeyad-101](https://github.com/Zeyad-101) |
| Mai Ahmed Khalaf | [@MaiKhalaf](https://github.com/MaiKhalaf) |
| Adam Tamer Ghobashy | [@Adam-Ghobashy](https://github.com/Adam-Ghobashy) |
| Judy Ehab Abdelmagied | — |
| Basma Mohamed Ibrahim | [@basmamohamedd0](https://github.com/basmamohamedd0) |
| Maha Mohamed Nasr | [@mahhanasr](https://github.com/mahhanasr) |

---

## License

This project is open for learning and personal use. If you build your own BMO, we'd genuinely love to see it.

---

<div align="center">
<i>"I am a living game console." — BMO</i>
</div>
