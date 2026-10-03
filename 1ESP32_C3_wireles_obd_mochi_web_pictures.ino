// ============================================================
// SMART GAUGE AUTONOMOUS v1.7 — ESP32-2424S012 (GC9A01 240x240)
// Один провод питания. BLE-клиент к ELM327-BLE напрямую.
// Передача = передаточные числа (K = RPM/speed), калибровка с сайта.
// LVGL UI + WEB + медиа + яркость ШИМ. Без ESP-NOW, без второго узла.
// ============================================================

// ВАЖНО: forward declaration для Arduino IDE auto-prototypes
struct SideAnim;

#include <lvgl.h>
LV_FONT_DECLARE(gear72);
#ifndef PROGMEM
#define PROGMEM
#endif
#include "mochi_frames.h"
#if MOCHI_W > 160 || MOCHI_H > 160
#error "mochi_frames.h slishkom bolshoy: peregeneriruy 128x128 ili 160x160"
#endif
#include "site_assets.h"
#include <SPI.h>
#include <WiFi.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <math.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#define FW_VERSION "1.7"
#define TFT_MOSI 7
#define TFT_SCLK 6
#define TFT_CS   10
#define TFT_DC   2
#define TFT_BL   3
#define BTN_PIN  9
#define LINK_TIMEOUT_MS 2500
#define PKT_POLL_MS 25
#define SPLASH_SHOW_MS 2500
#define SPLASH_FADE_MS 400
#define LOGO_ZOOM 384
#define RPM_MAX 7000
#define LONG_PRESS_MS 3000
#define SCREEN_COUNT 8
#define RPM_MOVE_MS 100.0f
#define SPEED_MOVE_MS 130.0f
#define RPM_SNAP_DELTA 2.0f
#define SPEED_SNAP_DELTA 0.15f
#define DEFAULT_ACCENT_HEX 0x3B82F6
#define DEFAULT_AP_PASS "gear1234"
#define BRIGHT_MIN 30
#define BRIGHT_MAX 255
#define MEDIA_PATH "/media.gimg"
#define MEDIA_MAX_FRAMES 4
#define MOCHI_ANGRY_RPM_START 3000
#define MOCHI_RED_RPM 4500
#define MOCHI_LOW_FUEL 20
#define MOCHI_APPEAR_MS 42
#define MOCHI_BLINK_MS  55
#define MOCHI_JUMP_MS   42
#define MOCHI_RELAX_MS  90
#define MOCHI_REV_MS    42

// ---------------- ТЕЛЕМЕТРИЯ (локальная) ----------------
struct Tele { uint8_t gear; uint16_t rpm; uint8_t speed; uint8_t coolant; uint16_t voltage; uint8_t fuel; };
static Tele pkt = {255,0,0,0,0,0};
static SemaphoreHandle_t pktMux = nullptr;
static volatile uint32_t lastDataMs = 0;
static float liveK = 0.0f;

// ---------------- КАЛЬКУЛЯТОР ПЕРЕДАЧ ----------------
static float  trK[7] = {0,0,0,0,0,0,0};
static uint8_t trMask = 0;
static float  trTol = 0.10f;
static uint8_t trRmax = 15;
static void trLoad() {
  Preferences pr; pr.begin("gear2", true);
  size_t got = pr.getBytes("k", trK, sizeof(trK));
  trMask = pr.getUChar("mask", 0);
  int tl = pr.getInt("tol", 10); trTol = tl / 100.0f;
  trRmax = pr.getUChar("rmax", 15);
  if (got != sizeof(trK)) { memset(trK, 0, sizeof(trK)); trMask = 0; }
  pr.end();
}
static void trSave() {
  Preferences pr; pr.begin("gear2", false);
  pr.putBytes("k", trK, sizeof(trK));
  pr.putUChar("mask", trMask);
  pr.putInt("tol", (int)(trTol * 100 + 0.5f));
  pr.putUChar("rmax", trRmax);
  pr.end();
}
static uint8_t computeGearRaw(uint16_t rpm, uint8_t speed) {
  if (rpm < 400) return 255;
  if (speed >= 1 && speed <= trRmax && (trMask & (1<<6))) {
    float K = (float)rpm / (float)speed;
    if (fabsf(K - trK[6]) <= trK[6] * trTol) return 6;
  }
  if (speed <= 3) return (trMask & 1) ? 0 : 255;
  float K = (float)rpm / (float)speed;
  liveK = K;
  int best = -1; float bestDev = 1e9f;
  for (int g = 1; g <= 5; g++) if (trMask & (1<<g)) {
    float d = fabsf(K - trK[g]);
    if (d <= trK[g] * trTol && d < bestDev) { bestDev = d; best = g; }
  }
  if (best >= 0) return (uint8_t)best;
  if (speed >= 10) return 0;
  return 255;
}
static uint8_t gearStable(uint16_t rpm, uint8_t speed) {
  static uint8_t last = 255, stable = 255;
  static uint32_t since = 0;
  uint8_t raw = computeGearRaw(rpm, speed);
  uint32_t now = millis();
  if (raw == last) { if (stable != raw && now - since > 500) stable = raw; }
  else { last = raw; since = now; }
  return stable;
}

// ---------------- РЕКОРДЕР: регрессия + свободная езда ----------------
static volatile int8_t recGear = -1;
static uint32_t recStart = 0, recValidMs = 0, recLastTick = 0;
static float recSRV = 0, recSVV = 0;
static int recProg = 0;
static volatile bool fdActive = false;
static float fdHist[64]; static uint8_t fdCount = 0;
static float fdProp[8];  static uint8_t fdPropN = 0;
static uint32_t tsRpm = 0, tsSpeed = 0;
static void recBegin(uint8_t g) {
  recGear = (int8_t)g; recStart = millis(); recValidMs = 0; recProg = 0;
  recSRV = recSVV = 0; recLastTick = 0;
  Serial.print("[TR] rec gear "); Serial.println(g);
}
static void recAbort() { recGear = -1; recProg = 0; recValidMs = 0; recSRV = recSVV = 0; recLastTick = 0; }
static void fdFinish() {
  fdActive = false; fdPropN = 0;
  if (fdCount < 16) { Serial.print("[TR] fd few samples: "); Serial.println(fdCount); fdCount = 0; return; }
  float s[64]; memcpy(s, fdHist, fdCount * sizeof(float));
  for (uint8_t i = 1; i < fdCount; i++) { float v = s[i]; int j = i - 1; while (j >= 0 && s[j] > v) { s[j+1] = s[j]; j--; } s[j+1] = v; }
  float csum = 0; uint8_t cnum = 0;
  for (uint8_t i = 0; i < fdCount; i++) {
    if (cnum == 0) { csum = s[i]; cnum = 1; }
    else if (s[i] > (csum / cnum) * 1.12f) {
      if (fdPropN < 8 && cnum >= 4) fdProp[fdPropN++] = csum / cnum;
      csum = s[i]; cnum = 1;
    } else { csum += s[i]; cnum++; }
  }
  if (fdPropN < 8 && cnum >= 4) fdProp[fdPropN++] = csum / cnum;
  Serial.print("[TR] fd clusters: "); Serial.println(fdPropN);
  fdCount = 0;
}
static void fdConfirm() {
  for (uint8_t i = 0; i < fdPropN; i++) {
    uint8_t g = (uint8_t)(fdPropN - i);
    if (g >= 1 && g <= 6) { trK[g] = fdProp[i]; trMask |= (uint8_t)(1 << g); }
  }
  trSave(); fdPropN = 0;
  Serial.println("[TR] fd proposals applied");
}
static void recTick(uint16_t rpm, uint8_t speed, bool linked) {
  if (recGear < 0 && !fdActive) return;
  if (!linked || rpm < 400 || speed < 1) return;
  uint32_t now = millis();
  if (now - tsRpm > 400 || now - tsSpeed > 400) return;
  int32_t dts = (int32_t)(tsRpm - tsSpeed); if (dts < 0) dts = -dts;
  if (dts > 150) return;
  if (rpm > 4500) return;
  static uint16_t lastRpm = 0; static uint32_t lastRpmT = 0;
  bool jerk = false;
  if (lastRpmT && now - lastRpmT <= 300) {
    int d = (int)rpm - (int)lastRpm; if (d < 0) d = -d;
    if (d > 90 + (int)(now - lastRpmT)) jerk = true;
  }
  lastRpm = rpm; lastRpmT = now;
  if (jerk) return;
  uint8_t vmin;
  if (recGear == 6) vmin = 1;
  else if (recGear >= 1) vmin = (recGear == 1) ? 8 : (recGear == 2) ? 15 : (recGear == 3) ? 25 : (recGear == 4) ? 35 : 55;
  else vmin = 15;
  if (rpm < 1200 || rpm > 4000 || speed < vmin || speed > 140) return;
  float v = (float)speed, r = (float)rpm;
  if (recGear >= 1) {
    recSRV += r * v; recSVV += v * v;
    uint32_t dt = recLastTick ? (now - recLastTick) : 0; if (dt > 500) dt = 500;
    recValidMs += dt; recLastTick = now;
    recProg = (int)(recValidMs / 40); if (recProg > 100) recProg = 100;
    if (recValidMs >= 4000 && recSVV > 1.0f) {
      trK[recGear] = recSRV / recSVV; trMask |= (uint8_t)(1 << recGear); trSave();
      Serial.print("[TR] gear "); Serial.print(recGear);
      Serial.print(" K="); Serial.print(trK[recGear]);
      Serial.print(" ms="); Serial.println(recValidMs);
      recAbort();
    } else if (now - recStart > 60000) {
      Serial.println("[TR] timeout 60s, stop");
      recAbort();
    }
  } else if (fdActive && fdCount < 64) {
    fdHist[fdCount++] = r / v;
  }
}

