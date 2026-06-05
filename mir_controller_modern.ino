#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>

// ================= COLOR PALETTE (modern dark theme) =================
#define COL_BG        0x0841
#define COL_SURFACE   0x1082
#define COL_BORDER    0x2104
#define COL_ACCENT    0x04FF   // electric blue
#define COL_ACCENT2   0x07FF   // cyan
#define COL_WHITE     0xFFFF
#define COL_GRAY      0x7BEF
#define COL_DIMGRAY   0x39E7
#define COL_GREEN     0x07E0
#define COL_YELLOW    0xFE60
#define COL_RED       0xF800
#define COL_BLACK     0x0000
#define COL_DARKGRAY  0x4208

// Screen dimensions (ILI9341 portrait)
#define SCR_W 240
#define SCR_H 320

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

// ================= STRUCTS =================
struct PillInfo { uint16_t dot; const char* text; };

// ================= MISSIONS =================
#define MAX_MISSIONS 8
struct Mission { char name[32]; char guid[64]; };
Mission missions[MAX_MISSIONS];
int missionCount = 0;
int selected     = 0;

// Per-mission last known result: 0=none, 1=pending/running, 2=done, 3=failed
uint8_t missionStatus[MAX_MISSIONS];

// ================= QUEUE =================
#define QSIZE 8
struct QEntry { int missionIdx; int priority; };
QEntry q[QSIZE];
int    qh = 0, qt = 0;

// ================= LED / GLOBAL STATE =================
enum LedState { LED_NONE, LED_RED, LED_YELLOW, LED_GREEN };
LedState currentLed = LED_NONE;

// Three circles: layout
#define LED_AREA_Y   38     // top of the LED row
#define LED_AREA_H   24     // height of LED strip
#define LED_R_X      50
#define LED_Y_X     120
#define LED_G_X     190
#define LED_CY      (LED_AREA_Y + LED_AREA_H/2)
#define LED_RADIUS   8

// Status pill (top-right of header)
#define PILL_X  140
#define PILL_Y    7
#define PILL_W   90
#define PILL_H   18

// ================= MISSION TRACKING =================
enum TrackState { TRACK_IDLE, TRACK_POLLING };
TrackState    trackState       = TRACK_IDLE;
int           activeMissionIdx = -1;
int           activePriority   = 0;
char          activeQueueId[64]= {0};
unsigned long lastPollMs       = 0;
#define POLL_INTERVAL_MS 2000

// ================= STATUS TEXT =================
String statusText = "Idle";

// ================= TOAST (non-blocking) =================
// Toast is drawn and a timestamp recorded; it is erased in the main loop.
bool          toastVisible  = false;
unsigned long toastShownAt  = 0;
#define TOAST_DURATION_MS 900
int  toastX, toastY, toastW, toastH;

// ================================================
// ================= DRAW HELPERS =================
// ================================================

void drawHGradBar(int x, int y, int w, int h, uint16_t c1, uint16_t c2) {
  uint8_t r1=(c1>>11)&0x1F, g1=(c1>>5)&0x3F, b1=c1&0x1F;
  uint8_t r2=(c2>>11)&0x1F, g2=(c2>>5)&0x3F, b2=c2&0x1F;
  for (int i=0;i<w;i++){
    uint8_t r=r1+(int)(r2-r1)*i/w;
    uint8_t g=g1+(int)(g2-g1)*i/w;
    uint8_t b=b1+(int)(b2-b1)*i/w;
    gfx->drawFastVLine(x+i,y,h,(r<<11)|(g<<5)|b);
  }
}

// ================================================
// ================= STATUS PILL ==================
// ================================================
PillInfo pillFor(LedState s) {
  switch(s){
    case LED_RED:    return {COL_RED,    "FAULT  "};
    case LED_YELLOW: return {COL_YELLOW, "RUNNING"};
    case LED_GREEN:  return {COL_GREEN,  "DONE   "};
    default:         return {COL_DIMGRAY,"IDLE   "};
  }
}

void drawStatusPill(LedState s) {
  PillInfo p = pillFor(s);
  gfx->fillRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 9, COL_SURFACE);
  gfx->drawRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 9, p.dot);
  gfx->fillCircle(PILL_X+10, PILL_Y+PILL_H/2, 4, p.dot);
  gfx->setTextSize(1);
  gfx->setTextColor(p.dot);
  gfx->setCursor(PILL_X+18, PILL_Y+5);
  gfx->print(p.text);
}

