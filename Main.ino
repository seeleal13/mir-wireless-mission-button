#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>

// ================= COLORS =================
#define BLACK    0x0000
#define WHITE    0xFFFF
#define GREEN    0x07E0
#define YELLOW   0xFFE0
#define RED      0xF800
#define BLUE     0x001F
#define DARKGRAY 0x4208

// ================= SD =================
#define SD_CS    5
#define SD_SCK  18
#define SD_MISO 19
#define SD_MOSI 23
SPIClass sd_spi(HSPI);

// ================= TFT =================
#define TFT_DC   2
#define TFT_CS   15
#define TFT_SCK  14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_BL   21

Arduino_DataBus *bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX    *gfx = new Arduino_ILI9341(bus);

// ================= BUTTONS =================
#define BTN_DOWN   22
#define BTN_SELECT 27

// ================= CONFIG =================
char ssid[64];
char pass[64];
char mir_ip[64];
char auth[256];

// ================= MISSIONS =================
#define MAX_MISSIONS 8

struct Mission {
  char name[32];
  char guid[64];
};

Mission missions[MAX_MISSIONS];
int missionCount = 0;
int selected     = 0;

// ================= QUEUE =================
// Each slot stores the mission index and its priority (0=normal, 1=high).
#define QSIZE 8

struct QEntry {
  int missionIdx;
  int priority;   // 0 = normal, 1 = high
};

QEntry q[QSIZE];
int    qh = 0, qt = 0;

// ================= LED STATE =================
// Three horizontal circles between title and mission list
// Laid out at y=42: RED at x=60, YELLOW at x=120, GREEN at x=180
enum LedState { LED_NONE, LED_RED, LED_YELLOW, LED_GREEN };
LedState currentLed = LED_NONE;

#define LED_Y       42
#define LED_R_X     60
#define LED_Y_X    120
#define LED_G_X    180
#define LED_RADIUS  12

// ================= MISSION TRACKING STATE MACHINE =================
enum TrackState {
  TRACK_IDLE,
  TRACK_POLLING
};

TrackState  trackState        = TRACK_IDLE;
int         activeMissionIdx  = -1;
int         activePriority    = 0;    // priority of the in-flight mission
char        activeQueueId[64] = {0};
unsigned long lastPollMs     = 0;
#define POLL_INTERVAL_MS  2000

// ================= STATUS TEXT =================
String statusText = "Idle";

// ================= HELPERS: minimal JSON field extractor =================
String jsonGetString(const String &json, const String &key) {
  // Try string value: "key":"value"
  String needle = "\"" + key + "\":\"";
  int idx = json.indexOf(needle);
  if (idx >= 0) {
    int start = idx + needle.length();
    int end   = json.indexOf('"', start);
    if (end > start) return json.substring(start, end);
  }
  // Try with space: "key": "value"
  needle = "\"" + key + "\": \"";
  idx = json.indexOf(needle);
  if (idx >= 0) {
    int start = idx + needle.length();
    int end   = json.indexOf('"', start);
    if (end > start) return json.substring(start, end);
  }
  // Try numeric / bare value: "key":value
  needle = "\"" + key + "\":";
  idx = json.indexOf(needle);
  if (idx >= 0) {
    int start = idx + needle.length();
    while (start < (int)json.length() && json[start] == ' ') start++;
    int end = start;
    while (end < (int)json.length() && json[end] != ',' && json[end] != '}') end++;
    return json.substring(start, end);
  }
  return "";
}

// ================= LOAD CONFIG =================
void loadConfig() {
  File f = SD.open("/config.txt");
  if (!f) return;

  while (f.available()) {
    String l = f.readStringUntil('\n');
    l.trim();
    l.replace("\r", "");
    if (l.length() == 0 || l.startsWith("#")) continue;

    if (l.startsWith("wifi_ssid=")) {
      String v = l.substring(10); v.trim();
      strncpy(ssid, v.c_str(), sizeof(ssid) - 1);
    }
    else if (l.startsWith("wifi_password=")) {
      String v = l.substring(14); v.trim();
      strncpy(pass, v.c_str(), sizeof(pass) - 1);
    }
    else if (l.startsWith("robot_ip=")) {
      String v = l.substring(9); v.trim();
      strncpy(mir_ip, v.c_str(), sizeof(mir_ip) - 1);
    }
    else if (l.startsWith("auth_token=")) {
      String v = l.substring(11); v.trim();
      strncpy(auth, v.c_str(), sizeof(auth) - 1);
    }
    else if (l.startsWith("mission=") && missionCount < MAX_MISSIONS) {
      String v = l.substring(8);
      int    c = v.indexOf(',');
      String n = v.substring(0, c); n.trim();
      String g = v.substring(c + 1); g.trim();
      strncpy(missions[missionCount].name, n.c_str(), 31);
      strncpy(missions[missionCount].guid, g.c_str(), 63);
      missionCount++;
    }
  }
  f.close();
}