// ---------------- BLE: БУФЕР И ELM-ПРОТОКОЛ ----------------
static char bleBuf[256];
static volatile size_t bleLen = 0;
static volatile bool blePrompt = false;
static void notifyCb(BLERemoteCharacteristic*, uint8_t* d, size_t l, bool) {
  for (size_t i = 0; i < l && bleLen < sizeof(bleBuf) - 1; i++) {
    char c = (char)d[i];
    if (c == '>') { bleBuf[bleLen] = 0; blePrompt = true; }
    else if (c != '\r') bleBuf[bleLen++] = c;
  }
  bleBuf[bleLen] = 0;
}
static BLEClient *pClient = nullptr;
static BLERemoteCharacteristic *pChar = nullptr;
static bool bleLinked = false;
static char bleMac[18] = {0};
static void btLoadCreds() {
  Preferences pr; pr.begin("btcred", true);
  String m = pr.getString("mac", "");
  strncpy(bleMac, m.c_str(), 17); bleMac[17] = 0;
  pr.end();
}
static void btSaveCreds(const char *mac) {
  Preferences pr; pr.begin("btcred", false);
  pr.putString("mac", mac);
  pr.end();
  strncpy(bleMac, mac, 17); bleMac[17] = 0;
}
static bool bleFindUart() {
  auto *svcs = pClient->getServices();
  if (!svcs) return false;
  for (auto &sp : *svcs) {
    BLERemoteService *s = sp.second;
    if (!s) continue;
    s->retrieveCharacteristics();
    auto *chrs = s->getCharacteristics();
    if (!chrs) continue;
    for (auto &cp : *chrs) {
      BLERemoteCharacteristic *c = cp.second;
      if (c && c->canWrite() && c->canNotify()) {
        pChar = c;
        c->registerForNotify(notifyCb);
        return true;
      }
    }
  }
  return false;
}
static bool bleConnectTo(const char *mac) {
  if (!pClient) pClient = BLEDevice::createClient();
  if (pClient->isConnected()) pClient->disconnect();
  bleLen = 0; blePrompt = false; pChar = nullptr;
  if (!pClient->connect(BLEAddress(mac))) { Serial.println("[BLE] connect fail"); return false; }
  if (!bleFindUart()) { Serial.println("[BLE] no UART char"); pClient->disconnect(); return false; }
  bleLinked = true;
  Serial.print("[BLE] linked: "); Serial.println(mac);
  return true;
}
static bool obdRequest(const char *cmd, uint32_t timeoutMs) {
  if (!bleLinked || !pChar) return false;
  bleLen = 0; blePrompt = false; bleBuf[0] = 0;
  pChar->writeValue((uint8_t*)cmd, strlen(cmd), false);
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (blePrompt) return true;
    delay(2);
  }
  return false;
}
char obdBuf[192];
const uint8_t OBD_PIDS[5] = {0x0C, 0x0D, 0x05, 0x42, 0x2F};
static int hexValue(char c) { if (c>='0'&&c<='9')return c-'0'; if (c>='A'&&c<='F')return c-'A'+10; if (c>='a'&&c<='f')return c-'a'+10; return -1; }
static bool readHexByte(const char*& p, uint8_t& v) {
  while (*p==' ' || *p==10 || *p=='\t') p++;
  int h = hexValue(*p); if (h<0) return false; p++;
  int l = hexValue(*p); if (l<0) return false; p++;
  v = (uint8_t)((h<<4)|l); return true;
}
static bool findPid(uint8_t want, uint8_t& a, uint8_t& b, bool& hasB) {
  const char* s = obdBuf;
  while (*s) {
    if (s[0]=='4' && s[1]=='1') {
      const char* p = s; uint8_t m, pid, d1, d2;
      if (readHexByte(p,m) && readHexByte(p,pid) && readHexByte(p,d1)) {
        if (m==0x41 && pid==want) { a=d1; hasB=readHexByte(p,d2); b=hasB?d2:0; return true; }
      }
    }
    s++;
  }
  return false;
}
static void parsePid(uint8_t pid) {
  uint8_t a = 0, b = 0; bool hb = false;
  if (!findPid(pid, a, b, hb)) return;
  xSemaphoreTake(pktMux, portMAX_DELAY);
  if (pid == 0x0C) { if (hb) { pkt.rpm = (uint16_t)(((uint16_t)a * 256U + b) / 4U); tsRpm = millis(); } }
  else if (pid == 0x0D) { pkt.speed = a; tsSpeed = millis(); }
  else if (pid == 0x05) pkt.coolant = (uint8_t)(a - 40);
  else if (pid == 0x42) { if (hb) pkt.voltage = (uint16_t)a * 256U + b; }
  else if (pid == 0x2F) pkt.fuel = (uint8_t)((100U * a) / 255U);
  xSemaphoreGive(pktMux);
  lastDataMs = millis();
}
static bool obdInitSeq() {
  if (!obdRequest("ATZ\r", 3500)) return false;
  bleLen = 0; blePrompt = false;
  if (!obdRequest("ATE0\r",1200)) return false;
  obdRequest("ATL0\r",1200); obdRequest("ATS0\r",1200);
  obdRequest("ATH0\r",1200); obdRequest("ATSP0\r",1800);
  Serial.println("[OBD] ELM ready");
  return true;
}
static void obdTask(void*) {
  uint32_t lastTry = 0; uint8_t fails = 0;
  for (;;) {
    if (!bleLinked) {
      if (bleMac[0] && millis() - lastTry >= 5000) {
        lastTry = millis();
        if (bleConnectTo(bleMac)) { if (obdInitSeq()) fails = 0; else bleLinked = false; }
      }
      delay(100); continue;
    }
    for (uint8_t i = 0; i < 5; i++) {
      if (!bleLinked) break;
      char cmd[8]; snprintf(cmd, sizeof(cmd), "01%02X\r", OBD_PIDS[i]);
      if (obdRequest(cmd, 1400)) { strncpy(obdBuf, bleBuf, sizeof(obdBuf)-1); obdBuf[sizeof(obdBuf)-1]=0; fails = 0; parsePid(OBD_PIDS[i]); }
      else if (++fails >= 5) { Serial.println("[OBD] lost, reconnect"); bleLinked = false; if (pClient->isConnected()) pClient->disconnect(); break; }
      delay(40);
    }
    delay(100);
  }
}

// ---------------- BLE: СКАН ДЛЯ САЙТА (фоновая задача) ----------------
struct BtDev { char mac[18]; char name[24]; };
static BtDev btDevs[12];
static uint8_t btDevCount = 0;
static volatile bool btScanning = false;
static uint32_t btScanEndMs = 0;
class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    String m = d.getAddress().toString(); m.toUpperCase();
    for (uint8_t i = 0; i < btDevCount; i++) if (m == btDevs[i].mac) return;
    if (btDevCount < 12) {
      strncpy(btDevs[btDevCount].mac, m.c_str(), 17); btDevs[btDevCount].mac[17]=0;
      String n = d.haveName() ? d.getName() : String("");
      strncpy(btDevs[btDevCount].name, n.c_str(), 23); btDevs[btDevCount].name[23]=0;
      btDevCount++;
    }
  }
};
static ScanCB scanCb;
static void btScanTask(void*) {
  BLEScan *s = BLEDevice::getScan();
  s->setAdvertisedDeviceCallbacks(&scanCb);
  s->setActiveScan(true);
  s->setInterval(100); s->setWindow(99);
  s->start(8, false);
  btScanning = false;
  vTaskDelete(NULL);
}
static void btStartScan() {
  if (btScanning) return;
  btDevCount = 0; btScanning = true; btScanEndMs = millis() + 9000;
  xTaskCreate(btScanTask, "btscan", 5120, nullptr, 1, nullptr);
}
static bool btScanActive() {
  if (btScanning && (int32_t)(millis() - btScanEndMs) > 0) btScanning = false;
  return btScanning;
}

// ---------------- UI: ГЛОБАЛКИ ----------------
extern const lv_img_dsc_t img_logo;
static lv_obj_t *rpmArc=NULL,*linkDot=NULL,*lblMain=NULL,*lblSub=NULL,*lblPrev=NULL,*lblNext=NULL;
static lv_obj_t *lblRow1=NULL,*lblRow2=NULL,*lblRow3=NULL,*lblVoltage=NULL,*lblFuel=NULL,*lblVoltSub=NULL;
static lv_obj_t *mochiCanvas=NULL;
static lv_color_t mochiBuf[MOCHI_W * MOCHI_H];
static File mediaFile;
static lv_color_t* mediaBuf = nullptr;  // ДИНАМИЧЕСКИЙ буфер для экономии памяти
static lv_obj_t *mediaCanvas = NULL;
static uint8_t mediaFrames=0, mediaFps=10, mediaCur=0;
static uint32_t mediaFrameAt=0;
static bool mediaOpen=false;
static uint8_t curScreen=0, rotIdx=0;
static uint32_t accentHex=DEFAULT_ACCENT_HEX;
static lv_color_t accentColor;
static uint8_t setUnits=0,setBright=70,setMochi=1,setSplash=1,setLang=0;
static char apPass[16]=DEFAULT_AP_PASS, apSSID[24];
static bool serviceOn=false;
static WebServer *web=nullptr;
static DNSServer *dns=nullptr;
static uint32_t splashStart=0,fadeOutStart=0;
static bool fadeOutStarted=false,splashDone=false;
static float shownRpm=0,shownSpeed=0;
static uint16_t lastDrawnRpm=65535,lastDrawnSpeed=65535,lastDrawnVoltage=65535;
static uint8_t lastDrawnGear=255,lastDrawnCoolant=255,lastDrawnFuel=255;
static int lastLinkState=-1;
static int lastSidePrev=255,lastSideNext=255;
static uint8_t lastGearForDir=255;
static bool mochiJumpQueued=false;
static uint8_t prevGear=255;
static bool bootHold=false;
static void setupWebRoutes(); static void startService(); static void stopService();

