#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <Arduino_GFX_Library.h>
#include "esp_wifi.h"
#include <SPI.h>
#include <SD.h>

// ================= COLORS =================
#define BLACK   0x0000
#define BLUE    0x001F
#define WHITE   0xFFFF
#define GREEN   0x07E0
#define RED     0xF800
#define YELLOW  0xFFE0

// ================= WIFI / MIR =================
const char* ssid     = "La_Fibre_dOrange_F64D";
const char* password = "pbaXk9bk9UUJQXtsRa";

String MIR_URL = "http://192.168.11.110/api/v2.0.0/mission_queue";

const char* AUTH =
  "Basic ZGlzdHJpYnV0b3I6NjJmMmYwZjFlZmYxMGQzMTUyYzk1ZjZmMDU5NjU3NmU0ODJiYjhlNDQ4MDY0MzNmNGNmOTI5NzkyODM0YjAxNA==";

// ================= SD CARD =================
#define SD_CS    5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23

SPIClass sd_spi(VSPI);

// ================= DISPLAY =================
#define TFT_BL   21
#define TFT_DC    2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_RST  -1

Arduino_DataBus *bus =
  new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *gfx =
  new Arduino_ILI9341(bus, TFT_RST, 0);

// ================= BUTTONS =================
#define BTN_DOWN   22
#define BTN_SELECT 27

bool          lastDownState   = HIGH;
bool          lastSelectState = HIGH;
unsigned long lastDownTime    = 0;
unsigned long lastSelectTime  = 0;
const int     debounceDelay   = 300;

// ================= MISSIONS =================
#define MAX_MISSIONS 8

struct Mission {
  char name[32];
  char guid[64];
};

// ESP2 screen missions — loaded from SD
Mission missions[MAX_MISSIONS];
int     missionCount    = 0;
int     selectedMission = 0;

// ESP1 dedicated mission — only guid loaded from SD
Mission esp1Mission = { "ESP1 Default", "FALLBACK-ESP1-GUID-HERE" };

// ================= SD CONFIG LOADER =================
bool loadConfig() {
  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sd_spi)) {
    Serial.println("SD init failed");
    return false;
  }

  Serial.println("SD OK");

  File file = SD.open("/config.txt", "r");
  if (!file) {
    Serial.println("config.txt not found");
    return false;
  }

  missionCount = 0;

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();

    // Skip empty lines and comments
    if (line.length() == 0 || line.startsWith("#")) continue;

    // esp1_guid=XXXXXXXX
    if (line.startsWith("esp1_guid=")) {
      String guid = line.substring(10);
      guid.trim();
      strncpy(esp1Mission.guid, guid.c_str(), sizeof(esp1Mission.guid) - 1);
      Serial.print("ESP1 GUID loaded: ");
      Serial.println(esp1Mission.guid);
    }

    // mission=Name,GUID
    else if (line.startsWith("mission=") && missionCount < MAX_MISSIONS) {
      String val   = line.substring(8);
      int    comma = val.indexOf(',');

      if (comma > 0) {
        String name = val.substring(0, comma);
        String guid = val.substring(comma + 1);
        name.trim();
        guid.trim();

        strncpy(missions[missionCount].name, name.c_str(), sizeof(missions[0].name) - 1);
        strncpy(missions[missionCount].guid, guid.c_str(), sizeof(missions[0].guid) - 1);

        Serial.print("Mission loaded: ");
        Serial.print(missions[missionCount].name);
        Serial.print(" -> ");
        Serial.println(missions[missionCount].guid);

        missionCount++;
      }
    }
  }

  file.close();

  Serial.print("Total missions loaded: ");
  Serial.println(missionCount);

  return missionCount > 0;
}

// ================= QUEUE =================
#define QUEUE_SIZE 8
volatile int  triggerQueue[QUEUE_SIZE];
volatile int  qHead      = 0;
volatile int  qTail      = 0;
volatile bool processing = false;

void enqueue(int missionIndex) {
  int next = (qTail + 1) % QUEUE_SIZE;
  if (next == qHead) {
    Serial.println("Queue full, dropping trigger");
    return;
  }
  triggerQueue[qTail] = missionIndex;
  qTail = next;
  Serial.print("Queued mission: ");
  Serial.println(missions[missionIndex].name);
}

