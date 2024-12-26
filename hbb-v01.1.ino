#include <esp_now.h>
#include <WiFi.h>
#include "AudioTools.h"
#include "AudioLibs/AudioSourceSDFAT.h"
#include "AudioCodecs/CodecMP3Helix.h"
#include "esp_task_wdt.h"

#define ROLE_PIN 17

typedef struct {
  char command[32];
} struct_message;

struct_message myData;

uint8_t broadcastAddress[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const char* filePath = "/file.mp3";
const char* startFilePath = "/";
const char* ext = "mp3";

AudioSourceSDFAT source(startFilePath, ext);
I2SStream i2s;
MP3DecoderHelix decoder;
AudioPlayer player(source, i2s, decoder);

bool isMaster = false;
unsigned long previousMillis = 0;
const long delayDuration = 4000;
bool delayActive = false;

bool shouldPlay = false;
bool isPlaying = false;

void printMetaData(MetaDataType type, const char* str, int len) {
  // Minimal metadata printing
}

void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  // Master send callback
}

void OnDataRecv(const esp_now_recv_info* info, const uint8_t* incomingData, int len) {
  memcpy(&myData, incomingData, sizeof(myData));

  if (strcmp(myData.command, "play") == 0) {
    shouldPlay = true;
    isPlaying = false;
  } else if (strcmp(myData.command, "stop") == 0) {
    shouldPlay = false;
    isPlaying = false;
    player.end();
    i2s.end(); // Ensure silence
  }
}

void addBroadcastPeer() {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
}

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  if (isMaster) {
    esp_now_register_send_cb(OnDataSent);
  } else {
    esp_now_register_recv_cb(OnDataRecv);
  }

  addBroadcastPeer();
}

void beginI2S() {
  auto config = i2s.defaultConfig(TX_MODE);
  config.bits_per_sample = 16;
  config.sample_rate = 44100;
  config.i2s_format = I2S_STD_FORMAT;
  config.is_master = true;
  config.port_no = 0;
  config.pin_ws = 15;
  config.pin_bck = 14;
  config.pin_data = 22;
  config.pin_mck = 0;
  config.use_apll = true;
  i2s.begin(config);
}

void setupWDT() {
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
}

void stopPlayback() {
  player.end();
  i2s.end(); // Completely disable I2S to ensure no leftover samples
  isPlaying = false;
}

bool startPlayback() {
  // Re-init I2S before starting playback
  beginI2S();
  source.begin();
  player.setPath(filePath);
  if (player.begin()) {
    isPlaying = true;
    return true;
  } else {
    i2s.end();
    isPlaying = false;
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ROLE_PIN, INPUT_PULLUP);
  isMaster = (digitalRead(ROLE_PIN) == LOW);
  Serial.println(isMaster ? "Acting as MASTER" : "Acting as SLAVE");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  setupEspNow();
  AudioLogger::instance().begin(Serial, AudioLogger::Info);

  // Initialize audio
  beginI2S();
  source.begin();
  player.setMetadataCallback(printMetaData);
  player.setSilenceOnInactive(true);
  player.setAutoNext(false);
  player.begin(0, false);
  player.end();
  i2s.end(); // Start silent

  setupWDT();

  if (isMaster) {
    // Master starts immediately
    startPlayback();
  } else {
    // Slave waits for "play"
    shouldPlay = false;
    isPlaying = false;
  }
}

void loopMaster() {
  unsigned long currentMillis = millis();

  if (isPlaying) {
    if (!player.copy()) {
      // Track ended
      stopPlayback();
      // Send "stop" command
      strcpy(myData.command, "stop");
      esp_now_send(broadcastAddress, (uint8_t*)&myData, sizeof(myData));
      previousMillis = currentMillis;
      delayActive = true;
    }
  } else {
    if (delayActive && (currentMillis - previousMillis >= delayDuration)) {
      delayActive = false;
      strcpy(myData.command, "play");
      esp_now_send(broadcastAddress, (uint8_t*)&myData, sizeof(myData));
      startPlayback();
    } else {
      delay(10);
    }
  }
}

void loopSlave() {
  if (isPlaying) {
    if (!player.copy()) {
      // Track ended and no new command
      stopPlayback();
      // Ensure silence after track ends with no new command
      shouldPlay = false; 
    }
  } else {
    // Not playing now
    if (shouldPlay && !isPlaying) {
      startPlayback();
    } else {
      // Remain silent
      delay(10);
    }
  }
}

void loop() {
  if (isMaster) {
    loopMaster();
  } else {
    loopSlave();
  }
  esp_task_wdt_reset();
}
