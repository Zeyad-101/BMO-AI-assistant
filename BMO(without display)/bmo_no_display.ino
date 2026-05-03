// ╔══════════════════════════════════════════════════════════════╗
// ║                    BMO  –  AI Assistant                      ║
// ║            ESP32-S3 + MAX98357A (Speaker Only)               ║
// ║   Input: Serial Monitor (Text)                               ║
// ║   LLM: OpenAI GPT-3.5-turbo / GPT-4o-mini                    ║
// ║   TTS: OpenAI TTS-1 (PCM Stream)                             ║
// ╚══════════════════════════════════════════════════════════════╝

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include "secrets.h" // Ensure OPENAI_API_KEY is in here

// ─────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────
// MAX98357A amplifier
#define I2S_AMP_BCLK  38
#define I2S_AMP_LRC   39
#define I2S_AMP_DIN   40

// ─────────────────────────────────────────
//  AUDIO SETTINGS
// ─────────────────────────────────────────
#define PLAYBACK_SAMPLE_RATE  24000     // 24 kHz for OpenAI TTS PCM
#define BYTES_PER_SAMPLE      2         // 16-bit PCM

// Allocate a huge 1MB buffer in PSRAM (approx 21 seconds of 24kHz audio)
#define BUFFER_BYTES          (1024 * 1024)

// I2S port IDs
#define I2S_AMP_PORT  I2S_NUM_1

// ─────────────────────────────────────────
//  API ENDPOINTS
// ─────────────────────────────────────────
#define OPENAI_HOST "api.openai.com"
#define LLM_PATH    "/v1/chat/completions"
#define TTS_PATH    "/v1/audio/speech"

// ─────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────
int16_t* psram_buffer = nullptr;   // lives in PSRAM

// Conversation history for GPT (keeps context across turns)
#define MAX_HISTORY 10
String conv_user[MAX_HISTORY];
String conv_model[MAX_HISTORY];
int    conv_len = 0;