// ---------------- карусель ----------------
struct SideAnim { lv_obj_t *lbl; int16_t cx, cy, x0; int dir; char txt[4]; };
static SideAnim sideAnim[2];
static void sidePlace(SideAnim *sa){ sa->x0=sa->cx-lv_obj_get_width(sa->lbl)/2; lv_obj_set_x(sa->lbl,sa->x0); lv_obj_set_y(sa->lbl,sa->cy-lv_obj_get_height(sa->lbl)/2); }
static void sidePhaseB(SideAnim*);
static void sideExecA(void*v,int32_t x){ SideAnim*s=(SideAnim*)v; lv_obj_set_style_opa(s->lbl,(lv_opa_t)(255-x),0); lv_obj_set_x(s->lbl,s->x0-s->dir*(int16_t)((x*20)/255)); }
static void sideExecB(void*v,int32_t x){ SideAnim*s=(SideAnim*)v; lv_obj_set_style_opa(s->lbl,(lv_opa_t)x,0); lv_obj_set_x(s->lbl,s->x0+s->dir*(int16_t)(20-(x*20)/255)); }
static void sideReadyA(lv_anim_t*a){ SideAnim*s=(SideAnim*)a->var; lv_label_set_text(s->lbl,s->txt); sidePlace(s); lv_obj_set_style_opa(s->lbl,0,0); sidePhaseB(s); }
static void sidePhaseB(SideAnim*sa){ lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a,sa); lv_anim_set_values(&a,0,255); lv_anim_set_time(&a,140); lv_anim_set_exec_cb(&a,sideExecB); lv_anim_start(&a); }
static void startSideFlip(SideAnim*sa,const char*t,int dir){ lv_anim_del(sa,NULL); snprintf(sa->txt,sizeof(sa->txt),"%s",t); sa->dir=dir; lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a,sa); lv_anim_set_values(&a,0,255); lv_anim_set_time(&a,120); lv_anim_set_exec_cb(&a,sideExecA); lv_anim_set_ready_cb(&a,sideReadyA); lv_anim_start(&a); }
static void sideSetStatic(SideAnim*sa,const char*t){ lv_anim_del(sa,NULL); lv_label_set_text(sa->lbl,t); lv_obj_set_style_opa(sa->lbl,255,0); sidePlace(sa); }
static int seqIdx(uint8_t g){ static const uint8_t S[7]={6,0,1,2,3,4,5}; for(int i=0;i<7;i++) if(S[i]==g) return i; return -1; }
static void gearStr(uint8_t g,char*b){ if(g==0){b[0]='N';b[1]=0;} else if(g==6){b[0]='R';b[1]=0;} else if(g<=5){b[0]='0'+g;b[1]=0;} else {b[0]='-';b[1]=0;} }

// ---------------- GC9A01 ----------------
static inline void lcdCmd(uint8_t c){ digitalWrite(TFT_DC,LOW); digitalWrite(TFT_CS,LOW); SPI.transfer(c); digitalWrite(TFT_CS,HIGH); }
static inline void lcdDat(uint8_t d){ digitalWrite(TFT_DC,HIGH); digitalWrite(TFT_CS,LOW); SPI.transfer(d); digitalWrite(TFT_CS,HIGH); }
static void lcdSeq(uint8_t c,const uint8_t*d,uint8_t n){ lcdCmd(c); for(uint8_t i=0;i<n;i++) lcdDat(d[i]); }
static void initDisplay(){
  pinMode(TFT_CS,OUTPUT); digitalWrite(TFT_CS,HIGH); pinMode(TFT_DC,OUTPUT);
  SPI.begin(TFT_SCLK,-1,TFT_MOSI,-1);
  SPI.beginTransaction(SPISettings(40000000,MSBFIRST,SPI_MODE0));
  lcdCmd(0xEF); lcdCmd(0xEB); lcdDat(0x14); lcdCmd(0xFE); lcdCmd(0xEF); lcdCmd(0xEB); lcdDat(0x14);
  lcdCmd(0x84); lcdDat(0x40); lcdCmd(0x85); lcdDat(0xFF); lcdCmd(0x86); lcdDat(0xFF); lcdCmd(0x87); lcdDat(0xFF);
  lcdCmd(0x88); lcdDat(0x0A); lcdCmd(0x89); lcdDat(0x21); lcdCmd(0x8A); lcdDat(0x00); lcdCmd(0x8B); lcdDat(0x80);
  lcdCmd(0x8C); lcdDat(0x01); lcdCmd(0x8D); lcdDat(0x01); lcdCmd(0x8E); lcdDat(0xFF); lcdCmd(0x8F); lcdDat(0xFF);
  const uint8_t b6[]={0x00,0x20}; lcdSeq(0xB6,b6,2);
  lcdCmd(0x3A); lcdDat(0x05);
  const uint8_t s90[]={0x08,0x08,0x08,0x08}; lcdSeq(0x90,s90,4);
  lcdCmd(0xBD); lcdDat(0x06); lcdCmd(0xBC); lcdDat(0x00);
  const uint8_t sFF[]={0x60,0x01,0x04}; lcdSeq(0xFF,sFF,3);
  lcdCmd(0xC3); lcdDat(0x13); lcdCmd(0xC4); lcdDat(0x13); lcdCmd(0xC9); lcdDat(0x22); lcdCmd(0xBE); lcdDat(0x11);
  const uint8_t e1[]={0x10,0x0E}; lcdSeq(0xE1,e1,2);
  const uint8_t df[]={0x21,0x0C,0x02}; lcdSeq(0xDF,df,3);
  const uint8_t f0[]={0x45,0x09,0x08,0x08,0x26,0x2A}; lcdSeq(0xF0,f0,6);
  const uint8_t f1[]={0x43,0x70,0x72,0x36,0x37,0x6F}; lcdSeq(0xF1,f1,6);
  const uint8_t f2[]={0x45,0x09,0x08,0x08,0x26,0x2A}; lcdSeq(0xF2,f2,6);
  const uint8_t f3[]={0x43,0x70,0x72,0x36,0x37,0x6F}; lcdSeq(0xF3,f3,6);
  const uint8_t ed[]={0x1B,0x0B}; lcdSeq(0xED,ed,2);
  lcdCmd(0xAE); lcdDat(0x77); lcdCmd(0xCD); lcdDat(0x63);
  const uint8_t s70[]={0x07,0x07,0x04,0x0E,0x0F,0x09,0x07,0x08,0x03}; lcdSeq(0x70,s70,9);
  lcdCmd(0xE8); lcdDat(0x34);
  const uint8_t s62[]={0x18,0x0D,0x71,0xED,0x70,0x70,0x18,0x0F,0x71,0xEF,0x70,0x70}; lcdSeq(0x62,s62,12);
  const uint8_t s63[]={0x18,0x11,0x71,0xF1,0x70,0x70,0x18,0x13,0x71,0xF3,0x70,0x70}; lcdSeq(0x63,s63,12);
  const uint8_t s64[]={0x28,0x29,0xF1,0x01,0xF1,0x00,0x07}; lcdSeq(0x64,s64,7);
  const uint8_t s66[]={0x3C,0x00,0xCD,0x67,0x45,0x45,0x10,0x00,0x00,0x00}; lcdSeq(0x66,s66,10);
  const uint8_t s67[]={0x00,0x3C,0x00,0x00,0x00,0x01,0x54,0x10,0x32,0x98}; lcdSeq(0x67,s67,10);
  const uint8_t s74[]={0x10,0x85,0x80,0x00,0x00,0x4E,0x00}; lcdSeq(0x74,s74,7);
  const uint8_t s98[]={0x3E,0x07}; lcdSeq(0x98,s98,2);
  lcdCmd(0x35); lcdCmd(0x21); lcdCmd(0x11); delay(120); lcdCmd(0x29); delay(20);
}
static void setWindow(int16_t x1,int16_t y1,int16_t x2,int16_t y2){
  lcdCmd(0x2A); lcdDat(x1>>8); lcdDat(x1); lcdDat(x2>>8); lcdDat(x2);
  lcdCmd(0x2B); lcdDat(y1>>8); lcdDat(y1); lcdDat(y2>>8); lcdDat(y2);
  lcdCmd(0x2C);
}
static void flush_cb(lv_disp_drv_t*drv,const lv_area_t*area,lv_color_t*color_p){
  setWindow(area->x1,area->y1,area->x2,area->y2);
  uint32_t px=(area->x2-area->x1+1)*(area->y2-area->y1+1);
  digitalWrite(TFT_DC,HIGH); digitalWrite(TFT_CS,LOW);
  SPI.writeBytes((uint8_t*)color_p,px*2);
  digitalWrite(TFT_CS,HIGH);
  lv_disp_flush_ready(drv);
}