// ================================================
// ================= 3 LED CIRCLES ================
// ================================================
void forcePaintLeds() {
  // Background strip for LED row
  gfx->fillRect(0, LED_AREA_Y, SCR_W, LED_AREA_H, COL_BG);

  // RED circle
  uint16_t rc = (currentLed==LED_RED)    ? COL_RED    : COL_DARKGRAY;
  gfx->fillCircle(LED_R_X, LED_CY, LED_RADIUS, rc);
  gfx->drawCircle(LED_R_X, LED_CY, LED_RADIUS, COL_WHITE);

  // YELLOW circle
  uint16_t yc = (currentLed==LED_YELLOW) ? COL_YELLOW : COL_DARKGRAY;
  gfx->fillCircle(LED_Y_X, LED_CY, LED_RADIUS, yc);
  gfx->drawCircle(LED_Y_X, LED_CY, LED_RADIUS, COL_WHITE);

  // GREEN circle
  uint16_t gc = (currentLed==LED_GREEN)  ? COL_GREEN  : COL_DARKGRAY;
  gfx->fillCircle(LED_G_X, LED_CY, LED_RADIUS, gc);
  gfx->drawCircle(LED_G_X, LED_CY, LED_RADIUS, COL_WHITE);
}

void drawLed(LedState next) {
  if (next == currentLed) return;
  currentLed = next;
  forcePaintLeds();
  drawStatusPill(next);   // keep pill in sync too
}

// ================================================
// ================= HEADER =======================
// ================================================
void drawHeader() {
  drawHGradBar(0, 0, SCR_W, 4, COL_ACCENT, COL_ACCENT2);
  gfx->fillRect(0, 4, SCR_W, 30, COL_SURFACE);
  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE);
  gfx->setCursor(10, 10);
  gfx->print("MiR");
  gfx->setTextColor(COL_ACCENT2);
  gfx->print(" CTL");
  drawStatusPill(currentLed);
  gfx->drawFastHLine(0, 34, SCR_W, COL_BORDER);
}

// ================================================
// ================= MISSION LIST =================
// ================================================
// Layout: LED row at y=38..62, list starts at y=66
#define LIST_TOP  66
#define ROW_H     28
#define ROW_PAD_X  8

// Draw the per-mission status icon at the right side of the row
// icon: 0=none(dot), 1=running(...), 2=done(v), 3=failed(x)
void drawMissionIcon(int i, uint8_t icon, bool hilite) {
  int y  = LIST_TOP + i * ROW_H;
  int ix = SCR_W - 20;
  int iy = y + ROW_H/2;

  // Clear icon area
  gfx->fillRect(ix-10, y+2, 18, ROW_H-4, hilite ? 0x0000 : COL_SURFACE);
  // For hilite rows the bg is a gradient; just overdraw with bg color approx
  if (hilite) gfx->fillRect(ix-10, y+2, 18, ROW_H-4, COL_BG);

  switch(icon){
    case 1: // running: three dots
      gfx->fillCircle(ix-4, iy, 2, COL_YELLOW);
      gfx->fillCircle(ix+1, iy, 2, COL_YELLOW);
      gfx->fillCircle(ix+6, iy, 2, COL_YELLOW);
      break;
    case 2: // done: checkmark ✓ drawn with lines
      gfx->drawLine(ix-4, iy,   ix-1, iy+3, COL_GREEN);
      gfx->drawLine(ix-3, iy,   ix,   iy+3, COL_GREEN);
      gfx->drawLine(ix-1, iy+3, ix+5, iy-3, COL_GREEN);
      gfx->drawLine(ix,   iy+3, ix+6, iy-3, COL_GREEN);
      break;
    case 3: // failed: X
      gfx->drawLine(ix-4, iy-4, ix+4, iy+4, COL_RED);
      gfx->drawLine(ix-3, iy-4, ix+5, iy+4, COL_RED);
      gfx->drawLine(ix+4, iy-4, ix-4, iy+4, COL_RED);
      gfx->drawLine(ix+5, iy-4, ix-3, iy+4, COL_RED);
      break;
    default: // idle: small dim dot
      gfx->fillCircle(ix+1, iy, 2, COL_DIMGRAY);
      break;
  }
}

