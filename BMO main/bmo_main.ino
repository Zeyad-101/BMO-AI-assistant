// ╔══════════════════════════════════════════════════════════════╗
// ║                    BMO  –  AI Assistant                      ║
// ║        ESP32-S3 + MAX98357A + SH1106 OLED (1.3")             ║
// ║   Input: Serial Monitor (Text)                               ║
// ║   LLM: OpenAI GPT-3.5-turbo                                  ║
// ║   TTS: OpenAI TTS-1-HD (PCM Stream)                          ║
// ║   Display: SH1106 I2C — SDA=GPIO8, SCL=GPIO9                 ║
// ╚══════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "secrets.h"

// ─────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────
#define I2S_AMP_BCLK  38
#define I2S_AMP_LRC   39
#define I2S_AMP_DIN   40
#define OLED_SDA      8
#define OLED_SCL      9

// ─────────────────────────────────────────
//  AUDIO SETTINGS
//
//  OpenAI PCM output is 24kHz, 16-bit, mono.
//  MAX98357A works best with a clean I2S signal.
//  APLL is DISABLED — on ESP32-S3 it can introduce
//  jitter/noise with external clocks. Use PLL_D2 instead.
//  VOLUME_GAIN kept conservative to avoid clipping noise.
// ─────────────────────────────────────────
#define PLAYBACK_SAMPLE_RATE  24000
#define BYTES_PER_SAMPLE      2
#define BUFFER_BYTES          (1024 * 1024)
#define I2S_AMP_PORT          I2S_NUM_1

// ── Volume: keep ≤ 2.5 to prevent clipping distortion ────────
#define VOLUME_GAIN  2.0f

// ─────────────────────────────────────────
//  API ENDPOINTS
// ─────────────────────────────────────────
#define OPENAI_HOST "api.openai.com"
#define LLM_PATH    "/v1/chat/completions"
#define TTS_PATH    "/v1/audio/speech"

// ─────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────
int16_t* psram_buffer = nullptr;

#define MAX_HISTORY 10
String conv_user[MAX_HISTORY];
String conv_model[MAX_HISTORY];
int    conv_len = 0;

// ─────────────────────────────────────────
//  OLED — SH1106 128×64 via I2C
// ─────────────────────────────────────────
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ═══════════════════════════════════════════════════════════════
//  BMO FACE — fills entire 128×64 screen
//
//  Design faithful to the Adventure Time BMO sprite:
//    • White filled screen (face plate)
//    • 1px black border
//    • Two tall pill-shaped black eyes
//    • Mouth: just a pure rounded black opening
//      that grows in size across 4 frames, like the cartoon.
//
//  Frame 0 = closed smile (resting)
//  Frame 1 = small oval open
//  Frame 2 = medium oval open
//  Frame 3 = wide oval open (widest talking frame)
// ═══════════════════════════════════════════════════════════════

void drawBMOFace(uint8_t mouthFrame) {
  u8g2.clearBuffer();

  // ── 1. Fill whole screen white (lit face plate) ───────────────
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 0, 128, 64);

  // ── 2. 1px black border ───────────────────────────────────────
  u8g2.setDrawColor(0);
  u8g2.drawFrame(0, 0, 128, 64);

  // ── 3. Eyes — black rounded-rect pills ───────────────────────
  //    BMO's eyes are tall and narrow with rounded corners.
  //    Left  eye: x=31, y=14, 12w × 14h, r=3
  //    Right eye: x=85, y=14, 12w × 14h, r=3
  u8g2.setDrawColor(0);
  u8g2.drawRBox(31, 14, 12, 14, 3);   // left eye
  u8g2.drawRBox(85, 14, 12, 14, 3);   // right eye

  // ── 4. Mouth — NO TEETH, pure BMO cartoon style ──────────────
  //
  //  BMO's mouth is just a black rounded oval that opens wider
  //  as he talks exactly like the show.
  //  All frames are centered at x=64, sitting in the lower face.
  //
  //  Frame 0: Closed — gentle parabola smile (resting)
  //  Frame 1: Small  — short wide oval  (just cracked open)
  //  Frame 2: Medium — taller oval      (mid-syllable)
  //  Frame 3: Wide   — full open oval   (peak syllable / vowel)

  u8g2.setDrawColor(0);

  switch (mouthFrame) {

    // ── Frame 0: Resting closed smile ────────────────────────
    // Thin upward-curving parabola, 2px thick.
    case 0:
    {
      int cx = 64, cy = 47;
      float k = 5.0f / (24.0f * 24.0f);  // 5px dip over ±24px
      for (int dx = -24; dx <= 24; dx++) {
        int dy = (int)(k * (float)(dx * dx));
        u8g2.drawPixel(cx + dx, cy - dy);
        u8g2.drawPixel(cx + dx, cy - dy + 1);
      }
      break;
    }

    // ── Frame 1: Small opening ────────────────────────────────
    // A small black rounded ellipse — barely open, like "m" or "b"
    case 1:
    {
      // drawEllipse uses rx, ry as radii
      // Filled black ellipse: center (64,46), 20w x 6h
      u8g2.drawFilledEllipse(64, 46, 10, 3, U8G2_DRAW_ALL);
      break;
    }

    // ── Frame 2: Medium opening ───────────────────────────────
    // Wider and taller — like pronouncing "ah" lightly
    case 2:
    {
      // Filled black ellipse: center (64,46), 28w x 10h
      u8g2.drawFilledEllipse(64, 46, 14, 5, U8G2_DRAW_ALL);
      break;
    }

    // ── Frame 3: Wide open — peak vowel ───────────────────────
    // Largest oval, BMO's classic wide-open talking mouth.
    // Use drawRBox for a slightly wider/squarer shape at peak.
    case 3:
    {
      // Black rounded rectangle: wide open mouth
      // x=36, y=38, w=56, h=16, r=8
      u8g2.drawRBox(36, 38, 56, 16, 8);
      break;
    }
  }

  u8g2.sendBuffer();
}