// ---------------- NVS UI ----------------
static void saveUiState(){ Preferences pr; pr.begin("gearui",false); pr.putUChar("rot",rotIdx); pr.putUChar("screen",curScreen); pr.end(); }
static void saveExtras(){ Preferences pr; pr.begin("gearui",false); pr.putUChar("units",setUnits); pr.putUChar("bright",setBright); pr.putUChar("mochi",setMochi); pr.putUChar("splash",setSplash); pr.putUChar("lang",setLang); pr.putString("apass",apPass); pr.end(); }
static void loadUiState(){
  Preferences pr; pr.begin("gearui",true);
  uint8_t r=pr.getUChar("rot",0), s=pr.getUChar("screen",0);
  accentHex=pr.getUInt("accent",DEFAULT_ACCENT_HEX);
  setUnits=pr.getUChar("units",0); setBright=pr.getUChar("bright",70);
  setMochi=pr.getUChar("mochi",1); setSplash=pr.getUChar("splash",1); setLang=pr.getUChar("lang",0);
  String p=pr.getString("apass",DEFAULT_AP_PASS); strncpy(apPass,p.c_str(),15); apPass[15]=0;
  pr.end();
  rotIdx=(r<=3)?r:0; curScreen=(s<=SCREEN_COUNT-1)?s:0;
}
static void applyAccentColor(){ accentColor=lv_color_hex(accentHex); lastDrawnGear=255; lastDrawnRpm=65535; lastDrawnSpeed=65535; lastDrawnCoolant=255; lastDrawnVoltage=65535; lastDrawnFuel=255; }
static void applyBrightness(){ uint8_t p=(setBright>100)?100:setBright; analogWrite(TFT_BL,(uint8_t)(BRIGHT_MIN+((uint32_t)(BRIGHT_MAX-BRIGHT_MIN)*p)/100)); }
static void setAccentHex(uint32_t h){ accentHex=h; applyAccentColor(); Preferences pr; pr.begin("gearui",false); pr.putUInt("accent",accentHex); pr.end(); }
static void applyRotation(){ lv_disp_set_rotation(lv_disp_get_default(),(lv_disp_rot_t)rotIdx); lv_obj_invalidate(lv_scr_act()); }
static void doRotate(){ rotIdx=(rotIdx+1)%4; applyRotation(); saveUiState(); }

// ---------------- цвета/математика ----------------
static lv_color_t tempColor(uint8_t t){ if(t<105)return lv_color_hex(0xC0C0C0); if(t<115)return lv_color_hex(0xFFC400); return lv_color_hex(0xFF2B2B); }
static lv_color_t fuelColor(uint8_t f){ if(f>40)return lv_color_hex(0x00E500); if(f>20)return lv_color_hex(0xFFC400); return lv_color_hex(0xFF2B2B); }
static lv_color_t voltageColor(uint16_t mv){ if(mv>=12000&&mv<=14800)return lv_color_hex(0x00E500); if(mv>=11500&&mv<=15200)return lv_color_hex(0xFFC400); return lv_color_hex(0xFF2B2B); }
static lv_color_t rpmColor(uint16_t rpm){
  uint16_t r=(rpm>RPM_MAX)?RPM_MAX:rpm; float f=(float)r/RPM_MAX;
  const lv_color_t g=lv_color_hex(0x00E500), y=lv_color_hex(0xFFC400), rd=lv_color_hex(0xFF2B2B);
  if(f<0.65f)return lv_color_mix(y,g,(uint8_t)((f/0.65f)*255));
  if(f<0.85f)return lv_color_mix(rd,y,(uint8_t)(((f-0.65f)/0.20f)*255));
  return rd;
}
static float moveToValue(float c,float t,float dt,float mv,float sn){ float d=t-c; if(fabsf(d)<=sn)return t; if(dt>=mv)return t; float r=c+d*(dt/mv); return (fabsf(t-r)<=sn)?t:r; }
static String jsonStr(const String&b,const char*k){ String s=String("\"")+k+"\""; int i=b.indexOf(s); if(i<0)return String(); i=b.indexOf(':',i+s.length()); if(i<0)return String(); i=b.indexOf('"',i+1); if(i<0)return String(); int j=b.indexOf('"',i+1); if(j<0)return String(); return b.substring(i+1,j); }
static int jsonBool(const String&b,const char*k,int d){ String s=String("\"")+k+"\""; int i=b.indexOf(s); if(i<0)return d; i=b.indexOf(':',i+s.length()); if(i<0)return d; String t=b.substring(i+1,i+10); t.trim(); if(t.startsWith("true"))return 1; if(t.startsWith("false"))return 0; return d; }
static int jsonInt(const String&b,const char*k,int d){ String s=String("\"")+k+"\""; int i=b.indexOf(s); if(i<0)return d; i=b.indexOf(':',i+s.length()); if(i<0)return d; return b.substring(i+1,i+8).toInt(); }
static float jsonFloat(const String&b,const char*k,float d){ String s=String("\"")+k+"\""; int i=b.indexOf(s); if(i<0)return d; i=b.indexOf(':',i+s.length()); if(i<0)return d; return b.substring(i+1,i+12).toFloat(); }