void drawMissionRow(int i, bool hilite) {
  int y = LIST_TOP + i * ROW_H;

  if (hilite) {
    drawHGradBar(0, y, SCR_W, ROW_H-2, COL_ACCENT, COL_BG);
    gfx->fillRect(0, y, 3, ROW_H-2, COL_ACCENT2);
  } else {
    gfx->fillRect(0, y, SCR_W, ROW_H-2, COL_SURFACE);
    gfx->fillRect(0, y, 2, ROW_H-2, COL_BORDER);
  }
  gfx->drawFastHLine(0, y+ROW_H-2, SCR_W, COL_BORDER);

  // Row number
  gfx->setTextSize(1);
  gfx->setTextColor(hilite ? COL_ACCENT2 : COL_DIMGRAY);
  gfx->setCursor(ROW_PAD_X, y+ROW_H/2-4);
  char num[4]; snprintf(num,sizeof(num),"%02d",i+1);
  gfx->print(num);

  // Mission name
  gfx->setTextColor(hilite ? COL_WHITE : COL_GRAY);
  gfx->setCursor(ROW_PAD_X+22, y+ROW_H/2-4);
  char buf[18]; strncpy(buf, missions[i].name, 17); buf[17]='\0';
  gfx->print(buf);

  // Per-mission status icon (right side)
  drawMissionIcon(i, missionStatus[i], hilite);
}

void drawMissionList() {
  gfx->fillRect(0, LIST_TOP, SCR_W, MAX_MISSIONS*ROW_H+4, COL_BG);
  for (int i=0;i<missionCount;i++) drawMissionRow(i, i==selected);
}

// ================================================
// ================= FOOTER / STATUS ==============
// ================================================
#define FOOTER_Y  (SCR_H - 36)
#define STATUS_Y  (FOOTER_Y - 20)

void drawFooter() {
  gfx->drawFastHLine(0, FOOTER_Y, SCR_W, COL_BORDER);
  gfx->fillRect(0, FOOTER_Y+1, SCR_W, SCR_H-FOOTER_Y-1, COL_SURFACE);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_DIMGRAY);
  gfx->setCursor(8, FOOTER_Y+5);
  gfx->print("[v] scroll  [o] send");
  gfx->setCursor(8, FOOTER_Y+17);
  gfx->print("[hold o] high-priority");
}

void updateStatus(const String &msg) {
  statusText = msg;
  gfx->fillRect(0, STATUS_Y, SCR_W, 19, COL_BG);
  uint16_t dotCol = COL_ACCENT;
  if (msg.indexOf("FAIL")>=0 || msg.indexOf("Err")>=0 || msg.indexOf("Failed")>=0) dotCol=COL_RED;
  else if (msg.indexOf("Done")>=0 || msg.indexOf("OK")>=0) dotCol=COL_GREEN;
  else if (msg.indexOf("Running")>=0 || msg.indexOf("...")>=0) dotCol=COL_YELLOW;
  gfx->fillCircle(10, STATUS_Y+9, 3, dotCol);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_GRAY);
  gfx->setCursor(18, STATUS_Y+4);
  String s=msg; if(s.length()>27){s=s.substring(0,24)+"...";}
  gfx->print(s);
}

// ================================================
// ================= FULL UI DRAW =================
// ================================================
void drawUI() {
  gfx->fillScreen(COL_BG);
  drawHeader();
  forcePaintLeds();
  drawMissionList();
  drawFooter();
  updateStatus(statusText);
}

// ================================================
// ================= TOAST (non-blocking) =========
// ================================================
// Called from handleButtons — just draws + timestamps, no delay.
void showToast(const char* msg, uint16_t col) {
  toastX=20; toastY=LIST_TOP+2; toastW=SCR_W-40; toastH=20;
  gfx->fillRoundRect(toastX, toastY, toastW, toastH, 5, col);
  gfx->setTextSize(1);
  gfx->setTextColor(COL_WHITE);
  gfx->setCursor(toastX+6, toastY+6);
  gfx->print(msg);
  toastVisible  = true;
  toastShownAt  = millis();
}

// Called from loop() — clears toast when time is up, no blocking.
void tickToast() {
  if (!toastVisible) return;
  if (millis() - toastShownAt < TOAST_DURATION_MS) return;
  toastVisible = false;
  // Redraw rows that were covered
  int firstRow = 0;
  int lastRow  = min(missionCount-1, (toastY+toastH-LIST_TOP)/ROW_H);
  for (int i=firstRow; i<=lastRow && i<missionCount; i++)
    drawMissionRow(i, i==selected);
}