// ==========================================
// PROTOTYPES
// ==========================================
void initAmp();
void connectWiFi();
void ensureWiFi();
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

  // ── PSRAM buffer ──────────────────────────────────────────────
  if (psramInit()) {
    psram_buffer = (int16_t*) ps_malloc(BUFFER_BYTES);
    if (!psram_buffer) {
      Serial.println("❌ PSRAM allocation failed! Check PSRAM mode.");
      while (true) delay(1000);
    }
    Serial.printf("✅ PSRAM buffer allocated: %d KB\n", BUFFER_BYTES / 1024);
  } else {
    Serial.println("❌ PSRAM NOT FOUND! Code will crash without PSRAM.");
    while (true) delay(1000);
  }

  // ── I2S peripherals ───────────────────────────────────────────
  initAmp();

  // ── WiFi ──────────────────────────────────────────────────────
  connectWiFi();

  Serial.println("\n✅ BMO ready! Type a message in the Serial Monitor and press Enter.");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  // Listen for text input from the Serial Monitor
  if (Serial.available() > 0) {
    String userInput = Serial.readStringUntil('\n');
    userInput.trim();

    if (userInput.length() > 0) {
      Serial.println("\n📝 You typed: " + userInput);

      Serial.println("🤖 Asking BMO (GPT)...");
      String reply = askOpenAI(userInput);

      if (reply.length() == 0) {
        Serial.println("⚠️  GPT returned empty.");
        return;
      }
      Serial.println("💬 BMO: " + reply);

      Serial.println("🔊 Synthesising speech (OpenAI TTS)...");
      size_t tts_bytes = openAITTS(reply);

      if (tts_bytes == 0) {
        Serial.println("⚠️  TTS returned no audio.");
        return;
      }

      Serial.printf("▶️  Playing %d KB of audio...\n", tts_bytes / 1024);
      playAudio(tts_bytes);

      Serial.println("\n✅ BMO ready! Type another message...");
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  I2S INIT
// ═══════════════════════════════════════════════════════════════
// ═══════════════════════════════════════════════════════════════
//  I2S INIT
// ═══════════════════════════════════════════════════════════════
void initAmp() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = PLAYBACK_SAMPLE_RATE,     // 24000 Hz
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    
    // --- THE SPEED FIX ---
    // Changed from RIGHT_LEFT (Stereo) to ONLY_LEFT (Mono)
    // OpenAI sends Mono audio. If set to Stereo, it plays twice as fast!
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT, 
    // ---------------------
    
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,

    // --- INCREASE BUFFERS FOR SMOOTH PLAYBACK ---
    .dma_buf_count        = 16,    // Increased from 8
    .dma_buf_len          = 1024,  // Increased from 256
    // --------------------------------------------
    
    .use_apll             = true,  // Change to true for a highly accurate audio clock
    .tx_desc_auto_clear   = true,
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
  Serial.println("✅ Amplifier I2S ready");
}

// ═══════════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.printf("📶 Connecting to %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(1000);
  
  // --- THE BROWNOUT FIX ---
  // Lower the WiFi transmit power to prevent USB power spikes
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  // ------------------------

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) {
      Serial.println("\n\n❌ WiFi FAILED.");
      Serial.print("Internal Error Code: ");
      Serial.println(WiFi.status());
      return; 
    }
    delay(500); 
    Serial.print(".");
  }
  Serial.printf("\n✅ WiFi connected: %s\n", WiFi.localIP().toString().c_str());
}
void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️  WiFi lost – reconnecting...");
    WiFi.reconnect();
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(500);
  }
}
// ═══════════════════════════════════════════════════════════════
//  PLAY AUDIO
// ═══════════════════════════════════════════════════════════════
void playAudio(size_t byte_count) {
  i2s_zero_dma_buffer(I2S_AMP_PORT);
  
  size_t samples = byte_count / BYTES_PER_SAMPLE;
  size_t bytes_written;
  const size_t chunk = 512;

  for (size_t i = 0; i < samples; i += chunk / BYTES_PER_SAMPLE) {
    size_t to_write = min((size_t)chunk, (size_t)((samples - i) * BYTES_PER_SAMPLE));
    i2s_write(I2S_AMP_PORT, &psram_buffer[i], to_write, &bytes_written, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(I2S_AMP_PORT);
}

// ═══════════════════════════════════════════════════════════════
//  OPENAI LLM (GPT Chat Completions)
// ═══════════════════════════════════════════════════════════════
String askOpenAI(const String& user_text) {
  ensureWiFi();

  // Save to history
  if (conv_len < MAX_HISTORY) {
    conv_user[conv_len] = user_text;
  } else {
    for (int i = 1; i < MAX_HISTORY; i++) {
      conv_user[i - 1]  = conv_user[i];
      conv_model[i - 1] = conv_model[i];
    }
    conv_user[MAX_HISTORY - 1] = user_text;
  }

  DynamicJsonDocument doc(8192);
  doc["model"] = "gpt-3.5-turbo"; // Switch to "gpt-4o-mini" if you prefer
  doc["max_tokens"] = 120;
  doc["temperature"] = 0.8;

  JsonArray messages = doc.createNestedArray("messages");
  
 JsonObject sysMsg = messages.createNestedObject();
  sysMsg["role"] = "system";
  
  // --- THE BILINGUAL UPDATE ---
  sysMsg["content"] = "You are BMO, a small friendly game console robot from Adventure Time. "
                      "You are fluent in both English and Arabic. "
                      "CRITICAL RULE: You must respond in the exact same language the user uses. If they type in Arabic, reply in Arabic. If English, reply in English. "
                      "Keep all responses under 60 words, be warm and cheerful, and call the user 'friend' (or 'يا صديقي' in Arabic).";
  // ----------------------------
  // Conversation history
  int hist_count = min((int)conv_len, (int)(MAX_HISTORY - 1));
  for (int i = 0; i < hist_count; i++) {
    JsonObject u = messages.createNestedObject();
    u["role"] = "user";
    u["content"] = conv_user[i];

    JsonObject m = messages.createNestedObject();
    m["role"] = "assistant";
    m["content"] = conv_model[i];
  }

  // Current message
  JsonObject cur = messages.createNestedObject();
  cur["role"] = "user";
  cur["content"] = user_text;

  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(OPENAI_HOST, 443)) {
    Serial.println("❌ GPT: connection failed");
    return "";
  }

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
  DeserializationError err = deserializeJson(rdoc, response);
  if (err) return "";

  String reply = rdoc["choices"][0]["message"]["content"].as<String>();
  reply.trim();

  // Save model reply to history
  if (conv_len < MAX_HISTORY) {
    conv_model[conv_len++] = reply;
  } else {
    conv_model[MAX_HISTORY - 1] = reply;
  }

  return reply;
}

// ═══════════════════════════════════════════════════════════════
//  OPENAI TTS (Streams RAW PCM directly into PSRAM)
// ═══════════════════════════════════════════════════════════════
size_t openAITTS(const String& text) {
  ensureWiFi();
  
 DynamicJsonDocument doc(2048);
  // --- UPGRADE TO HD AUDIO ---
  doc["model"] = "tts-1-hd"; 
  // ---------------------------
  doc["input"] = text;
  doc["voice"] = "alloy"; 
  doc["response_format"] = "pcm";
  String body;
  serializeJson(doc, body);

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(OPENAI_HOST, 443)) {
    Serial.println("❌ TTS: connection failed");
    return 0;
  }

  client.print("POST " + String(TTS_PATH) + " HTTP/1.1\r\n");
  client.print("Host: " + String(OPENAI_HOST) + "\r\n");
  client.print("Authorization: Bearer " + String(OPENAI_API_KEY) + "\r\n");
  client.println("Content-Type: application/json");
  client.printf("Content-Length: %d\r\n", body.length());
  client.println("Connection: close\r\n");
  client.print(body);

  // Directly download binary payload into our massive PSRAM buffer
  size_t bytes_downloaded = readBinaryHTTPResponse(client, (uint8_t*)psram_buffer, BUFFER_BYTES);
  client.stop();
  
  return bytes_downloaded;
}