// ---------------- виджеты ----------------
static void makeRpmArc(lv_obj_t*scr){
  rpmArc=lv_arc_create(scr); lv_obj_set_size(rpmArc,198,198); lv_obj_center(rpmArc);
  lv_arc_set_bg_angles(rpmArc,180,360); lv_arc_set_range(rpmArc,0,RPM_MAX); lv_arc_set_value(rpmArc,0);
  lv_obj_set_style_arc_width(rpmArc,12,LV_PART_MAIN); lv_obj_set_style_arc_width(rpmArc,12,LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(rpmArc,lv_color_hex(0x303030),LV_PART_MAIN);
  lv_obj_remove_style(rpmArc,NULL,LV_PART_KNOB);
}
static void makeLinkDot(lv_obj_t*scr){
  linkDot=lv_obj_create(scr); lv_obj_set_size(linkDot,10,10);
  lv_obj_set_style_radius(linkDot,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(linkDot,lv_color_hex(0xFF2B2B),0);
  lv_obj_set_style_border_width(linkDot,0,0);
  lv_obj_align(linkDot,LV_ALIGN_TOP_MID,0,36);
}
static void drawMochiFrame(int idx,lv_color_t color){
  if(idx<0)idx=0; if(idx>=MOCHI_FRAMES)idx=MOCHI_FRAMES-1;
  const uint8_t*f=mochi_frames[idx];
  for(int y=0;y<MOCHI_H;y++){ const uint8_t*row=f+(y*MOCHI_W)/8; lv_color_t*dst=mochiBuf+y*MOCHI_W;
    for(int x=0;x<MOCHI_W;x++){ bool on=(row[x>>3]>>(7-(x&7)))&1; dst[x]=on?color:lv_color_black(); } }
  lv_obj_invalidate(mochiCanvas);
}
static void mochiTick(uint32_t now,bool linked,bool justLinked,const Tele&p){
  static uint8_t st=255,frame=0; static uint32_t frameAt=0,nextBlink=0;
  if(!setMochi){ drawMochiFrame(12,linked?accentColor:lv_color_hex(0x606060)); return; }
  if(st==255||justLinked){ st=0; frame=0; frameAt=now; }
  if(mochiJumpQueued){ mochiJumpQueued=false; if(st!=0){ st=3; frame=29; frameAt=now; } }
  if(!linked){ st=1; frame=12; }
  else switch(st){
    case 0: if(now-frameAt>=MOCHI_APPEAR_MS){frameAt=now;frame++;if(frame>11){st=1;nextBlink=now+1500;}} break;
    case 1: if(p.gear==6){st=6;frame=47;frameAt=now;} else if(p.rpm>=MOCHI_ANGRY_RPM_START){st=4;frame=40;} else if(now>=nextBlink){st=2;frame=16;frameAt=now;} else frame=12; break;
    case 2: if(now-frameAt>=MOCHI_BLINK_MS){frameAt=now;frame++;if(frame>26){st=1;nextBlink=now+2000+(now%3000);}} break;
    case 3: if(now-frameAt>=MOCHI_JUMP_MS){frameAt=now;frame++;if(frame>35)st=1;} break;
    case 4: if(p.gear==6){st=6;frame=47;frameAt=now;} else if(p.rpm>=MOCHI_ANGRY_RPM_START){uint8_t l=(p.rpm-MOCHI_ANGRY_RPM_START)/250;if(l>2)l=2;frame=40+l;} else {st=5;frame=43;frameAt=now;} break;
    case 5: if(p.gear==6){st=6;frame=47;frameAt=now;} else if(p.rpm>=MOCHI_ANGRY_RPM_START){st=4;frame=40;} else if(now-frameAt>=MOCHI_RELAX_MS){frameAt=now;frame++;if(frame>46){st=1;nextBlink=now+1500;}} break;
    case 6: if(p.gear!=6){st=1;frame=12;} else if(frame<59&&now-frameAt>=MOCHI_REV_MS){frameAt=now;frame++;} break;
  }
  lv_color_t c; if(!linked)c=lv_color_hex(0x606060); else if(p.rpm>MOCHI_RED_RPM)c=lv_color_hex(0xFF2B2B); else if(p.fuel<MOCHI_LOW_FUEL)c=lv_color_hex(0xFFC400); else c=accentColor;
  drawMochiFrame(frame,c);
}
static void buildMochi(lv_obj_t*scr){
  makeLinkDot(scr);
  for(int i=0;i<MOCHI_W*MOCHI_H;i++)mochiBuf[i]=lv_color_black();
  mochiCanvas=lv_canvas_create(scr);
  lv_canvas_set_buffer(mochiCanvas,mochiBuf,MOCHI_W,MOCHI_H,LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(mochiCanvas,LV_ALIGN_CENTER,0,0);
}

// ---------------- медиа (ДИНАМИЧЕСКИЙ буфер) ----------------
static void mediaClose(){
  if(mediaOpen){ mediaFile.close(); mediaOpen=false; }
  mediaCanvas=NULL; mediaFrames=0;
  if(mediaBuf){ free(mediaBuf); mediaBuf=nullptr; }
}
static bool mediaOpenFile(){
  if(!LittleFS.begin(false))return false;
  mediaFile=LittleFS.open(MEDIA_PATH,"r");
  if(!mediaFile)return false;
  uint8_t h[16];
  if(mediaFile.read(h,16)!=16||memcmp(h,"GIMG",4)!=0||h[4]!=2){mediaFile.close();return false;}
  mediaFrames=h[5]; mediaFps=h[6]; if(!mediaFps)mediaFps=10; if(mediaFps>12)mediaFps=12;
  if(!mediaFrames||mediaFrames>MEDIA_MAX_FRAMES){mediaFile.close();mediaFrames=0;return false;}
  const uint32_t fs=240u*240u*2u;
  if((uint32_t)mediaFile.size()<16u+mediaFrames*fs){mediaFile.close();mediaFrames=0;return false;}
  if(!mediaBuf) mediaBuf=(lv_color_t*)malloc(fs);
  if(!mediaBuf){ mediaFile.close(); mediaFrames=0; return false; }
  mediaOpen=true; mediaCur=0; mediaFrameAt=0;
  return true;
}
static bool mediaDrawFrame(uint8_t f){
  if(!mediaOpen||!mediaBuf)return false;
  const uint32_t fs=240u*240u*2u;
  mediaFile.seek(16u+(uint32_t)f*fs);
  if(mediaFile.read((uint8_t*)mediaBuf,fs)!=fs)return false;
  if(mediaCanvas)lv_obj_invalidate(mediaCanvas);
  return true;
}
static void mediaTick(uint32_t now){
  if(!mediaOpen||mediaFrames<2)return;
  if(now-mediaFrameAt>=1000/mediaFps){ mediaFrameAt=now; mediaCur=(mediaCur+1)%mediaFrames; mediaDrawFrame(mediaCur); }
}
static void buildMedia(lv_obj_t*scr){
  mediaClose();
  if(!mediaOpenFile()){
    lv_obj_t*l=lv_label_create(scr);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0);
    lv_obj_set_style_text_color(l,lv_color_hex(0x808080),0);
    lv_label_set_text(l,"UPLOAD MEDIA"); lv_obj_center(l);
    return;
  }
  mediaCanvas=lv_canvas_create(scr);
  lv_canvas_set_buffer(mediaCanvas,mediaBuf,240,240,LV_IMG_CF_TRUE_COLOR);
  lv_obj_center(mediaCanvas);
  mediaDrawFrame(0); mediaFrameAt=millis();
}

// ---------------- экраны ----------------
static void buildGear(lv_obj_t*scr){
  makeRpmArc(scr); makeLinkDot(scr);
  lblMain=lv_label_create(scr); lv_obj_set_style_text_font(lblMain,&gear72,0); lv_obj_set_style_text_color(lblMain,accentColor,0);
  lv_label_set_text(lblMain,"--"); lv_obj_align(lblMain,LV_ALIGN_CENTER,0,12);
  sideAnim[0].lbl=lblPrev=lv_label_create(scr); sideAnim[1].lbl=lblNext=lv_label_create(scr);
  for(int i=0;i<2;i++){ lv_obj_set_style_text_font(sideAnim[i].lbl,&lv_font_montserrat_24,0); lv_obj_set_style_text_color(sideAnim[i].lbl,lv_color_hex(0x909090),0); lv_label_set_text(sideAnim[i].lbl,"-"); sideAnim[i].cx=(i==0)?42:198; sideAnim[i].cy=147; sidePlace(&sideAnim[i]); }
  lblSub=lv_label_create(scr); lv_obj_set_style_text_font(lblSub,&lv_font_montserrat_14,0); lv_obj_set_style_text_color(lblSub,lv_color_hex(0x808080),0);
  lv_label_set_text(lblSub,"WAITING..."); lv_obj_align(lblSub,LV_ALIGN_BOTTOM_MID,0,-12);
}
static void buildSpeed(lv_obj_t*scr){
  makeRpmArc(scr); makeLinkDot(scr);
  lblMain=lv_label_create(scr); lv_obj_set_style_text_font(lblMain,&gear72,0); lv_obj_set_style_text_color(lblMain,accentColor,0);
  lv_label_set_text(lblMain,"--"); lv_obj_align(lblMain,LV_ALIGN_CENTER,0,3);
  lblSub=lv_label_create(scr); lv_obj_set_style_text_font(lblSub,&lv_font_montserrat_14,0); lv_obj_set_style_text_color(lblSub,lv_color_hex(0x808080),0);
  lv_label_set_text(lblSub,"km/h"); lv_obj_align(lblSub,LV_ALIGN_CENTER,0,75);
}
static void buildRpm(lv_obj_t*scr){
  makeRpmArc(scr); makeLinkDot(scr);
  lblMain=lv_label_create(scr); lv_obj_set_style_text_font(lblMain,&lv_font_montserrat_48,0); lv_obj_set_style_text_color(lblMain,accentColor,0);
  lv_label_set_text(lblMain,"0"); lv_obj_align(lblMain,LV_ALIGN_CENTER,0,6);
  lblSub=lv_label_create(scr); lv_obj_set_style_text_font(lblSub,&lv_font_montserrat_14,0); lv_obj_set_style_text_color(lblSub,lv_color_hex(0x808080),0);
  lv_label_set_text(lblSub,"RPM"); lv_obj_align(lblSub,LV_ALIGN_CENTER,0,75);
}
static void buildMulti(lv_obj_t*scr){
  makeRpmArc(scr); makeLinkDot(scr);
  lblRow1=lv_label_create(scr); lv_obj_set_style_text_font(lblRow1,&lv_font_montserrat_20,0); lv_label_set_text(lblRow1,"--- RPM"); lv_obj_align(lblRow1,LV_ALIGN_CENTER,0,-18);
  lblRow2=lv_label_create(scr); lv_obj_set_style_text_font(lblRow2,&lv_font_montserrat_20,0); lv_label_set_text(lblRow2,"-- km/h"); lv_obj_align(lblRow2,LV_ALIGN_CENTER,0,27);
  lblRow3=lv_label_create(scr); lv_obj_set_style_text_font(lblRow3,&lv_font_montserrat_20,0); lv_label_set_text(lblRow3,"-- C"); lv_obj_align(lblRow3,LV_ALIGN_CENTER,0,72);
}
static void buildVoltageFuel(lv_obj_t*scr){
  makeRpmArc(scr); makeLinkDot(scr);
  lblVoltage=lv_label_create(scr); lv_obj_set_style_text_font(lblVoltage,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(lblVoltage,lv_color_white(),0); lv_label_set_text(lblVoltage,"--.-- V"); lv_obj_align(lblVoltage,LV_ALIGN_CENTER,0,-18);
  lblFuel=lv_label_create(scr); lv_obj_set_style_text_font(lblFuel,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(lblFuel,lv_color_white(),0); lv_label_set_text(lblFuel,"FUEL --%"); lv_obj_align(lblFuel,LV_ALIGN_CENTER,0,27);
  lblVoltSub=lv_label_create(scr); lv_obj_set_style_text_font(lblVoltSub,&lv_font_montserrat_14,0); lv_obj_set_style_text_color(lblVoltSub,lv_color_hex(0x808080),0); lv_label_set_text(lblVoltSub,"VOLTAGE / FUEL"); lv_obj_align(lblVoltSub,LV_ALIGN_CENTER,0,72);
}
static void buildServiceScreen(lv_obj_t*scr){
  lv_obj_t*t1=lv_label_create(scr);
  lv_obj_set_style_text_font(t1,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(t1,accentColor,0);
  lv_label_set_text(t1,"SERVICE MODE");
  lv_obj_align(t1,LV_ALIGN_TOP_MID,0,22);
  lv_obj_t*a1=lv_label_create(scr);
  lv_obj_set_style_text_font(a1,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_color(a1,lv_color_hex(0x909090),0);
  lv_label_set_text(a1,"1. Wi-Fi:");
  lv_obj_align(a1,LV_ALIGN_TOP_MID,0,52);
  lv_obj_t*s1=lv_label_create(scr);
  lv_obj_set_style_text_font(s1,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(s1,lv_color_hex(0x00E500),0);
  lv_label_set_text_fmt(s1,"%s",apSSID);
  lv_obj_align(s1,LV_ALIGN_TOP_MID,0,72);
  lv_obj_t*a2=lv_label_create(scr);
  lv_obj_set_style_text_font(a2,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_color(a2,lv_color_hex(0x909090),0);
  lv_label_set_text(a2,"2. Password:");
  lv_obj_align(a2,LV_ALIGN_TOP_MID,0,102);
  lv_obj_t*s2=lv_label_create(scr);
  lv_obj_set_style_text_font(s2,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(s2,lv_color_hex(0xFFC400),0);
  lv_label_set_text_fmt(s2,"%s",apPass);
  lv_obj_align(s2,LV_ALIGN_TOP_MID,0,122);
  lv_obj_t*a3=lv_label_create(scr);
  lv_obj_set_style_text_font(a3,&lv_font_montserrat_14,0);
  lv_obj_set_style_text_color(a3,lv_color_hex(0x909090),0);
  lv_label_set_text(a3,"3. Browser:");
  lv_obj_align(a3,LV_ALIGN_TOP_MID,0,152);
  lv_obj_t*s3=lv_label_create(scr);
  lv_obj_set_style_text_font(s3,&lv_font_montserrat_24,0);
  lv_obj_set_style_text_color(s3,accentColor,0);
  lv_label_set_text(s3,"192.168.4.1");
  lv_obj_align(s3,LV_ALIGN_TOP_MID,0,176);
}
static void applyScreen(){
  lv_obj_t*scr=lv_scr_act();
  if(serviceOn&&curScreen!=6)stopService();
  mediaClose();
  lv_anim_del(&sideAnim[0],NULL); lv_anim_del(&sideAnim[1],NULL);
  lv_obj_clean(scr); lv_obj_set_style_bg_color(scr,lv_color_black(),0);
  rpmArc=NULL;linkDot=NULL;lblMain=NULL;lblSub=NULL;lblPrev=NULL;lblNext=NULL;
  lblRow1=NULL;lblRow2=NULL;lblRow3=NULL;lblVoltage=NULL;lblFuel=NULL;lblVoltSub=NULL;mochiCanvas=NULL;
  lastDrawnRpm=65535;lastDrawnSpeed=65535;lastDrawnGear=255;lastDrawnCoolant=255;lastDrawnVoltage=65535;lastDrawnFuel=255;
  lastLinkState=-1;lastSidePrev=255;lastSideNext=255;lastGearForDir=255;
  if(curScreen==0)buildGear(scr);
  else if(curScreen==1)buildSpeed(scr);
  else if(curScreen==2)buildRpm(scr);
  else if(curScreen==3)buildMulti(scr);
  else if(curScreen==4)buildMochi(scr);
  else if(curScreen==5)buildVoltageFuel(scr);
  else if(curScreen==6)buildServiceScreen(scr);
  else if(curScreen==7)buildMedia(scr);
  applyBrightness();
  if(curScreen==6)startService();
}

// ---------------- refresh ----------------
static void refreshUI(){
  static bool linkedOnce=false; static uint32_t greenStart=0,lastSmooth=0;
  Tele p; if(xSemaphoreTake(pktMux,portMAX_DELAY)==pdTRUE){p=pkt;xSemaphoreGive(pktMux);}
  uint32_t now=millis();
  bool linked=(lastDataMs!=0)&&(now-lastDataMs<LINK_TIMEOUT_MS);
  bool justLinked=linked&&!linkedOnce;
  float dt=(lastSmooth==0)?PKT_POLL_MS:(float)(now-lastSmooth); lastSmooth=now; if(dt>150)dt=150;
  shownRpm=moveToValue(shownRpm,(float)p.rpm,dt,RPM_MOVE_MS,RPM_SNAP_DELTA);
  shownSpeed=moveToValue(shownSpeed,(float)p.speed,dt,SPEED_MOVE_MS,SPEED_SNAP_DELTA);
  uint16_t ri=(uint16_t)(shownRpm+0.5f), si=(uint16_t)(shownSpeed+0.5f); if(ri>RPM_MAX)ri=RPM_MAX;
  if(justLinked){greenStart=now;linkedOnce=true;}
  if(!linked)linkedOnce=false;
  if(linked){ if(prevGear<=6&&p.gear<=6&&p.gear!=prevGear)mochiJumpQueued=true; prevGear=p.gear; } else prevGear=255;
  int ls=!linked?0:((now-greenStart<3000)?1:2);
  if(linkDot&&ls!=lastLinkState){ lv_obj_set_style_bg_color(linkDot,(ls==0)?lv_color_hex(0xFF2B2B):(ls==1?lv_color_hex(0x00E500):lv_color_black()),0); lastLinkState=ls; }
  if(rpmArc&&ri!=lastDrawnRpm){ lv_arc_set_value(rpmArc,ri); lv_obj_set_style_arc_color(rpmArc,rpmColor(ri),LV_PART_INDICATOR); }
  lv_color_t stale=lv_color_hex(0xFF4040);
  if(curScreen==0&&lblMain){
    if(!linked&&lastDataMs!=0){
      if(lastLinkState!=0||lastDrawnGear!=254){ lv_label_set_text(lblMain,"--"); lv_obj_set_style_text_color(lblMain,stale,0); lv_label_set_text(lblSub,"NO SIGNAL"); lastDrawnGear=254; }
    } else if(p.gear!=lastDrawnGear||lastLinkState==1){
      const char*txt,*info; lv_color_t col;
      if(p.gear==255){txt="--";col=lv_color_hex(0x404040);info="WAITING...";}
      else if(p.gear==0){txt="N";col=lv_color_hex(0xC0C0C0);info="NEUTRAL";}
      else if(p.gear<=5){static char g[2];g[0]='0'+p.gear;g[1]=0;txt=g;col=accentColor;info="";}
      else if(p.gear==6){txt="R";col=lv_color_hex(0xFF2B2B);info="REVERSE";}
      else{txt="?";col=lv_color_hex(0xFFC400);info="TRANSITION";}
      lv_label_set_text(lblMain,txt); lv_obj_set_style_text_color(lblMain,col,0); lv_label_set_text(lblSub,info); lastDrawnGear=p.gear;
    }
    if(lblPrev&&lblNext){
      int ip=seqIdx(p.gear);
      if(ip>=0&&linked){
        static const uint8_t S[7]={6,0,1,2,3,4,5};
        uint8_t pv=S[(ip+6)%7], nx=S[(ip+1)%7];
        if(pv!=lastSidePrev||nx!=lastSideNext){
          char b[4];
          if(lastSidePrev==255){ gearStr(pv,b);sideSetStatic(&sideAnim[0],b); gearStr(nx,b);sideSetStatic(&sideAnim[1],b); }
          else{ int dir=1; int io=seqIdx(lastGearForDir); if(io>=0&&ip==(io+6)%7)dir=-1; gearStr(pv,b);startSideFlip(&sideAnim[0],b,dir); gearStr(nx,b);startSideFlip(&sideAnim[1],b,dir); }
          lastSidePrev=pv;lastSideNext=nx;
        }
        lastGearForDir=p.gear;
      } else if(lastSidePrev!=254){ sideSetStatic(&sideAnim[0],"-"); sideSetStatic(&sideAnim[1],"-"); lastSidePrev=254;lastSideNext=254; }
    }
  }
  else if(curScreen==1&&lblMain){ if(si!=lastDrawnSpeed){lv_label_set_text_fmt(lblMain,"%u",si);lastDrawnSpeed=si;} lv_obj_set_style_text_color(lblMain,linked?accentColor:stale,0); }
  else if(curScreen==2&&lblMain){ if(ri!=lastDrawnRpm)lv_label_set_text_fmt(lblMain,"%u",ri); lv_obj_set_style_text_color(lblMain,linked?accentColor:stale,0); }
  else if(curScreen==3&&lblRow1){
    if(ri!=lastDrawnRpm)lv_label_set_text_fmt(lblRow1,"%u RPM",ri);
    lv_obj_set_style_text_color(lblRow1,linked?rpmColor(ri):stale,0);
    if(si!=lastDrawnSpeed){lv_label_set_text_fmt(lblRow2,"%u km/h",si);lastDrawnSpeed=si;}
    lv_obj_set_style_text_color(lblRow2,linked?accentColor:stale,0);
    if(p.coolant!=lastDrawnCoolant){lv_label_set_text_fmt(lblRow3,"%u C",p.coolant);lastDrawnCoolant=p.coolant;}
    lv_obj_set_style_text_color(lblRow3,linked?tempColor(p.coolant):stale,0);
  }
  else if(curScreen==4&&mochiCanvas)mochiTick(now,linked,justLinked,p);
  else if(curScreen==5&&lblVoltage&&lblFuel){
    if(!linked&&lastDataMs!=0){ if(lastLinkState!=0){ lv_label_set_text(lblVoltage,"--.-- V"); lv_label_set_text(lblFuel,"FUEL --%"); lv_obj_set_style_text_color(lblVoltage,stale,0); lv_obj_set_style_text_color(lblFuel,stale,0); } }
    else{
      if(p.voltage!=lastDrawnVoltage){ lv_label_set_text_fmt(lblVoltage,"%u.%02u V",p.voltage/1000,(p.voltage%1000)/10); lv_obj_set_style_text_color(lblVoltage,voltageColor(p.voltage),0); lastDrawnVoltage=p.voltage; }
      if(p.fuel!=lastDrawnFuel){ lv_label_set_text_fmt(lblFuel,"FUEL %u%%",p.fuel); lv_obj_set_style_text_color(lblFuel,fuelColor(p.fuel),0); lastDrawnFuel=p.fuel; }
    }
  }
  lastDrawnRpm=ri;
}

// ---------------- заставка ----------------
static lv_obj_t*splashImg=NULL;
static void set_img_opa(void*o,int32_t v){ lv_obj_set_style_img_opa((lv_obj_t*)o,(lv_opa_t)v,0); }
static void buildSplash(){
  lv_obj_t*scr=lv_scr_act(); lv_obj_set_style_bg_color(scr,lv_color_black(),0);
  splashImg=lv_img_create(scr); lv_img_set_src(splashImg,&img_logo); lv_img_set_zoom(splashImg,LOGO_ZOOM);
  lv_obj_center(splashImg); lv_obj_set_style_img_opa(splashImg,0,0);
  lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a,splashImg); lv_anim_set_values(&a,0,255); lv_anim_set_time(&a,SPLASH_FADE_MS); lv_anim_set_exec_cb(&a,set_img_opa); lv_anim_start(&a);
}

// ---------------- WEB API ----------------
static void apiTelemetry() {
  Tele p; if (xSemaphoreTake(pktMux, portMAX_DELAY) == pdTRUE) { p = pkt; xSemaphoreGive(pktMux); }
  bool linked = (lastDataMs != 0) && (millis() - lastDataMs < LINK_TIMEOUT_MS);
  char g[4]; if (!linked) strcpy(g, "--"); else gearStr(p.gear, g);
  char b[320];
  snprintf(b, sizeof(b),
    "{\"gear\":\"%s\",\"rpm\":%u,\"speed\":%u,\"temp\":%u,\"voltage\":%u,\"fuel\":%u,\"linked\":%s,\"k\":%.1f,\"rec\":%d,\"prog\":%d,\"fd\":%d,\"sensors\":[0,0,0,0,0,0]}",
    g, p.rpm, p.speed, p.coolant, p.voltage, p.fuel, linked ? "true" : "false",
    liveK, (int)recGear, recProg, fdActive ? 1 : 0);
  web->send(200, "application/json", b);
}
static void apiSettingsGet(){
  char b[220];
  snprintf(b,sizeof(b),"{\"accent_color\":\"#%06X\",\"units\":\"%s\",\"brightness\":%u,\"mochi_enabled\":%s,\"splash_enabled\":%s,\"language\":\"%s\"}",
    (unsigned)accentHex,setUnits?"imperial":"metric",setBright,setMochi?"true":"false",setSplash?"true":"false",setLang?"en":"ru");
  web->send(200,"application/json",b);
}
static void apiSettingsPost(){
  String b=web->arg("plain");
  String acc=jsonStr(b,"accent_color"); if(acc.length()>=7)setAccentHex((uint32_t)strtoul(acc.c_str()+1,NULL,16));
  String un=jsonStr(b,"units"); if(un.length())setUnits=(un=="imperial")?1:0;
  int br=jsonInt(b,"brightness",-1); if(br>=0)setBright=(uint8_t)br;
  setMochi=(uint8_t)jsonBool(b,"mochi_enabled",setMochi);
  setSplash=(uint8_t)jsonBool(b,"splash_enabled",setSplash);
  String lg=jsonStr(b,"language"); if(lg.length())setLang=(lg=="en")?1:0;
  saveExtras(); applyBrightness();
  web->send(200,"application/json","{\"success\":true}");
}
static void apiInfo(){
  char b[260];
  snprintf(b,sizeof(b),"{\"firmware_version\":\"%s\",\"sensor_version\":\"ratio\",\"device_name\":\"Smart Gauge 2424S012\",\"ip\":\"%s\",\"uptime\":%lu,\"free_heap\":%u}",
    FW_VERSION,WiFi.softAPIP().toString().c_str(),(unsigned long)(millis()/1000),(unsigned)ESP.getFreeHeap());
  web->send(200,"application/json",b);
}
static void apiReboot(){ web->send(200,"application/json","{\"success\":true}"); delay(400); ESP.restart(); }
static void apiFactory(){ web->send(200,"application/json","{\"success\":true}"); delay(200); Preferences pr; pr.begin("gearui",false); pr.clear(); pr.end(); pr.begin("gear2",false); pr.clear(); pr.end(); delay(200); ESP.restart(); }
static void apiWifiPass(){
  String p=jsonStr(web->arg("plain"),"password");
  if(p.length()<8){web->send(400,"application/json","{\"error\":\"short\"}");return;}
  strncpy(apPass,p.c_str(),15); apPass[15]=0; saveExtras();
  if(serviceOn)WiFi.softAP(apSSID,apPass,1);
  web->send(200,"application/json","{\"success\":true}");
}
static void apiTrGet() {
  char b[420];
  int n = snprintf(b, sizeof(b),
    "{\"k\":[0,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f],\"mask\":%u,\"tol\":%.2f,\"rmax\":%u,\"rec\":%d,\"prog\":%d,\"liveK\":%.1f,\"fd\":%d,\"prop\":[",
    trK[1], trK[2], trK[3], trK[4], trK[5], trK[6], trMask, trTol, trRmax, (int)recGear, recProg, liveK, fdActive ? 1 : 0);
  for (uint8_t i = 0; i < fdPropN; i++) n += snprintf(b + n, sizeof(b) - n, "%s%.2f", i ? "," : "", fdProp[i]);
  snprintf(b + n, sizeof(b) - n, "]}");
  web->send(200, "application/json", b);
}
static void apiTrRecord() {
  int g = jsonInt(web->arg("plain"), "gear", -1);
  if (g < 1 || g > 6) { web->send(400, "application/json", "{\"error\":\"bad gear\"}"); return; }
  fdActive = false; fdPropN = 0;
  recBegin((uint8_t)g);
  web->send(200, "application/json", "{\"success\":true}");
}
static void apiTrStop() {
  recAbort();
  if (fdActive) fdFinish();
  web->send(200, "application/json", "{\"success\":true}");
}
static void apiTrFreeDrive() {
  String a = jsonStr(web->arg("plain"), "action");
  if (a == "start") { recAbort(); fdCount = 0; fdPropN = 0; fdActive = true; }
  else if (a == "stop") { if (fdActive) fdFinish(); }
  else if (a == "confirm") { fdConfirm(); }
  else { web->send(400, "application/json", "{\"error\":\"bad action\"}"); return; }
  web->send(200, "application/json", "{\"success\":true}");
}
static void apiTrSet(){
  String b=web->arg("plain");
  int g=jsonInt(b,"gear",-1); float k=jsonFloat(b,"k",0);
  if(g<1||g>6||k<=0){web->send(400,"application/json","{\"error\":\"bad args\"}");return;}
  trK[g]=k; trMask|=(uint8_t)(1<<g); trSave();
  web->send(200,"application/json","{\"success\":true}");
}
static void apiTrReset(){
  int g=jsonInt(web->arg("plain"),"gear",255);
  if(g>=1&&g<=6){ trMask&=(uint8_t)~(1<<g); trK[g]=0; }
  else { trMask=0; memset(trK,0,sizeof(trK)); }
  trSave();
  web->send(200,"application/json","{\"success\":true}");
}
static void apiTrParams(){
  String b=web->arg("plain");
  float tol=jsonFloat(b,"tol",-1); if(tol>0.02f&&tol<0.4f)trTol=tol;
  int rm=jsonInt(b,"rmax",-1); if(rm>=5&&rm<=40)trRmax=(uint8_t)rm;
  trSave();
  web->send(200,"application/json","{\"success\":true}");
}
static void apiBtScan(){ btStartScan(); web->send(200,"application/json","{\"ok\":true}"); }
static void apiBtList(){
  String j="{\"scanning\":"; j+=btScanActive()?"true":"false"; j+=",\"devices\":[";
  for(uint8_t i=0;i<btDevCount;i++){
    if(i)j+=",";
    j+="{\"mac\":\""+String(btDevs[i].mac)+"\",\"name\":\""+String(btDevs[i].name)+"\"}";
  }
  j+="]}";
  web->send(200,"application/json",j);
}
static void apiBtStatus(){
  char b[200];
  snprintf(b,sizeof(b),"{\"connected\":%s,\"mac\":\"%s\",\"pending\":false,\"lastOk\":null}",
    bleLinked?"true":"false",bleMac);
  web->send(200,"application/json",b);
}
static void apiBtConnect(){
  String m=jsonStr(web->arg("plain"),"mac"); m.toUpperCase();
  if(m.length()!=17){web->send(400,"application/json","{\"error\":\"bad mac\"}");return;}
  bool ok=bleConnectTo(m.c_str())&&obdInitSeq();
  if(ok)btSaveCreds(m.c_str());
  web->send(200,"application/json",ok?"{\"success\":true}":"{\"error\":\"connect fail\"}");
}
static void apiBtDisconnect(){
  bleLinked=false;
  if(pClient&&pClient->isConnected())pClient->disconnect();
  web->send(200,"application/json","{\"ok\":true}");
}
static void apiMediaList(){
  bool ex=LittleFS.begin(false)&&LittleFS.exists(MEDIA_PATH);
  if(!ex){web->send(200,"application/json","{\"files\":[]}");return;}
  File f=LittleFS.open(MEDIA_PATH,"r"); uint32_t sz=f?f.size():0; if(f)f.close();
  char b[160]; snprintf(b,sizeof(b),"{\"files\":[{\"name\":\"media.gimg\",\"size\":%u,\"type\":\"gimg\"}]}",sz);
  web->send(200,"application/json",b);
}
static File upFile; static uint32_t upSize=0; static bool upOk=false;
static void mediaUploadCb(){
  HTTPUpload&up=web->upload();
  if(up.status==UPLOAD_FILE_START){
    upOk=false;upSize=0;
    if(LittleFS.totalBytes()==0)return;
    LittleFS.remove(MEDIA_PATH);
    upFile=LittleFS.open(MEDIA_PATH,"w");
  } else if(up.status==UPLOAD_FILE_WRITE){
    if(upFile){ upSize+=upFile.write(up.buf,up.currentSize); delay(1);
      if(upSize>600000){upFile.close();LittleFS.remove(MEDIA_PATH);} }
  } else if(up.status==UPLOAD_FILE_END){
    if(upFile){ upFile.close();
      File f=LittleFS.open(MEDIA_PATH,"r");
      if(f){ uint8_t h[16]={0}; size_t rd=f.read(h,16); uint32_t fsz=f.size(); f.close();
        uint32_t need=16u+(uint32_t)h[5]*115200u;
        if(rd==16&&!memcmp(h,"GIMG",4)&&h[4]==2&&h[5]>=1&&h[5]<=MEDIA_MAX_FRAMES&&fsz>=need)upOk=true;
        else LittleFS.remove(MEDIA_PATH); } }
  } else if(up.status==UPLOAD_FILE_ABORTED){ if(upFile)upFile.close(); LittleFS.remove(MEDIA_PATH); }
}
static void apiMediaPost(){
  if(upOk){ char b[120]; snprintf(b,sizeof(b),"{\"success\":true,\"filename\":\"media.gimg\",\"size\":%u}",(unsigned)upSize); web->send(200,"application/json",b); }
  else web->send(500,"application/json","{\"error\":\"bad media\"}");
}
static void apiMediaDelete(){ LittleFS.begin(false); LittleFS.remove(MEDIA_PATH); web->send(200,"application/json","{\"success\":true}"); }
static void otaUpload(){
  HTTPUpload&up=web->upload();
  if(up.status==UPLOAD_FILE_START)Update.begin(UPDATE_SIZE_UNKNOWN);
  else if(up.status==UPLOAD_FILE_WRITE)Update.write(up.buf,up.currentSize);
  else if(up.status==UPLOAD_FILE_END)Update.end(true);
}
static void apiOta(){
  if(Update.hasError()){web->send(500,"application/json","{\"success\":false}");return;}
  web->send(200,"application/json","{\"success\":true}"); delay(400); ESP.restart();
}
static void setupWebRoutes(){
  web->on("/",HTTP_GET,[]{web->send_P(200,"text/html; charset=utf-8",(const char*)INDEX_HTML,INDEX_HTML_LEN);});
  web->on("/index.html",HTTP_GET,[]{web->send_P(200,"text/html; charset=utf-8",(const char*)INDEX_HTML,INDEX_HTML_LEN);});
  web->on("/styles.css",HTTP_GET,[]{web->send_P(200,"text/css",(const char*)STYLES_CSS,STYLES_CSS_LEN);});
  web->on("/i18n.js",HTTP_GET,[]{web->send_P(200,"application/javascript",(const char*)I18N_JS,I18N_JS_LEN);});
  web->on("/app.js",HTTP_GET,[]{web->send_P(200,"application/javascript",(const char*)APP_JS,APP_JS_LEN);});
  web->on("/api/telemetry",HTTP_GET,apiTelemetry);
  web->on("/api/settings",HTTP_GET,apiSettingsGet);
  web->on("/api/settings",HTTP_POST,apiSettingsPost);
  web->on("/api/info",HTTP_GET,apiInfo);
  web->on("/api/reboot",HTTP_POST,apiReboot);
  web->on("/api/factory_reset",HTTP_POST,apiFactory);
  web->on("/api/wifi/password",HTTP_POST,apiWifiPass);
  web->on("/api/transmission",HTTP_GET,apiTrGet);
  web->on("/api/transmission/record",HTTP_POST,apiTrRecord);
  web->on("/api/transmission/stop",HTTP_POST,apiTrStop);
  web->on("/api/transmission/freedrive",HTTP_POST,apiTrFreeDrive);
  web->on("/api/transmission/set",HTTP_POST,apiTrSet);
  web->on("/api/transmission/reset",HTTP_POST,apiTrReset);
  web->on("/api/transmission/params",HTTP_POST,apiTrParams);
  web->on("/api/bt/scan",HTTP_POST,apiBtScan);
  web->on("/api/bt/list",HTTP_GET,apiBtList);
  web->on("/api/bt/status",HTTP_GET,apiBtStatus);
  web->on("/api/bt/connect",HTTP_POST,apiBtConnect);
  web->on("/api/bt/disconnect",HTTP_POST,apiBtDisconnect);
  web->on("/api/media/list",HTTP_GET,apiMediaList);
  web->on("/api/media",HTTP_POST,apiMediaPost,mediaUploadCb);
  web->on("/api/media/delete",HTTP_POST,apiMediaDelete);
  web->on("/api/ota",HTTP_POST,apiOta,otaUpload);
  web->onNotFound([]{ String u=web->uri(); if(u.startsWith("/api/")){web->send(404,"application/json","{\"error\":\"nf\"}");return;} web->send_P(200,"text/html; charset=utf-8",(const char*)INDEX_HTML,INDEX_HTML_LEN); });
}
static void startService(){
  if(serviceOn)return;
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(apSSID, apPass, 1);
  Serial.print("AP start: "); Serial.println(apOk ? "OK" : "FAIL");
  Serial.print("heap before web: "); Serial.println(ESP.getFreeHeap());
  delay(300);
  web=new WebServer(80); setupWebRoutes(); web->begin();
  dns=new DNSServer(); dns->start(53,"*",WiFi.softAPIP());
  MDNS.begin("gearui"); MDNS.addService("http","tcp",80);
  serviceOn=true;
  Serial.print("SERVICE ON: "); Serial.print(apSSID);
  Serial.print(" IP="); Serial.print(WiFi.softAPIP());
  Serial.print(" heap="); Serial.println(ESP.getFreeHeap());
}
static void stopService(){
  if(!serviceOn)return;
  web->stop(); delete web; web=nullptr;
  dns->stop(); delete dns; dns=nullptr;
  MDNS.end(); WiFi.mode(WIFI_STA); delay(100);
  serviceOn=false;
  Serial.println("SERVICE OFF");
}

// ---------------- setup / loop ----------------
static lv_color_t buf[240*40];
static lv_disp_draw_buf_t draw_buf;
void setup(){
  Serial.begin(115200);
  pinMode(BTN_PIN,INPUT_PULLUP);
  pinMode(TFT_BL,OUTPUT); analogWrite(TFT_BL,0);
  if (digitalRead(BTN_PIN)==LOW) { delay(1200); bootHold = (digitalRead(BTN_PIN)==LOW); }
  pktMux=xSemaphoreCreateMutex();
  uint64_t mac=ESP.getEfuseMac();
  snprintf(apSSID,sizeof(apSSID),"GEAR-UI-%04X",(unsigned)(mac&0xFFFF));
  Serial.print("heap boot KB: "); Serial.println(ESP.getFreeHeap()/1024);
  if(LittleFS.begin(true)){ Serial.print("LittleFS KB: "); Serial.println((unsigned)(LittleFS.totalBytes()/1024)); }
  else Serial.println("LittleFS: NO PARTITION");
  trLoad();
  btLoadCreds();
  BLEDevice::init("");
  initDisplay();
  lv_init();
  lv_disp_draw_buf_init(&draw_buf,buf,NULL,240*40);
  static lv_disp_drv_t drv; lv_disp_drv_init(&drv);
  drv.draw_buf=&draw_buf; drv.flush_cb=flush_cb; drv.hor_res=240; drv.ver_res=240; drv.sw_rotate=1;
  lv_disp_drv_register(&drv);
  loadUiState();
  if (bootHold) { curScreen=0; rotIdx=0; saveUiState(); Serial.println("BOOT HOLD: screen reset to 0"); }
  applyAccentColor(); applyBrightness(); applyRotation();
  if(setSplash){buildSplash();splashStart=millis();} else {splashDone=true;applyScreen();}
  xTaskCreatePinnedToCore(obdTask,"obd",8192,nullptr,2,nullptr,0);
  Serial.print("heap after init KB: "); Serial.println(ESP.getFreeHeap()/1024);
  Serial.print("Ready FW v"); Serial.println(FW_VERSION);
}
void loop(){
  static uint32_t tUi=0; static bool btnWas=false,btnLong=false; static uint32_t btnAt=0;
  static String serBuf="";
  static uint8_t lastStations=0;
  uint32_t now=millis();
  while(Serial.available()){
    char c=(char)Serial.read();
    if(c==10||c=='\r'){ if(serBuf.length()){ serBuf.trim();
      if(serBuf.startsWith("accent=")||serBuf.startsWith("accent ")){const char*p=serBuf.c_str()+6;while(*p=='='||*p==' ')p++;setAccentHex((uint32_t)strtoul(p,NULL,16));}
      serBuf=""; } }
    else if(serBuf.length()<32)serBuf+=c;
  }
  if(serviceOn){
    web->handleClient(); dns->processNextRequest();
    uint8_t st=WiFi.softAPgetStationNum();
    if(st!=lastStations){ lastStations=st; Serial.print("AP stations: "); Serial.println(st); }
  }
  lv_timer_handler();
  if(!splashDone){
    if(!fadeOutStarted&&now-splashStart>=SPLASH_SHOW_MS){fadeOutStarted=true;fadeOutStart=now;
      lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,splashImg);lv_anim_set_values(&a,255,0);lv_anim_set_time(&a,SPLASH_FADE_MS);lv_anim_set_exec_cb(&a,set_img_opa);lv_anim_start(&a);}
    if(fadeOutStarted&&now-fadeOutStart>=SPLASH_FADE_MS+50){splashDone=true;applyScreen();}
  } else {
    Tele p; if(xSemaphoreTake(pktMux,portMAX_DELAY)==pdTRUE){p=pkt;xSemaphoreGive(pktMux);}
    bool linked=(lastDataMs!=0)&&(now-lastDataMs<LINK_TIMEOUT_MS);
    uint8_t g=gearStable(p.rpm,p.speed);
    recTick(p.rpm,p.speed,linked);
    if(xSemaphoreTake(pktMux,0)==pdTRUE){ pkt.gear=linked?g:255; xSemaphoreGive(pktMux); }
    bool bn=(digitalRead(BTN_PIN)==LOW);
    if(bn&&!btnWas){btnAt=now;btnLong=false;}
    if(bn&&!btnLong&&now-btnAt>=LONG_PRESS_MS){btnLong=true;doRotate();}
    if(!bn&&btnWas){ if(!btnLong&&now-btnAt>50){ curScreen=(curScreen+1)%SCREEN_COUNT; applyScreen(); saveUiState(); } }
    btnWas=bn;
    if(curScreen!=6&&curScreen!=7&&now-tUi>=PKT_POLL_MS){tUi=now;refreshUI();}
    if(curScreen==7)mediaTick(now);
  }
  delay(5);
}