// ================================================
// ================= JSON HELPER ==================
// ================================================
String jsonGetString(const String &json, const String &key) {
  String needle = "\""+key+"\":\"";
  int idx=json.indexOf(needle);
  if(idx>=0){int s=idx+needle.length(),e=json.indexOf('"',s);if(e>s)return json.substring(s,e);}
  needle="\""+key+"\": \"";
  idx=json.indexOf(needle);
  if(idx>=0){int s=idx+needle.length(),e=json.indexOf('"',s);if(e>s)return json.substring(s,e);}
  needle="\""+key+"\":";
  idx=json.indexOf(needle);
  if(idx>=0){
    int s=idx+needle.length();
    while(s<(int)json.length()&&json[s]==' ')s++;
    int e=s;
    while(e<(int)json.length()&&json[e]!=','&&json[e]!='}')e++;
    return json.substring(s,e);
  }
  return "";
}

// ================================================
// ================= LOAD CONFIG ==================
// ================================================
void loadConfig() {
  File f=SD.open("/config.txt"); if(!f)return;
  while(f.available()){
    String l=f.readStringUntil('\n'); l.trim(); l.replace("\r","");
    if(l.length()==0||l.startsWith("#"))continue;
    if(l.startsWith("wifi_ssid="))         {String v=l.substring(10);v.trim();strncpy(ssid,v.c_str(),sizeof(ssid)-1);}
    else if(l.startsWith("wifi_password=")){String v=l.substring(14);v.trim();strncpy(pass,v.c_str(),sizeof(pass)-1);}
    else if(l.startsWith("robot_ip="))     {String v=l.substring(9);v.trim();strncpy(mir_ip,v.c_str(),sizeof(mir_ip)-1);}
    else if(l.startsWith("auth_token="))   {String v=l.substring(11);v.trim();strncpy(auth,v.c_str(),sizeof(auth)-1);}
    else if(l.startsWith("mission=")&&missionCount<MAX_MISSIONS){
      String v=l.substring(8);int c=v.indexOf(',');
      String n=v.substring(0,c);n.trim();
      String g=v.substring(c+1);g.trim();
      strncpy(missions[missionCount].name,n.c_str(),31);
      strncpy(missions[missionCount].guid,g.c_str(),63);
      missionStatus[missionCount]=0;
      missionCount++;
    }
  }
  f.close();
}

// ================================================
// ================= WIFI =========================
// ================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid,pass);
  updateStatus("WiFi connecting...");
  int t=0;
  while(WiFi.status()!=WL_CONNECTED&&t++<60)delay(333);
  updateStatus(WiFi.status()==WL_CONNECTED?"WiFi OK":"WiFi FAIL");
}

// ================================================
// ================= MIR API ======================
// ================================================
String queueURL()                   {return String("http://")+mir_ip+"/api/v2.0.0/mission_queue";}
String queueItemURL(const char *id) {return String("http://")+mir_ip+"/api/v2.0.0/mission_queue/"+id;}

String postMission(const char *guid, int priority) {
  HTTPClient http;
  http.begin(queueURL());
  http.addHeader("Content-Type","application/json");
  http.addHeader("Accept-Language","en_US");
  http.addHeader("Authorization",auth);
  String payload="{\"mission_id\":\""+String(guid)+"\",\"parameters\":[],\"priority\":"+String(priority)+"}";
  int code=http.POST(payload);
  String body=http.getString();
  http.end();
  if(code<200||code>=300)return "";
  String id=jsonGetString(body,"id");id.trim();return id;
}

String pollMissionState(const char *id) {
  HTTPClient http;
  http.begin(queueItemURL(id));
  http.addHeader("Authorization",auth);
  http.addHeader("Accept-Language","en_US");
  http.addHeader("Accept","application/json");
  int code=http.GET();
  String body=http.getString();
  http.end();
  if(code<200||code>=300)return "";
  String s=jsonGetString(body,"state");s.trim();return s;
}

// ================================================
// ================= SOFT QUEUE ===================
// ================================================
void enqueue(int missionIdx,int priority){
  int n=(qt+1)%QSIZE; if(n==qh)return;
  q[qt]={missionIdx,priority}; qt=n;
}
bool dequeue(int &missionIdx,int &priority){
  if(qh==qt)return false;
  missionIdx=q[qh].missionIdx; priority=q[qh].priority;
  qh=(qh+1)%QSIZE; return true;
}