// ═══════════════════════════════════════════════════════════════
//  HELPERS
// ═══════════════════════════════════════════════════════════════
// Reads String bodies (for JSON responses)
String readHTTPResponse(WiFiClientSecure& client) {
  unsigned long timeout = millis();
  while (!client.available() && millis() - timeout < 10000) delay(10);

  bool chunked = false;
  String line;
  while (client.connected() || client.available()) {
    line = client.readStringUntil('\n'); line.trim();
    if (line.equalsIgnoreCase("transfer-encoding: chunked")) chunked = true;
    if (line.length() == 0) break; 
  }

  String body = "";
  if (chunked) {
    while (client.connected() || client.available()) {
      String size_line = client.readStringUntil('\n'); size_line.trim();
      if (size_line.length() == 0) continue;
      long chunk_size = strtol(size_line.c_str(), nullptr, 16);
      if (chunk_size == 0) break;
      unsigned long t = millis();
      while ((long)client.available() < chunk_size && millis() - t < 5000) delay(1);
      for (long i = 0; i < chunk_size; i++) if (client.available()) body += (char)client.read();
      client.readStringUntil('\n'); 
    }
  } else {
    while (client.connected() || client.available()) {
      if (client.available()) body += client.readString();
      delay(1);
    }
  }
  body.trim();
  return body;
}

// Reads pure Binary bodies directly into memory (Crucial for Audio streaming!)
size_t readBinaryHTTPResponse(WiFiClientSecure& client, uint8_t* buffer, size_t max_len) {
  unsigned long timeout = millis();
  while (!client.available() && millis() - timeout < 10000) delay(10);

  bool chunked = false;
  String line;
  while (client.connected() || client.available()) {
    line = client.readStringUntil('\n'); line.trim();
    if (line.equalsIgnoreCase("transfer-encoding: chunked")) chunked = true;
    if (line.length() == 0) break; 
  }

  size_t total_bytes = 0;
  if (chunked) {
    while (client.connected() || client.available()) {
      String size_line = client.readStringUntil('\n'); size_line.trim();
      if (size_line.length() == 0) continue;
      long chunk_size = strtol(size_line.c_str(), nullptr, 16);
      if (chunk_size <= 0) break;

      size_t bytes_to_read = chunk_size;
      while (bytes_to_read > 0 && (client.connected() || client.available())) {
        int available = client.available();
        if (available > 0) {
          int to_read = min((int)available, (int)bytes_to_read);
          if (total_bytes + to_read <= max_len) {
            client.read(buffer + total_bytes, to_read);
            total_bytes += to_read;
          } else {
            for (int i=0; i<to_read; i++) client.read(); // Drop excess if buffer is full
          }
          bytes_to_read -= to_read;
        } else {
          delay(1);
        }
      }
      client.readStringUntil('\n'); 
    }
  } else {
    while (client.connected() || client.available()) {
      int available = client.available();
      if (available > 0) {
        if (total_bytes + available <= max_len) {
          client.read(buffer + total_bytes, available);
          total_bytes += available;
        } else {
           int safe = max_len - total_bytes;
           if (safe > 0) { client.read(buffer + total_bytes, safe); total_bytes += safe; }
           client.read(); // Burn rest
        }
      }
    }
  }
  return total_bytes;
}