// ================= LED DRAW =================
// Mirrors the clean setLED(r, y, g) logic from the working code,
// adapted to on-screen circles. Only redraws when state changes.
void forcePaintLeds() {
  auto circle = [&](int cx, int cy, LedState which, uint16_t activeColor) {
    uint16_t col = (currentLed == which) ? activeColor : DARKGRAY;
    gfx->fillCircle(cx, cy, LED_RADIUS, col);
    gfx->drawCircle(cx, cy, LED_RADIUS, WHITE);
  };
  circle(LED_R_X, LED_Y, LED_RED,    RED);
  circle(LED_Y_X, LED_Y, LED_YELLOW, YELLOW);
  circle(LED_G_X, LED_Y, LED_GREEN,  GREEN);
}

void drawLed(LedState next) {
  if (next == currentLed) return;   // no change → no flicker
  currentLed = next;
  forcePaintLeds();
}

// ================= UI =================
void drawUI() {
  gfx->fillScreen(BLACK);

  // Title — full width, centred vertically above LED row
  gfx->setTextSize(2);
  gfx->setTextColor(WHITE);
  gfx->setCursor(10, 10);
  gfx->print("MiR Controller");

  // Horizontal LED circles (repainted below)
  forcePaintLeds();

  // Mission list starts below the LED row
  for (int i = 0; i < missionCount; i++) {
    gfx->setCursor(10, 65 + i * 30);
    gfx->setTextColor(i == selected ? BLUE : GREEN);
    char buf[12];
    strncpy(buf, missions[i].name, 11);
    buf[11] = '\0';
    gfx->print(buf);
  }

  // Status line at bottom
  gfx->setTextColor(YELLOW);
  gfx->setCursor(10, 200);
  gfx->print(statusText);
}

// ================= STATUS TEXT (no full redraw) =================
void updateStatus(const String &msg) {
  gfx->fillRect(0, 193, 240, 24, BLACK);
  statusText = msg;
  gfx->setTextSize(2);
  gfx->setTextColor(YELLOW);
  gfx->setCursor(10, 197);
  gfx->print(statusText);
}

// ================= WIFI =================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  updateStatus("WiFi connecting...");
  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t++ < 60) {
    delay(333);
  }
  updateStatus((WiFi.status() == WL_CONNECTED) ? "WiFi OK" : "WiFi FAIL");
}

// ================= MIR API =================
String queueURL() {
  return String("http://") + mir_ip + "/api/v2.0.0/mission_queue";
}

String queueItemURL(const char *id) {
  return String("http://") + mir_ip + "/api/v2.0.0/mission_queue/" + id;
}

// POST a mission; returns the assigned queue entry ID, or "" on failure.
// priority: 0 = normal, 1 = high (same values as reference sendMission() code)
String postMission(const char *guid, int priority) {
  HTTPClient http;
  http.begin(queueURL());
  http.addHeader("Content-Type",    "application/json");
  http.addHeader("Accept-Language", "en_US");
  http.addHeader("Authorization",   auth);

  String payload =
    "{\"mission_id\":\"" + String(guid) + "\","
    "\"parameters\":[],"
    "\"priority\":" + String(priority) + "}";

  int    code = http.POST(payload);
  String body = http.getString();
  http.end();

  Serial.println("=== SEND MISSION ===");
  Serial.print("Priority: "); Serial.println(priority);
  Serial.println(code);
  Serial.println(body.substring(0, 120));

  if (code < 200 || code >= 300) return "";

  String id = jsonGetString(body, "id");
  id.trim();
  return id;
}

// GET current state of a queued mission entry.
// Returns "Pending", "Executing", "Done", "Aborted", "Failed", or "" on error.
String pollMissionState(const char *id) {
  HTTPClient http;
  http.begin(queueItemURL(id));
  http.addHeader("Authorization",  auth);
  http.addHeader("Accept-Language", "en_US");
  http.addHeader("Accept",          "application/json");

  int    code = http.GET();
  String body = http.getString();
  http.end();

  Serial.print("[POLL] id="); Serial.print(id);
  Serial.print(" code=");     Serial.print(code);
  Serial.print(" body=");     Serial.println(body.substring(0, 120));

  if (code < 200 || code >= 300) return "";

  String s = jsonGetString(body, "state");
  s.trim();
  return s;
}

// ================= SOFT QUEUE =================
void enqueue(int missionIdx, int priority) {
  int n = (qt + 1) % QSIZE;
  if (n == qh) return;   // full
  q[qt] = { missionIdx, priority };
  qt = n;
}

bool dequeue(int &missionIdx, int &priority) {
  if (qh == qt) return false;   // empty
  missionIdx = q[qh].missionIdx;
  priority   = q[qh].priority;
  qh = (qh + 1) % QSIZE;
  return true;
}