// ================================================
// ================= ENGINE =======================
// ================================================
void processQueue() {
  switch(trackState){

    case TRACK_IDLE:{
      int idx,prio;
      if(!dequeue(idx,prio))return;
      activeMissionIdx=idx; activePriority=prio;
      drawLed(LED_NONE);

      // Mark mission as "running"
      missionStatus[idx]=1;
      drawMissionRow(idx, idx==selected);

      String id="";
      for(int k=0;k<3&&id.isEmpty();k++){id=postMission(missions[idx].guid,prio);if(id.isEmpty())delay(400);}

      if(id.isEmpty()){
        missionStatus[idx]=3;
        drawMissionRow(idx, idx==selected);
        drawLed(LED_RED);
        updateStatus("Send FAIL");
        activeMissionIdx=-1; return;
      }

      strncpy(activeQueueId,id.c_str(),sizeof(activeQueueId)-1);
      activeQueueId[sizeof(activeQueueId)-1]='\0';
      drawLed(LED_YELLOW);
      updateStatus("Running: "+String(missions[idx].name));
      lastPollMs=millis()-POLL_INTERVAL_MS;
      trackState=TRACK_POLLING;
      break;
    }

    case TRACK_POLLING:{
      if(millis()-lastPollMs<POLL_INTERVAL_MS)return;
      lastPollMs=millis();
      String state=pollMissionState(activeQueueId);

      if(state=="Pending"||state=="Executing"){
        drawLed(LED_YELLOW);
      } else if(state=="Done"){
        missionStatus[activeMissionIdx]=2;
        drawMissionRow(activeMissionIdx, activeMissionIdx==selected);
        drawLed(LED_GREEN);
        updateStatus("Done: "+String(missions[activeMissionIdx].name));
        activeMissionIdx=-1; trackState=TRACK_IDLE;
      } else if(state=="Aborted"||state=="Failed"){
        missionStatus[activeMissionIdx]=3;
        drawMissionRow(activeMissionIdx, activeMissionIdx==selected);
        drawLed(LED_RED);
        updateStatus("Failed: "+String(state));
        activeMissionIdx=-1; trackState=TRACK_IDLE;
      } else {
        missionStatus[activeMissionIdx]=3;
        drawMissionRow(activeMissionIdx, activeMissionIdx==selected);
        drawLed(LED_RED);
        updateStatus("Poll Err");
        activeMissionIdx=-1; trackState=TRACK_IDLE;
      }
      break;
    }

    default: trackState=TRACK_IDLE; break;
  }
}

// ================================================
// ================= INPUT ========================
// ================================================
#define LONG_PRESS_MS 600

void handleButtons() {
  static bool          lastD=HIGH, lastS=HIGH;
  static unsigned long sPressedAt=0;
  static bool          sLongFired=false;

  bool d=digitalRead(BTN_DOWN);
  bool s=digitalRead(BTN_SELECT);

  // DOWN: cycle selection (instant, no blocking)
  if(lastD==HIGH && d==LOW){
    int prev=selected;
    selected=(selected+1)%missionCount;
    drawMissionRow(prev,  false);
    drawMissionRow(selected, true);
  }

  // SELECT: falling edge — start timing
  if(lastS==HIGH && s==LOW){ sPressedAt=millis(); sLongFired=false; }

  // SELECT held: show HIGH-PRIO badge (non-blocking)
  if(s==LOW && !sLongFired && (millis()-sPressedAt>=LONG_PRESS_MS)){
    sLongFired=true;
    gfx->fillRoundRect(SCR_W-42, 5, 38, 16, 4, COL_RED);
    gfx->setTextSize(1); gfx->setTextColor(COL_WHITE);
    gfx->setCursor(SCR_W-38, 9); gfx->print("PRI!");
  }

  // SELECT released — decide short / long
  if(lastS==LOW && s==HIGH){
    unsigned long held=millis()-sPressedAt;
    // Restore pill (clears PRI! badge area)
    drawStatusPill(currentLed);

    if(held>=LONG_PRESS_MS){
      enqueue(selected,1);
      showToast("HIGH PRIO queued!", COL_RED);
      Serial.println("[BTN] Long press -> HIGH priority");
    } else {
      enqueue(selected,0);
      showToast("Queued!", COL_ACCENT);
      Serial.println("[BTN] Short press -> normal priority");
    }
    sLongFired=false;
  }

  lastD=d; lastS=s;
}

// ================================================
// ================= SETUP / LOOP =================
// ================================================
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
  memset(missionStatus, 0, sizeof(missionStatus));
  loadConfig();
  drawUI();
  connectWiFi();
  drawUI();
}

void loop() {
  handleButtons();   // always responsive — zero blocking calls here
  tickToast();       // clears toast when timer expires, no delay
  processQueue();
  delay(10);
}
