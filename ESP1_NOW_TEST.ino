#include <WiFi.h>
#include <esp_now.h>
#include "esp_wifi.h"

uint8_t esp2MAC[] = { 0x5C, 0x01, 0x3B, 0x69, 0x23, 0xE8 };

#define BUTTON_PIN 4

typedef struct {
  bool trigger;
} ButtonPacket;

bool          lastState     = HIGH;
unsigned long lastDebounce  = 0;
const int     debounceDelay = 250;

void sendTrigger() {
  ButtonPacket packet;
  packet.trigger = true;

  esp_err_t result = esp_now_send(esp2MAC, (uint8_t*)&packet, sizeof(packet));

  if (result == ESP_OK) {
    Serial.println("Trigger sent");
  } else {
    Serial.print("Send failed: ");
    Serial.println(result);
  }
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("Delivery: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP1 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, esp2MAC, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  peerInfo.ifidx   = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Add peer failed");
    return;
  }

  Serial.println("ESP1 ready");
}

void loop() {
  bool state = digitalRead(BUTTON_PIN);

  if (lastState == HIGH && state == LOW &&
      millis() - lastDebounce > debounceDelay) {
    lastDebounce = millis();
    sendTrigger();
  }

  lastState = state;
}