// ================= NON-BLOCKING ENGINE =================
// LED logic mirrors the working single-button code exactly:
//   - All off (LED_NONE) when idle
//   - YELLOW while sending / pending / executing
//   - GREEN  on Done
//   - RED    on any failure (POST fail, Aborted, Failed, parse error)
void processQueue() {
  switch (trackState) {

    // ---- IDLE: pull next job and POST it -------------------------
    case TRACK_IDLE: {
      int idx, prio;
      if (!dequeue(idx, prio)) return;

      activeMissionIdx = idx;
      activePriority   = prio;

      // All LEDs off while we attempt the POST
      drawLed(LED_NONE);

      String id = "";
      for (int k = 0; k < 3 && id.isEmpty(); k++) {
        id = postMission(missions[idx].guid, prio);
        if (id.isEmpty()) delay(400);
      }

      if (id.isEmpty()) {
        // POST failed → RED, same as working code's sendMission() failure path
        drawLed(LED_RED);
        activeMissionIdx = -1;
        return;
      }

      // Mission accepted → YELLOW (same as working code after successful POST)
      strncpy(activeQueueId, id.c_str(), sizeof(activeQueueId) - 1);
      activeQueueId[sizeof(activeQueueId) - 1] = '\0';

      drawLed(LED_YELLOW);
      lastPollMs = millis() - POLL_INTERVAL_MS;   // poll immediately next tick
      trackState = TRACK_POLLING;
      break;
    }

    // ---- POLLING: check mission state every POLL_INTERVAL_MS ----
    case TRACK_POLLING: {
      if (millis() - lastPollMs < POLL_INTERVAL_MS) return;
      lastPollMs = millis();

      String state = pollMissionState(activeQueueId);

      if (state == "Pending" || state == "Executing") {
        // Still in progress → YELLOW (same as working code)
        drawLed(LED_YELLOW);
      }
      else if (state == "Done") {
        // Complete → GREEN, stop polling (same as working code)
        drawLed(LED_GREEN);
        activeMissionIdx = -1;
        trackState = TRACK_IDLE;
      }
      else if (state == "Aborted" || state == "Failed") {
        // Terminal failure → RED, stop polling (same as working code)
        drawLed(LED_RED);
        activeMissionIdx = -1;
        trackState = TRACK_IDLE;
      }
      else {
        // Empty string (HTTP error) or unknown state → RED (same as working code)
        drawLed(LED_RED);
        activeMissionIdx = -1;
        trackState = TRACK_IDLE;
      }
      break;
    }

    default:
      trackState = TRACK_IDLE;
      break;
  }
}

// ================= INPUT =================
// BTN_SELECT behaviour (mirrors reference code's priority parameter):
//   Short press  (<600 ms) → enqueue at priority 0 (normal)
//   Long press   (≥600 ms) → enqueue at priority 1 (high)
//
// A small "!" indicator flashes on screen during the hold so the user
// knows the long-press threshold has been crossed, then clears on release.

#define LONG_PRESS_MS  600   // hold duration to trigger high priority

void handleButtons() {
  static bool          lastD          = HIGH;
  static bool          lastS          = HIGH;
  static unsigned long sPressedAt     = 0;     // millis() when SELECT went LOW
  static bool          sLongFired     = false; // indicator already shown this press

  bool d = digitalRead(BTN_DOWN);
  bool s = digitalRead(BTN_SELECT);

  // ---- DOWN button: cycle selection on falling edge ----
  if (lastD == HIGH && d == LOW) {
    selected = (selected + 1) % missionCount;
    gfx->fillRect(0, 60, 240, missionCount * 30 + 5, BLACK);
    gfx->setTextSize(2);
    for (int i = 0; i < missionCount; i++) {
      gfx->setCursor(10, 65 + i * 30);
      gfx->setTextColor(i == selected ? BLUE : GREEN);
      char buf[12];
      strncpy(buf, missions[i].name, 11);
      buf[11] = '\0';
      gfx->print(buf);
    }
  }

  // ---- SELECT button: falling edge → start timing ----
  if (lastS == HIGH && s == LOW) {
    sPressedAt = millis();
    sLongFired = false;
  }

  // ---- SELECT held: show "!" indicator once threshold is crossed ----
  if (s == LOW && !sLongFired && (millis() - sPressedAt >= LONG_PRESS_MS)) {
    sLongFired = true;
    // Draw a small "!" at top-right to signal high-priority mode
    gfx->setTextSize(2);
    gfx->setTextColor(RED);
    gfx->setCursor(220, 10);
    gfx->print("!");
  }

  // ---- SELECT released: rising edge → decide short or long press ----
  if (lastS == LOW && s == HIGH) {
    unsigned long held = millis() - sPressedAt;

    // Clear the "!" indicator regardless
    gfx->fillRect(218, 8, 22, 20, BLACK);

    if (held >= LONG_PRESS_MS) {
      // Long press → high priority (priority=1, same as reference sendMission(mission, 1))
      enqueue(selected, 1);
      Serial.println("[BTN] Long press → HIGH priority");
    } else {
      // Short press → normal priority (priority=0, same as reference sendMission(mission, 0))
      enqueue(selected, 0);
      Serial.println("[BTN] Short press → normal priority");
    }
    sLongFired = false;
  }

  lastD = d;
  lastS = s;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(BTN_DOWN,   INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(TFT_BL,     OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  gfx->begin();
  gfx->setRotation(0);

  sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  SD.begin(SD_CS, sd_spi);
  loadConfig();

  drawUI();
  connectWiFi();
  drawUI();
}

// ================= LOOP =================
void loop() {
  handleButtons();
  processQueue();
  delay(10);
}