bool dequeue(int &missionIndex) {
  if (qHead == qTail) return false;
  missionIndex = triggerQueue[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return true;
}

// ================= ESP1 FLAG =================
volatile bool esp1Triggered = false;

// ================= STATUS =================
String statusText = "Ready";

// ================= UI =================
void drawUI() {
  gfx->fillScreen(BLACK);

  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(20, 15);
  gfx->print("MiR Missions");

  int pending = (qTail - qHead + QUEUE_SIZE) % QUEUE_SIZE;
  if (pending > 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(YELLOW);
    gfx->setCursor(170, 18);
    gfx->print("Q:");
    gfx->print(pending);
  }

  for (int i = 0; i < missionCount; i++) {
    if (i == selectedMission) {
      gfx->fillRect(10, 60 + (i * 40), 220, 30, BLUE);
      gfx->setTextColor(WHITE);
    } else {
      gfx->setTextColor(GREEN);
    }

    gfx->setTextSize(2);
    gfx->setCursor(20, 68 + (i * 40));
    gfx->print(i == selectedMission ? "> " : "  ");
    gfx->print(missions[i].name);
  }

  gfx->setTextColor(YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(20, 220);
  gfx->print("Status:");
  gfx->setCursor(20, 250);
  gfx->print("                ");
  gfx->setCursor(20, 250);
  gfx->print(statusText);
}

// ================= WIFI =================
void connectWiFi() {
  Serial.println("Connecting WiFi...");
  statusText = "Connecting...";
  drawUI();

  WiFi.begin(ssid, password);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    if (++retries > 30) {
      statusText = "WiFi FAILED";
      drawUI();
      Serial.println("WiFi failed");
      return;
    }
  }

  Serial.print("WiFi OK, channel: ");
  Serial.println(WiFi.channel());

  statusText = "WiFi OK";
  drawUI();
}

// ================= HTTP =================
bool sendMissionByGuid(const char* guid) {
  if (WiFi.status() != WL_CONNECTED) {
    statusText = "No WiFi";
    drawUI();
    return false;
  }

  HTTPClient http;
  http.begin(MIR_URL);
  http.addHeader("Content-Type",    "application/json");
  http.addHeader("Accept-Language", "en-US");
  http.addHeader("Authorization",   AUTH);

  String payload =
    "{"
      "\"mission_id\":\"" + String(guid) + "\","
      "\"parameters\":[],"
      "\"priority\":1"
    "}";

  int    code = http.POST(payload);
  String body = http.getString();

  Serial.print("HTTP ");
  Serial.print(code);
  Serial.print(" | ");
  Serial.println(body);

  http.end();
  return (code >= 200 && code < 300);
}

bool sendMission(int index) {
  return sendMissionByGuid(missions[index].guid);
}

// ================= TRIGGER =================
void triggerMission(int index) {
  Serial.print("Sending: ");
  Serial.println(missions[index].name);

  statusText = "Sending...";
  drawUI();

  bool success = false;
  for (int i = 0; i < 3; i++) {
    success = sendMission(index);
    if (success) break;
    Serial.println("Retry...");
    delay(500);
  }

  statusText = success ? "Accepted!" : "Failed!";
  Serial.println(success ? "Mission accepted!" : "Mission failed!");
  drawUI();
}

void triggerEsp1Mission() {
  Serial.print("ESP1 mission: ");
  Serial.println(esp1Mission.name);

  statusText = "ESP1 Sending...";
  drawUI();

  bool success = false;
  for (int i = 0; i < 3; i++) {
    success = sendMissionByGuid(esp1Mission.guid);
    if (success) break;
    Serial.println("Retry...");
    delay(500);
  }

  statusText = success ? "ESP1 OK!" : "ESP1 Failed!";
  Serial.println(success ? "ESP1 mission accepted!" : "ESP1 mission failed!");
  drawUI();
}

// ================= ESP-NOW =================
typedef struct {
  bool trigger;
} ButtonPacket;

void onDataRecv(const esp_now_recv_info_t* info,
                const uint8_t* data,
                int len) {

  Serial.println("ESP-NOW trigger received!");

  if (len < (int)sizeof(ButtonPacket)) return;

  ButtonPacket packet;
  memcpy(&packet, data, sizeof(packet));

  if (packet.trigger) {
    esp1Triggered = true;
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  gfx->begin();
  gfx->setRotation(0);

  // Show loading screen while reading SD
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 100);
  gfx->print("Loading config...");

  // Load missions from SD card
  if (!loadConfig()) {
    Serial.println("Config load failed — check SD card and config.txt");
    gfx->fillScreen(BLACK);
    gfx->setTextColor(RED);
    gfx->setTextSize(2);
    gfx->setCursor(20, 80);
    gfx->print("SD ERROR");
    gfx->setCursor(20, 110);
    gfx->print("Check SD card");
    gfx->setCursor(20, 140);
    gfx->print("& config.txt");
    delay(4000);   // pause so user can read the error, then continue with fallback
  }

  drawUI();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  Serial.print("ESP2 MAC: ");
  Serial.println(WiFi.macAddress());

  connectWiFi();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW FAIL");
    statusText = "ESP-NOW FAIL";
    drawUI();
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  Serial.println("ESP2 ready");
  statusText = "Ready";
  drawUI();
}

// ================= LOOP =================
void loop() {

  bool currentDown   = digitalRead(BTN_DOWN);
  bool currentSelect = digitalRead(BTN_SELECT);

  // DOWN — scroll mission list
  if (lastDownState == HIGH && currentDown == LOW &&
      millis() - lastDownTime > debounceDelay) {

    lastDownTime    = millis();
    selectedMission = (selectedMission + 1) % missionCount;
    Serial.print("Selected: ");
    Serial.println(missions[selectedMission].name);
    statusText = "Selected";
    drawUI();
  }

  // SELECT — send highlighted mission
  if (lastSelectState == HIGH && currentSelect == LOW &&
      millis() - lastSelectTime > debounceDelay) {

    lastSelectTime = millis();
    Serial.print("ESP2 button sending: ");
    Serial.println(missions[selectedMission].name);
    enqueue(selectedMission);
  }

  lastDownState   = currentDown;
  lastSelectState = currentSelect;

  // Handle ESP1 trigger safely from main loop
  if (esp1Triggered) {
    esp1Triggered = false;
    triggerEsp1Mission();
    drawUI();
  }

  // Process ESP2 queue
  if (!processing) {
    int nextMission;
    if (dequeue(nextMission)) {
      processing = true;
      triggerMission(nextMission);
      processing = false;
      drawUI();
    }
  }

  delay(10);
}