// ── Status bar: black strip at bottom, centered white text ────
void drawStatus(const char* msg) {
  drawBMOFace(0);
  u8g2.setDrawColor(0);
  u8g2.drawBox(1, 53, 126, 10);
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_5x7_tr);
  int tw = u8g2.getStrWidth(msg);
  u8g2.setCursor((128 - tw) / 2, 61);
  u8g2.print(msg);
  u8g2.sendBuffer();
}

// ─────────────────────────────────────────
//  MOUTH ANIMATION TASK (Core 0)
//
//  Pattern cycles through frames 0-3 in a speech-like rhythm.
//  No teeth — just BMO's oval mouth opening and closing.
// ─────────────────────────────────────────
volatile bool mouthAnimating = false;

void mouthAnimationTask(void* param) {
  // Natural speech rhythm: vowel peaks at frame 3,
  // quick closures at frame 0, mid-opens at 1 and 2.
  const uint8_t pattern[] = {1, 2, 3, 2, 1, 0, 2, 3, 1, 3, 2, 0, 1, 3, 2, 1, 0, 2};
  const int patLen = sizeof(pattern);
  int idx = 0;

  while (true) {
    if (mouthAnimating) {
      drawBMOFace(pattern[idx % patLen]);
      idx++;
      vTaskDelay(pdMS_TO_TICKS(110));   // ~9 fps — natural speech pace
    } else {
      drawBMOFace(0);   // resting closed smile
      idx = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// ==========================================
//  PROTOTYPES
// ==========================================
void initAmp();
void connectWiFi();
void ensureWiFi();
void amplifyBuffer(size_t byte_count);
void playAudio(size_t byte_count);
String askOpenAI(const String& user_text);
size_t openAITTS(const String& text);
String readHTTPResponse(WiFiClientSecure& client);
size_t readBinaryHTTPResponse(WiFiClientSecure& client, uint8_t* buffer, size_t max_len);

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n🟢 BMO booting up...");

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setContrast(220);
  drawBMOFace(0);
  Serial.println("✅ OLED ready");

  if (psramInit()) {
    psram_buffer = (int16_t*) ps_malloc(BUFFER_BYTES);
    if (!psram_buffer) { Serial.println("❌ PSRAM alloc failed!"); while(true) delay(1000); }
    Serial.printf("✅ PSRAM: %d KB\n", BUFFER_BYTES / 1024);
  } else { Serial.println("❌ PSRAM NOT FOUND!"); while(true) delay(1000); }

  initAmp();

  drawStatus("Connecting...");
  connectWiFi();

  xTaskCreatePinnedToCore(mouthAnimationTask, "MouthAnim", 2048, NULL, 1, NULL, 0);

  drawBMOFace(0);
  Serial.println("\n✅ BMO ready! Type a message and press Enter.");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  if (Serial.available() > 0) {
    String userInput = Serial.readStringUntil('\n');
    userInput.trim();

    if (userInput.length() > 0) {
      Serial.println("\n📝 You: " + userInput);
      drawStatus("Thinking...");

      String reply = askOpenAI(userInput);
      if (reply.length() == 0) { Serial.println("⚠️ GPT empty."); drawBMOFace(0); return; }
      Serial.println("💬 BMO: " + reply);
      drawStatus("Talking...");

      size_t tts_bytes = openAITTS(reply);
      if (tts_bytes == 0) { Serial.println("⚠️ TTS empty."); drawBMOFace(0); return; }

      // Gentle amplification — see VOLUME_GAIN note above
      amplifyBuffer(tts_bytes);

      Serial.printf("▶️ Playing %d KB...\n", tts_bytes / 1024);
      mouthAnimating = true;
      playAudio(tts_bytes);
      mouthAnimating = false;

      drawBMOFace(0);
      Serial.println("\n✅ Ready! Type another message...");
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  VOLUME AMPLIFICATION
//  Conservative gain to prevent clipping (the main source of
//  noise/distortion). For hardware volume increase, pull the
//  MAX98357A GAIN pin high or use a resistor divider.
// ═══════════════════════════════════════════════════════════════
void amplifyBuffer(size_t byte_count) {
  size_t samples = byte_count / BYTES_PER_SAMPLE;
  for (size_t i = 0; i < samples; i++) {
    int32_t s = (int32_t)psram_buffer[i];
    s = (int32_t)(s * VOLUME_GAIN);
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    psram_buffer[i] = (int16_t)s;
  }
}

// ═══════════════════════════════════════════════════════════════
//  I2S INIT — Tuned for ESP32-S3 + MAX98357A + 24kHz PCM
// ═══════════════════════════════════════════════════════════════
void initAmp() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = PLAYBACK_SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,      // fewer, larger buffers = more stable on ESP32-S3
    .dma_buf_len          = 1024,   // samples per DMA buffer (not bytes) — safe max for S3
    .use_apll             = false,  // ← CRITICAL: APLL causes noise on ESP32-S3
    .tx_desc_auto_clear   = true,   // output silence on underrun
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_AMP_BCLK,
    .ws_io_num    = I2S_AMP_LRC,
    .data_out_num = I2S_AMP_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_AMP_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_AMP_PORT, &pins);

  // Explicitly set sample rate after install to ensure clock is correct
  i2s_set_clk(I2S_AMP_PORT, PLAYBACK_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  Serial.println("✅ Amp I2S ready (APLL disabled, 8x1024 DMA)");
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.printf("📶 Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(1000);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) { Serial.println("\n❌ WiFi FAILED."); return; }
    delay(500); Serial.print(".");
  }
  Serial.printf("\n✅ WiFi: %s\n", WiFi.localIP().toString().c_str());
}

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(500);
  }
}

// ═══════════════════════════════════════════════════════════════
//  PLAY AUDIO
//  Chunk size increased to 2048 bytes to reduce I2S write
//  overhead and keep DMA buffers fed continuously.
// ═══════════════════════════════════════════════════════════════
void playAudio(size_t byte_count) {
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  size_t samples = byte_count / BYTES_PER_SAMPLE;
  size_t bytes_written;
  const size_t chunk = 4096;  // 2× DMA buf (8KB total) — stable on ESP32-S3
  for (size_t i = 0; i < samples; i += chunk / BYTES_PER_SAMPLE) {
    size_t to_write = min((size_t)chunk, (size_t)((samples - i) * BYTES_PER_SAMPLE));
    i2s_write(I2S_AMP_PORT, &psram_buffer[i], to_write, &bytes_written, portMAX_DELAY);
  }
  // Brief silence flush — prevents last-buffer pop on some MAX98357A boards
  delay(50);
  i2s_zero_dma_buffer(I2S_AMP_PORT);
}

// ═══════════════════════════════════════════════════════════════
//  OPENAI LLM
// ═══════════════════════════════════════════════════════════════
String askOpenAI(const String& user_text) {
  ensureWiFi();

  if (conv_len < MAX_HISTORY) {
    conv_user[conv_len] = user_text;
  } else {
    for (int i = 1; i < MAX_HISTORY; i++) {
      conv_user[i-1] = conv_user[i]; conv_model[i-1] = conv_model[i];
    }
    conv_user[MAX_HISTORY-1] = user_text;
  }

  DynamicJsonDocument doc(8192);
  doc["model"] = "gpt-3.5-turbo";
  doc["max_tokens"] = 120;
  doc["temperature"] = 0.8;
  JsonArray messages = doc.createNestedArray("messages");

  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = "You are BMO, a small friendly game console robot from Adventure Time. "
                   "Fluent in English and Arabic. "
                   "RULE: Reply in the same language the user uses. "
                   "Keep replies under 60 words. Be warm, cheerful. "
                   "Call the user 'friend' or 'يا صديقي' in Arabic.";

  int hist = min((int)conv_len, MAX_HISTORY - 1);
  for (int i = 0; i < hist; i++) {
    JsonObject u = messages.createNestedObject(); u["role"]="user";      u["content"]=conv_user[i];
    JsonObject m = messages.createNestedObject(); m["role"]="assistant"; m["content"]=conv_model[i];
  }
  JsonObject cur = messages.createNestedObject(); cur["role"]="user"; cur["content"]=user_text;

  String body; serializeJson(doc, body);
  WiFiClientSecure client; client.setInsecure();
  if (!client.connect(OPENAI_HOST, 443)) { Serial.println("❌ GPT connect"); return ""; }

  client.print("POST " + String(LLM_PATH) + " HTTP/1.1\r\n");
  client.print("Host: " + String(OPENAI_HOST) + "\r\n");
  client.print("Authorization: Bearer " + String(OPENAI_API_KEY) + "\r\n");
  client.println("Content-Type: application/json");
  client.printf("Content-Length: %d\r\n", body.length());
  client.println("Connection: close\r\n");
  client.print(body);

  String response = readHTTPResponse(client);
  client.stop();

  DynamicJsonDocument rdoc(4096);
  if (deserializeJson(rdoc, response)) return "";
  String reply = rdoc["choices"][0]["message"]["content"].as<String>();
  reply.trim();

  if (conv_len < MAX_HISTORY) conv_model[conv_len++] = reply;
  else conv_model[MAX_HISTORY-1] = reply;
  return reply;
}

// ═══════════════════════════════════════════════════════════════
//  OPENAI TTS
// ═══════════════════════════════════════════════════════════════
size_t openAITTS(const String& text) {
  ensureWiFi();
  DynamicJsonDocument doc(2048);
  doc["model"]           = "tts-1-hd";   // ← HD model for clean audio
  doc["input"]           = text;
  doc["voice"]           = "nova";        // lighter, BMO-appropriate pitch
  doc["response_format"] = "pcm";         // raw 24kHz 16-bit mono PCM
  String body; serializeJson(doc, body);

  WiFiClientSecure client; client.setInsecure();
  if (!client.connect(OPENAI_HOST, 443)) { Serial.println("❌ TTS connect"); return 0; }

  client.print("POST " + String(TTS_PATH) + " HTTP/1.1\r\n");
  client.print("Host: " + String(OPENAI_HOST) + "\r\n");
  client.print("Authorization: Bearer " + String(OPENAI_API_KEY) + "\r\n");
  client.println("Content-Type: application/json");
  client.printf("Content-Length: %d\r\n", body.length());
  client.println("Connection: close\r\n");
  client.print(body);

  size_t bytes = readBinaryHTTPResponse(client, (uint8_t*)psram_buffer, BUFFER_BYTES);
  client.stop();
  return bytes;
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
String readHTTPResponse(WiFiClientSecure& client) {
  unsigned long timeout = millis();
  while (!client.available() && millis() - timeout < 10000) delay(10);

  bool chunked = false; String line;
  while (client.connected() || client.available()) {
    line = client.readStringUntil('\n'); line.trim();
    if (line.equalsIgnoreCase("transfer-encoding: chunked")) chunked = true;
    if (line.length() == 0) break;
  }
  String body = "";
  if (chunked) {
    while (client.connected() || client.available()) {
      String sl = client.readStringUntil('\n'); sl.trim();
      if (!sl.length()) continue;
      long cs = strtol(sl.c_str(), nullptr, 16);
      if (!cs) break;
      unsigned long t = millis();
      while ((long)client.available() < cs && millis()-t < 5000) delay(1);
      for (long i=0; i<cs; i++) if (client.available()) body += (char)client.read();
      client.readStringUntil('\n');
    }
  } else {
    while (client.connected() || client.available()) {
      if (client.available()) body += client.readString();
      delay(1);
    }
  }
  body.trim(); return body;
}

size_t readBinaryHTTPResponse(WiFiClientSecure& client, uint8_t* buffer, size_t max_len) {
  unsigned long timeout = millis();
  while (!client.available() && millis() - timeout < 10000) delay(10);

  bool chunked = false; String line;
  while (client.connected() || client.available()) {
    line = client.readStringUntil('\n'); line.trim();
    if (line.equalsIgnoreCase("transfer-encoding: chunked")) chunked = true;
    if (line.length() == 0) break;
  }
  size_t total = 0;
  if (chunked) {
    while (client.connected() || client.available()) {
      String sl = client.readStringUntil('\n'); sl.trim();
      if (!sl.length()) continue;
      long cs = strtol(sl.c_str(), nullptr, 16);
      if (cs <= 0) break;
      size_t btr = cs;
      while (btr > 0 && (client.connected() || client.available())) {
        int av = client.available();
        if (av > 0) {
          int tr = min((int)av,(int)btr);
          if (total+tr <= max_len) { client.read(buffer+total,tr); total+=tr; }
          else { for(int i=0;i<tr;i++) client.read(); }
          btr -= tr;
        } else delay(1);
      }
      client.readStringUntil('\n');
    }
  } else {
    while (client.connected() || client.available()) {
      int av = client.available();
      if (av > 0) {
        if (total+av <= max_len) { client.read(buffer+total,av); total+=av; }
        else { int s=max_len-total; if(s>0){client.read(buffer+total,s);total+=s;} client.read(); }
      }
    }
  }
  return total;
}
