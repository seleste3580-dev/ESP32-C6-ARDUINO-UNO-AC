/*
  OS v1.0  -  ESP32-C6 "brain" node
  ------------------------------------------
  Runs on FreeRTOS tasks:  UI/keypad (loop) | net (WiFi, captive portal, web) |
                           link (UART to Uno) | oledB (second screen)
  Needs: Espressif "esp32" board package 3.x, library "U8g2".
  Board: ESP32C6 Dev Module.

  KEYS   2 up   8 down   4 left   6 right   # enter/OK   * back/backspace   5 = OK too
         A home   B change OLED-2 view   C lock door now   D mute/unmute
  TEXT   (WiFi password / PIN) phone-style multi-tap. In text mode:
         # confirm  * backspace  A cancel  B Aa case  C abc/123  D show/hide
  Default PIN: 1234  (change it in Settings!)
  Set OLED2_SHARED_BUS to 1 if both OLEDs share one bus (second one jumpered to 0x3D).
  If your OLEDs are 1.3" SH1106, change the two constructors to U8G2_SH1106_...
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include <stdarg.h>

// ---------------- PINS ----------------
#define OLED_A_SDA 22
#define OLED_A_SCL 23
#define OLED_B_SDA 20
#define OLED_B_SCL 19
#define OLED2_SHARED_BUS 0
#define LINK_RX 21            // <- Uno D3 through 1k/2k divider
#define LINK_TX 18            // -> Uno D2
const uint8_t rowPins[4] = {0, 1, 2, 3};
const uint8_t colPins[4] = {6, 7, 10, 11};

#define UTC_OFFSET_SEC (3 * 3600)   // Nairobi

// ---------------- displays ----------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oledA(U8G2_R0, U8X8_PIN_NONE);
#if OLED2_SHARED_BUS
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oledB(U8G2_R0, U8X8_PIN_NONE);
SemaphoreHandle_t i2cMtx;
#define I2C_LOCK()   xSemaphoreTakeRecursive(i2cMtx, portMAX_DELAY)
#define I2C_UNLOCK() xSemaphoreGiveRecursive(i2cMtx)
#else
U8G2_SSD1306_128X64_NONAME_F_SW_I2C oledB(U8G2_R0, OLED_B_SCL, OLED_B_SDA, U8X8_PIN_NONE);
#define I2C_LOCK()
#define I2C_UNLOCK()
#endif

// ---------------- shared state ----------------
enum { NET_IDLE = 0, NET_CONN, NET_STA, NET_AP };
enum { IP_GATE = 1, IP_DOOR, IP_NEWPIN, IP_WIFIPASS };

SemaphoreHandle_t mtx;
#define LOCK()   xSemaphoreTakeRecursive(mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(mtx)

Preferences prefs;
volatile int dist1 = 999, dist2 = 999;
volatile bool irBlocked = false, doorLocked = true;
volatile uint32_t lastRx = 0;
volatile uint32_t frame = 0;
volatile uint8_t netState = NET_IDLE;
char ipStr[20] = "-";

char logBuf[10][32]; int logCount = 0;
char lastEvent[32] = "Boot";
char cards[16][21]; int nCards = 0;
char wSsid[33], wPass[65];
char adminPin[9] = "1234";
uint8_t relockSec = 5, wallpaper = 0, bright = 2;
bool soundOn = true;

volatile bool enrolling = false;
volatile int enrollResult = 0;
volatile bool reqScan = false, reqPortal = false, reqConnect = false, reqForget = false, scanDone = false;
char newSsid[33], newPass[65], selSsid[33];
char scanSsid[8][33]; int scanCount = 0;

// ---------------- helpers ----------------
void sendLink(const char *s) { LOCK(); Serial1.println(s); UNLOCK(); }
bool linkUp() { return millis() - lastRx < 3000; }

bool getTimeStr(char *o, size_t n) {
  struct tm t;
  if (getLocalTime(&t, 0)) { strftime(o, n, "%H:%M", &t); return true; }
  uint32_t s = millis() / 1000;
  snprintf(o, n, "%02u:%02u", (unsigned)(s / 60 % 100), (unsigned)(s % 60));
  return false;
}

void logEvent(const char *fmt, ...) {
  char m[24];
  va_list a; va_start(a, fmt); vsnprintf(m, sizeof m, fmt, a); va_end(a);
  char t[8]; getTimeStr(t, sizeof t);
  LOCK();
  if (logCount < 10) logCount++;
  for (int i = logCount - 1; i > 0; i--) strcpy(logBuf[i], logBuf[i - 1]);
  snprintf(logBuf[0], sizeof logBuf[0], "%s %s", t, m);
  strlcpy(lastEvent, logBuf[0], sizeof lastEvent);
  UNLOCK();
}

void saveSettings() {
  LOCK();
  prefs.putString("pin", adminPin);
  prefs.putUChar("relock", relockSec);
  prefs.putBool("snd", soundOn);
  prefs.putUChar("wp", wallpaper);
  prefs.putUChar("br", bright);
  UNLOCK();
}
void saveCards() {
  String s;
  for (int i = 0; i < nCards; i++) { s += cards[i]; s += ','; }
  LOCK(); prefs.putString("cards", s); UNLOCK();
}
void loadAll() {
  strlcpy(adminPin, prefs.getString("pin", "1234").c_str(), sizeof adminPin);
  relockSec = prefs.getUChar("relock", 5);
  soundOn = prefs.getBool("snd", true);
  wallpaper = prefs.getUChar("wp", 0);
  bright = prefs.getUChar("br", 2);
  strlcpy(wSsid, prefs.getString("ssid", "").c_str(), sizeof wSsid);
  strlcpy(wPass, prefs.getString("pass", "").c_str(), sizeof wPass);
  String s = prefs.getString("cards", "");
  nCards = 0;
  int st = 0;
  while (st < (int)s.length() && nCards < 16) {
    int e = s.indexOf(',', st);
    if (e < 0) break;
    if (e > st) { strlcpy(cards[nCards++], s.substring(st, e).c_str(), 21); }
    st = e + 1;
  }
}
bool cardKnown(const char *u) {
  bool f = false;
  LOCK();
  for (int i = 0; i < nCards; i++) if (!strcmp(cards[i], u)) { f = true; break; }
  UNLOCK();
  return f;
}
void pushConfig() {
  char b[8];
  snprintf(b, sizeof b, "R%u", relockSec);
  sendLink(b);
  sendLink(soundOn ? "M0" : "M1");
}
void applyBright() {
  const uint8_t lv[3] = {4, 60, 255};
  I2C_LOCK();
  oledA.setContrast(lv[bright]);
  oledB.setContrast(lv[bright]);
  I2C_UNLOCK();
}

// ---------------- link (UART to Uno) ----------------
void onCard(const char *u) {
  const char *sh = u + (strlen(u) > 8 ? strlen(u) - 8 : 0);
  if (enrolling) {
    enrolling = false;
    LOCK();
    if (cardKnown(u)) enrollResult = 2;
    else if (nCards >= 16) enrollResult = 3;
    else { strlcpy(cards[nCards++], u, 21); saveCards(); enrollResult = 1; }
    UNLOCK();
    logEvent("ADD %s", sh);
    sendLink(enrollResult == 1 ? "S1" : "S2");
    return;
  }
  if (cardKnown(u)) { logEvent("OK %s", sh); sendLink("G"); }
  else              { logEvent("NO %s", sh); sendLink("X"); }
}

void parseLine(char *l) {
  lastRx = millis();
  if (l[1] != ':') return;
  char *v = l + 2;
  switch (l[0]) {
    case 'C': onCard(v); break;
    case 'D': { int a, b; if (sscanf(v, "%d,%d", &a, &b) == 2) { dist1 = a; dist2 = b; } } break;
    case 'I': irBlocked = (v[0] == '1'); break;
    case 'B': if (v[0] == '1') { logEvent("EXIT BUTTON"); sendLink("G"); } break;
    case 'S': { bool nl = (v[0] == '1'); if (nl != doorLocked) { doorLocked = nl; logEvent(nl ? "Door locked" : "Door unlocked"); } } break;
    case 'H': pushConfig(); break;
  }
}

void linkTask(void *) {
  char b[40]; int n = 0;
  pushConfig();
  for (;;) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c == '\n') { b[n] = 0; if (n) parseLine(b); n = 0; }
      else if (c != '\r' && n < (int)sizeof b - 1) b[n++] = c;
    }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ---------------- network: STA, captive portal, web ----------------
WebServer server(80);
DNSServer dns;
bool portalOn = false;
uint8_t webFails = 0; uint32_t webLock = 0;

const char DASH[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>Seleste Access</title>
<style>body{font-family:system-ui;background:#0b1220;color:#e5e7eb;max-width:460px;margin:auto;padding:16px}
.card{background:#111a2e;border-radius:12px;padding:14px;margin:10px 0}h1{font-size:20px}.big{font-size:32px;font-weight:800}
input,button{padding:12px;border-radius:8px;border:0;font-size:16px;margin:4px 0;width:100%;box-sizing:border-box}
button{font-weight:700;background:#22c55e}button.l{background:#ef4444;color:#fff}pre{white-space:pre-wrap;font-size:13px;color:#9ca3af}a{color:#93c5fd}</style></head><body>
<h1>Seleste Access</h1>
<div class=card><div class=big id=st>...</div><div id=sn></div></div>
<div class=card><input id=pin type=password inputmode=numeric placeholder="PIN"><button onclick="cmd('unlock')">Unlock door</button><button class=l onclick="cmd('lock')">Lock door</button></div>
<div class=card><b>Events</b><pre id=lg></pre></div><a href=/setup>WiFi setup</a>
<script>
async function r(){try{let j=await(await fetch('/api/status')).json();
st.textContent=j.locked?'LOCKED':'UNLOCKED';st.style.color=j.locked?'#ef4444':'#22c55e';
sn.textContent='Outside '+j.d1+' cm | Inside '+j.d2+' cm | IR '+(j.ir?'blocked':'clear')+' | Uno '+(j.link?'online':'offline')+' | '+j.cards+' cards';
lg.textContent=j.log.join('\n')}catch(e){}}
async function cmd(c){let x=await fetch('/api/'+c,{method:'POST',body:new URLSearchParams({pin:pin.value})});alert(await x.text());r()}
setInterval(r,1500);r();
</script></body></html>)rawliteral";

String esc(const char *s) {
  String o;
  for (; *s; s++) {
    if (*s == '&') o += "&amp;"; else if (*s == '<') o += "&lt;";
    else if (*s == '>') o += "&gt;"; else if (*s == '"') o += "&quot;"; else o += *s;
  }
  return o;
}

void doScan() {
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  scanCount = 0;
  for (int i = 0; i < n && scanCount < 8; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    strlcpy(scanSsid[scanCount++], s.c_str(), 33);
  }
  WiFi.scanDelete();
}

void startPortal() {
  netState = NET_CONN;
  WiFi.mode(WIFI_STA);
  doScan();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("Seleste-Setup");
  vTaskDelay(300 / portTICK_PERIOD_MS);
  dns.start(53, "*", WiFi.softAPIP());
  portalOn = true;
  strlcpy(ipStr, "192.168.4.1", sizeof ipStr);
  netState = NET_AP;
  logEvent("Setup portal on");
}
void stopPortal() { dns.stop(); WiFi.softAPdisconnect(true); portalOn = false; }

bool connectSta(const char *ssid, const char *pass, uint32_t ms) {
  netState = NET_CONN;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < ms) vTaskDelay(200 / portTICK_PERIOD_MS);
  if (WiFi.status() == WL_CONNECTED) {
    netState = NET_STA;
    strlcpy(ipStr, WiFi.localIP().toString().c_str(), sizeof ipStr);
    configTime(UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    MDNS.begin("seleste");
    return true;
  }
  WiFi.disconnect(true);
  netState = NET_IDLE;
  return false;
}

void sendSetup() {
  String h = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Seleste Setup</title>"
               "<style>body{font-family:sans-serif;background:#0b1220;color:#e5e7eb;max-width:420px;margin:auto;padding:16px}"
               "input,select,button{width:100%;padding:12px;margin:6px 0;border-radius:8px;border:0;font-size:16px;box-sizing:border-box}"
               "button{background:#22c55e;color:#04210f;font-weight:700}</style>"
               "<h2>Seleste Access - WiFi setup</h2><form method=POST action=/save><select name=ssid>");
  for (int i = 0; i < scanCount; i++) { h += "<option>"; h += esc(scanSsid[i]); h += "</option>"; }
  h += F("</select><input name=manual placeholder='or type network name'>"
         "<input name=pass type=password placeholder='WiFi password'><button>Connect</button></form>");
  server.send(200, "text/html", h);
}
void handleRoot() {
  if (portalOn || netState != NET_STA) sendSetup();
  else server.send_P(200, "text/html", DASH);
}
void handleSave() {
  String s = server.arg("manual");
  if (!s.length()) s = server.arg("ssid");
  if (!s.length()) { server.send(400, "text/plain", "Missing network name"); return; }
  LOCK();
  strlcpy(newSsid, s.c_str(), sizeof newSsid);
  strlcpy(newPass, server.arg("pass").c_str(), sizeof newPass);
  UNLOCK();
  reqConnect = true;
  server.send(200, "text/html", "<meta name=viewport content='width=device-width,initial-scale=1'><body style='font-family:sans-serif;padding:20px'>"
                                "<h3>Connecting...</h3><p>Put your phone back on your normal WiFi. The device IP appears on its OLED (Info app) "
                                "or open <b>http://seleste.local</b></p>");
}
void handleStatus() {
  String j; j.reserve(700);
  j += "{\"locked\":"; j += doorLocked ? 1 : 0;
  j += ",\"d1\":"; j += (int)dist1;
  j += ",\"d2\":"; j += (int)dist2;
  j += ",\"ir\":"; j += irBlocked ? 1 : 0;
  j += ",\"link\":"; j += linkUp() ? 1 : 0;
  j += ",\"cards\":"; j += nCards;
  j += ",\"log\":[";
  LOCK();
  for (int i = 0; i < logCount; i++) { if (i) j += ','; j += '"'; j += logBuf[i]; j += '"'; }
  UNLOCK();
  j += "]}";
  server.send(200, "application/json", j);
}
void handleCmd() {
  bool un = server.uri().endsWith("unlock");
  if (millis() < webLock) { server.send(429, "text/plain", "Locked out, wait 30 s"); return; }
  if (server.arg("pin") != String(adminPin)) {
    if (++webFails >= 3) { webLock = millis() + 30000; webFails = 0; }
    logEvent("WEB BAD PIN");
    server.send(403, "text/plain", "Wrong PIN");
    return;
  }
  webFails = 0;
  if (un) { sendLink("G"); logEvent("WEB UNLOCK"); } else { sendLink("L"); logEvent("WEB LOCK"); }
  server.send(200, "text/plain", "OK");
}
void handleNF() {
  if (portalOn) { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); }
  else server.send(404, "text/plain", "Not found");
}

void netTask(void *) {
  server.on("/", handleRoot);
  server.on("/setup", sendSetup);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api/status", handleStatus);
  server.on("/api/unlock", HTTP_POST, handleCmd);
  server.on("/api/lock", HTTP_POST, handleCmd);
  server.onNotFound(handleNF);
  server.begin();

  if (wSsid[0]) connectSta(wSsid, wPass, 15000);
  if (netState != NET_STA) startPortal();

  uint32_t lastTry = 0;
  for (;;) {
    if (portalOn) dns.processNextRequest();
    server.handleClient();
    if (reqScan)   { reqScan = false; doScan(); scanDone = true; }
    if (reqPortal) { reqPortal = false; WiFi.disconnect(); startPortal(); }
    if (reqForget) {
      reqForget = false;
      LOCK(); prefs.remove("ssid"); prefs.remove("pass"); wSsid[0] = 0; wPass[0] = 0; UNLOCK();
      startPortal();
    }
    if (reqConnect) {
      reqConnect = false;
      vTaskDelay(1500 / portTICK_PERIOD_MS);
      char s[33], p[65];
      LOCK(); strlcpy(s, newSsid, sizeof s); strlcpy(p, newPass, sizeof p); UNLOCK();
      if (connectSta(s, p, 20000)) {
        LOCK(); prefs.putString("ssid", s); prefs.putString("pass", p);
        strlcpy(wSsid, s, sizeof wSsid); strlcpy(wPass, p, sizeof wPass); UNLOCK();
        if (portalOn) stopPortal();
        logEvent("WiFi joined");
      } else {
        logEvent("WiFi failed");
        startPortal();
      }
    }
    if (netState == NET_STA && WiFi.status() != WL_CONNECTED && millis() - lastTry > 15000) {
      lastTry = millis(); WiFi.reconnect();
    }
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

// ---------------- keypad ----------------
char scanKeys() {
  static const char kmap[4][4] = {{'1','2','3','A'},{'4','5','6','B'},{'7','8','9','C'},{'*','0','#','D'}};
  for (int r = 0; r < 4; r++) {
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(30);
    for (int c = 0; c < 4; c++) {
      if (!digitalRead(colPins[c])) { digitalWrite(rowPins[r], HIGH); return kmap[r][c]; }
    }
    digitalWrite(rowPins[r], HIGH);
  }
  return 0;
}
char getKey() {
  static char last = 0; static uint32_t t = 0;
  char k = scanKeys();
  if (k != last && millis() - t > 30) { t = millis(); last = k; return k; }
  return 0;
}

// ---------------- UI state ----------------
const char *appNames[8] = {"Door", "Cards", "Radar", "Log", "Wallpaper", "Snake", "Settings", "Info"};
const char *wpNames[6] = {"Stars", "Rain", "Waves", "Ball", "Grid", "Sweep"};
int cur = -1, sel = 0, sub = 0, lsel = 0, ssel = 0, lscroll = 0, pendingApp = 0;
uint8_t oledBView = 0;
bool saver = false;
uint32_t lastAct = 0, pinUntil = 0, toastUntil = 0;
char toastMsg[24];

// text input
bool inActive = false, inMask = true, inNumeric = false, inForce = false, inUpper = false, inShow = false;
uint8_t inPurpose = 0;
char inBuf[65]; int inLen = 0, inMax = 8, inCycle = 0;
char inLastKey = 0; uint32_t inLastTap = 0;
const char *inTitle = "";
const char *MT[10] = {" 0", "1.,@-_!?#$%&", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"};

void toast(const char *m) { strlcpy(toastMsg, m, sizeof toastMsg); toastUntil = millis() + 1500; }

void startInput(uint8_t purpose, const char *title, int maxLen, bool mask, bool numeric) {
  inActive = true; inPurpose = purpose; inTitle = title; inMax = maxLen; inMask = mask;
  inNumeric = numeric; inForce = numeric; inUpper = false; inShow = false;
  inLen = 0; inBuf[0] = 0; inLastKey = 0;
}

// snake
struct Snake { uint8_t x[128], y[128]; int len, dir, fx, fy, score; bool dead; uint32_t last; } sn;
const int8_t SDX[4] = {0, 1, 0, -1}, SDY[4] = {-1, 0, 1, 0};
bool onSnake(int x, int y) { for (int i = 0; i < sn.len; i++) if (sn.x[i] == x && sn.y[i] == y) return true; return false; }
void placeFood() { do { sn.fx = random(16); sn.fy = random(7); } while (onSnake(sn.fx, sn.fy)); }
void snakeReset() {
  sn.len = 3; sn.dir = 1; sn.score = 0; sn.dead = false; sn.last = millis();
  for (int i = 0; i < 3; i++) { sn.x[i] = 8 - i; sn.y[i] = 3; }
  placeFood();
}
void snakeStep() {
  int nx = sn.x[0] + SDX[sn.dir], ny = sn.y[0] + SDY[sn.dir];
  if (nx < 0 || nx > 15 || ny < 0 || ny > 6) { sn.dead = true; sendLink("S2"); return; }
  for (int i = 0; i < sn.len - 1; i++) if (sn.x[i] == nx && sn.y[i] == ny) { sn.dead = true; sendLink("S2"); return; }
  bool ate = (nx == sn.fx && ny == sn.fy);
  if (ate && sn.len < 127) sn.len++;
  for (int i = sn.len - 1; i > 0; i--) { sn.x[i] = sn.x[i - 1]; sn.y[i] = sn.y[i - 1]; }
  sn.x[0] = nx; sn.y[0] = ny;
  if (ate) { sn.score++; placeFood(); sendLink("S0"); }
}

void openApp(int i) {
  if ((i == 1 || i == 6) && millis() > pinUntil) {
    pendingApp = i;
    startInput(IP_GATE, "Enter PIN", 8, true, true);
    return;
  }
  cur = i; sub = 0; lsel = 0; ssel = 0; lscroll = 0;
  if (i == 5) snakeReset();
}

void inputDone(bool ok) {
  if (!ok) return;
  switch (inPurpose) {
    case IP_GATE:
      if (!strcmp(inBuf, adminPin)) { pinUntil = millis() + 60000; openApp(pendingApp); }
      else { toast("Wrong PIN"); sendLink("S2"); }
      break;
    case IP_DOOR:
      if (!strcmp(inBuf, adminPin)) { pinUntil = millis() + 60000; sendLink("G"); toast("Opening"); }
      else { toast("Wrong PIN"); sendLink("S2"); }
      break;
    case IP_NEWPIN:
      if (inLen >= 4) { strlcpy(adminPin, inBuf, sizeof adminPin); saveSettings(); toast("PIN changed"); }
      else toast("Min 4 digits");
      break;
    case IP_WIFIPASS:
      LOCK(); strlcpy(newSsid, selSsid, sizeof newSsid); strlcpy(newPass, inBuf, sizeof newPass); UNLOCK();
      reqConnect = true; sub = 0; toast("Joining WiFi...");
      break;
  }
}

void inputKey(char k) {
  if (k >= '0' && k <= '9') {
    if (inNumeric) {
      if (inLen < inMax) { inBuf[inLen++] = k; inBuf[inLen] = 0; }
      inLastKey = 0;
      return;
    }
    const char *s = MT[k - '0']; int n = strlen(s);
    if (k == inLastKey && millis() - inLastTap < 900 && inLen > 0) inCycle = (inCycle + 1) % n;
    else { if (inLen >= inMax) return; inCycle = 0; inLen++; inLastKey = k; }
    char ch = s[inCycle]; if (inUpper) ch = toupper(ch);
    inBuf[inLen - 1] = ch; inBuf[inLen] = 0; inLastTap = millis();
  }
  else if (k == '*') { if (inLen) inBuf[--inLen] = 0; inLastKey = 0; }
  else if (k == 'B') inUpper = !inUpper;
  else if (k == 'C') { if (!inForce) inNumeric = !inNumeric; }
  else if (k == 'D') inShow = !inShow;
  else if (k == 'A') { inActive = false; inputDone(false); }
  else if (k == '#') { inActive = false; inputDone(true); }
}

void settingText(int i, char *b, size_t n) {
  switch (i) {
    case 0: strlcpy(b, "Join WiFi network", n); break;
    case 1: strlcpy(b, "Open setup portal", n); break;
    case 2: strlcpy(b, "Forget WiFi", n); break;
    case 3: strlcpy(b, "Change PIN", n); break;
    case 4: snprintf(b, n, "Relock time: %us", relockSec); break;
    case 5: snprintf(b, n, "Sound: %s", soundOn ? "on" : "off"); break;
    case 6: snprintf(b, n, "Brightness: %u/3", bright + 1); break;
    case 7: strlcpy(b, "Reboot", n); break;
  }
}

void homeKey(char k) {
  switch (k) {
    case '4': sel = (sel + 7) % 8; break;
    case '6': sel = (sel + 1) % 8; break;
    case '2': case '8': sel = (sel + 4) % 8; break;
    case '5': case '#': openApp(sel); break;
  }
}

void appKey(int a, char k) {
  switch (a) {
    case 0:
      if (k == '#' || k == '5') {
        if (doorLocked) {
          if (millis() < pinUntil) sendLink("G");
          else startInput(IP_DOOR, "PIN to open", 8, true, true);
        } else sendLink("L");
      }
      break;
    case 1:
      if (sub == 0) {
        if (k == '2') lsel = (lsel + nCards) % (nCards + 1);
        if (k == '8') lsel = (lsel + 1) % (nCards + 1);
        if (k == '#') {
          if (lsel == 0) { if (nCards < 16) { enrollResult = 0; enrolling = true; sub = 1; } else toast("Card list full"); }
          else {
            LOCK();
            for (int i = lsel - 1; i < nCards - 1; i++) strcpy(cards[i], cards[i + 1]);
            nCards--; saveCards();
            UNLOCK();
            logEvent("Card removed");
            toast("Card removed");
            if (lsel > nCards) lsel = nCards;
          }
        }
      }
      break;
    case 3:
      if (k == '2' && lscroll > 0) lscroll--;
      if (k == '8' && lscroll < logCount - 5) lscroll++;
      break;
    case 4:
      if (k == '4') wallpaper = (wallpaper + 5) % 6;
      if (k == '6') wallpaper = (wallpaper + 1) % 6;
      if (k == '#') { saveSettings(); toast("Wallpaper saved"); }
      break;
    case 5:
      if (k == '2' && sn.dir != 2) sn.dir = 0;
      if (k == '6' && sn.dir != 3) sn.dir = 1;
      if (k == '8' && sn.dir != 0) sn.dir = 2;
      if (k == '4' && sn.dir != 1) sn.dir = 3;
      if (k == '#' && sn.dead) snakeReset();
      break;
    case 6:
      if (sub == 0) {
        if (k == '2') ssel = (ssel + 7) % 8;
        if (k == '8') ssel = (ssel + 1) % 8;
        if (k == '#') {
          switch (ssel) {
            case 0: sub = 1; scanDone = false; scanCount = 0; lsel = 0; reqScan = true; break;
            case 1: reqPortal = true; toast("Portal: Seleste-Setup"); break;
            case 2: reqForget = true; toast("WiFi forgotten"); break;
            case 3: startInput(IP_NEWPIN, "New PIN", 8, true, true); break;
            case 4: { const uint8_t o[4] = {3, 5, 10, 20}; int i = 0; for (; i < 4; i++) if (o[i] == relockSec) break;
                      relockSec = o[(i + 1) % 4]; saveSettings(); pushConfig(); } break;
            case 5: soundOn = !soundOn; saveSettings(); pushConfig(); break;
            case 6: bright = (bright + 1) % 3; saveSettings(); applyBright(); break;
            case 7: ESP.restart(); break;
          }
        }
      } else if (sub == 1 && scanDone && scanCount > 0) {
        if (k == '2') lsel = (lsel + scanCount - 1) % scanCount;
        if (k == '8') lsel = (lsel + 1) % scanCount;
        if (k == '#') { strlcpy(selSsid, scanSsid[lsel], sizeof selSsid); startInput(IP_WIFIPASS, "WiFi password", 64, true, false); }
      }
      break;
  }
}

void handleKey(char k) {
  if (inActive) { inputKey(k); return; }
  if (k == 'A') { cur = -1; enrolling = false; return; }
  if (k == 'B') { oledBView = (oledBView + 1) % 4; return; }
  if (k == 'C') { sendLink("L"); toast("Locking door"); return; }
  if (k == 'D') { soundOn = !soundOn; saveSettings(); pushConfig(); toast(soundOn ? "Sound ON" : "Sound OFF"); return; }
  if (cur < 0) { homeKey(k); return; }
  if (k == '*') { enrolling = false; if (sub > 0) sub = 0; else cur = -1; return; }
  appKey(cur, k);
}

void appTick() {
  if (cur == 5 && !sn.dead && millis() - sn.last > (uint32_t)(190 - min(sn.score, 12) * 8)) { sn.last = millis(); snakeStep(); }
  if (cur == 1 && sub == 1 && enrollResult) {
    int r = enrollResult; enrollResult = 0; sub = 0;
    toast(r == 1 ? "Card added" : r == 2 ? "Already added" : "Card list full");
  }
}

void visitorCheck() {
  static uint32_t last = 0;
  if (dist1 < 35 && millis() - last > 15000) {
    last = millis();
    logEvent("Visitor %dcm", (int)dist1);
    lastAct = millis(); saver = false;
  }
}

// ---------------- wallpapers (virtual 256x64 canvas across BOTH screens) ----------------
// xo = 0 for the left screen (OLED A), 128 for the right screen (OLED B)
void wpDraw(U8G2 &d, int xo, uint32_t f, int idx) {
  switch (idx) {
    case 0:
      for (int i = 0; i < 48; i++) {
        int sp = 1 + (i % 3);
        int gx = 255 - ((i * 97 + f * sp) % 256);
        int y = (i * 37) % 64, sx = gx - xo;
        if (sx >= 0 && sx < 128) { d.drawPixel(sx, y); if (sp == 3) d.drawPixel(sx + 1, y); }
      }
      break;
    case 1:
      for (int c = 0; c < 32; c++) {
        int sx = c * 8 + 3 - xo;
        if (sx < 0 || sx >= 128) continue;
        int head = ((f * (1 + c % 3)) / 2 + c * 13) % 90;
        for (int k = 0; k < 6; k++) { int y = head - k * 5; if (y >= 0 && y < 64) d.drawBox(sx, y, 2, k == 0 ? 4 : 3); }
      }
      break;
    case 2:
      for (int x = 0; x < 128; x++) {
        float g = x + xo;
        d.drawPixel(x, 32 + (int)(14 * sinf((g + f * 2) * 0.06f)));
        d.drawPixel(x, 32 + (int)(8 * sinf((g - f * 3) * 0.11f + 1.0f)));
      }
      break;
    case 3: {
      int t = (f * 3) % 496, bx = t < 248 ? t + 4 : 499 - t;
      int u = (f * 2) % 112, by = u < 56 ? u + 4 : 115 - u;
      d.drawCircle(bx - xo, by, 6);
      d.drawDisc(bx - xo, by, 3);
    } break;
    case 4:
      for (int v = 0; v <= 16; v++) { int sx = v * 16 - (int)(f % 16) - xo; if (sx >= 0 && sx < 128) d.drawVLine(sx, 0, 64); }
      for (int y = 8; y < 64; y += 16) d.drawHLine(0, y, 128);
      d.drawBox(((f * 4) % 256) - xo, 30, 6, 4);
      break;
    case 5: {
      float a = (f % 90) * 0.0698f;
      int cx = 128 - xo;
      d.drawCircle(cx, 32, 15); d.drawCircle(cx, 32, 30);
      d.drawLine(cx, 32, cx + (int)(120 * cosf(a)), 32 + (int)(120 * sinf(a)));
    } break;
  }
}

// ---------------- drawing: OLED A ----------------
void ctext(U8G2 &d, int y, const char *s) { d.drawStr((128 - d.getStrWidth(s)) / 2, y, s); }

void hdr(const char *t) {
  oledA.setFont(u8g2_font_6x10_tr); oledA.drawStr(0, 9, t);
  char tm[8]; getTimeStr(tm, sizeof tm);
  oledA.setFont(u8g2_font_5x7_tr); oledA.drawStr(128 - oledA.getStrWidth(tm), 8, tm);
  oledA.drawHLine(0, 11, 128);
}

void drawIcon(int i, int x, int y) {
  U8G2 &d = oledA;
  switch (i) {
    case 0: d.drawRFrame(x + 4, y, 8, 10, 3); d.drawRBox(x + 2, y + 7, 12, 9, 2); d.setDrawColor(0); d.drawDisc(x + 8, y + 11, 1); d.setDrawColor(1); break;
    case 1: d.drawRFrame(x, y + 2, 16, 12, 2); d.drawBox(x + 2, y + 5, 5, 4); d.drawHLine(x + 9, y + 6, 5); d.drawHLine(x + 9, y + 9, 5); break;
    case 2: d.drawCircle(x + 8, y + 8, 7); d.drawCircle(x + 8, y + 8, 3); d.drawLine(x + 8, y + 8, x + 14, y + 3); break;
    case 3: d.drawFrame(x + 2, y, 12, 16); d.drawHLine(x + 4, y + 4, 8); d.drawHLine(x + 4, y + 8, 8); d.drawHLine(x + 4, y + 12, 5); break;
    case 4: d.drawFrame(x, y + 1, 16, 14); d.drawTriangle(x + 2, y + 13, x + 7, y + 6, x + 12, y + 13); d.drawDisc(x + 11, y + 5, 2); break;
    case 5: d.drawBox(x, y + 11, 10, 4); d.drawBox(x + 6, y + 5, 4, 10); d.drawBox(x + 6, y + 5, 9, 4); d.drawFrame(x + 1, y + 2, 3, 3); break;
    case 6: d.drawCircle(x + 8, y + 8, 5); d.drawDisc(x + 8, y + 8, 2); d.drawBox(x + 7, y, 2, 3); d.drawBox(x + 7, y + 13, 2, 3); d.drawBox(x, y + 7, 3, 2); d.drawBox(x + 13, y + 7, 3, 2); break;
    case 7: d.drawCircle(x + 8, y + 8, 7); d.drawBox(x + 7, y + 7, 2, 5); d.drawBox(x + 7, y + 4, 2, 2); break;
  }
}

void drawHome() {
  oledA.setFont(u8g2_font_5x7_tr);
  const char *w = netState == NET_STA ? "WiFi" : netState == NET_AP ? "Setup AP" : netState == NET_CONN ? "Join.." : "No net";
  oledA.drawStr(0, 7, w);
  char tm[8]; getTimeStr(tm, sizeof tm); ctext(oledA, 7, tm);
  const char *l = doorLocked ? "LOCK" : "OPEN"; int lw = oledA.getStrWidth(l);
  oledA.drawStr(128 - lw, 7, l);
  if (linkUp()) oledA.drawDisc(128 - lw - 6, 4, 2); else oledA.drawCircle(128 - lw - 6, 4, 2);
  oledA.drawHLine(0, 9, 128);
  for (int i = 0; i < 8; i++) {
    int cx = (i % 4) * 32 + 16, cy = 22 + (i / 4) * 22;
    drawIcon(i, cx - 8, cy - 8);
    if (i == sel) oledA.drawRFrame(cx - 14, cy - 10, 28, 21, 3);
  }
  oledA.setFont(u8g2_font_6x10_tr);
  ctext(oledA, 63, appNames[sel]);
}

void drawInput() {
  oledA.setFont(u8g2_font_6x10_tr);
  oledA.drawStr(0, 10, inTitle);
  oledA.drawFrame(2, 16, 124, 16);
  char tmp[24]; int st = inLen > 18 ? inLen - 18 : 0, n = 0;
  for (int i = st; i < inLen; i++) {
    bool fresh = (i == inLen - 1 && !inNumeric && millis() - inLastTap < 900);
    tmp[n++] = (inMask && !inShow && !fresh) ? '*' : inBuf[i];
  }
  tmp[n] = 0;
  oledA.drawStr(5, 28, tmp);
  if ((millis() / 400) % 2) oledA.drawVLine(6 + n * 6, 18, 12);
  oledA.setFont(u8g2_font_5x7_tr);
  oledA.drawStr(2, 42, inNumeric ? "mode: 123" : (inUpper ? "mode: ABC" : "mode: abc"));
  oledA.drawStr(2, 53, "#=OK *=del A=cancel");
  oledA.drawStr(2, 62, "B=case C=123 D=show");
}

void drawToast() {
  if (millis() > toastUntil) return;
  oledA.setFont(u8g2_font_6x10_tr);
  int w = oledA.getStrWidth(toastMsg) + 10, x = (128 - w) / 2;
  oledA.setDrawColor(0); oledA.drawRBox(x - 2, 24, w + 4, 18, 4);
  oledA.setDrawColor(1); oledA.drawRFrame(x, 26, w, 14, 3);
  oledA.drawStr(x + 5, 37, toastMsg);
}

void drawApp(int a) {
  char b[40];
  switch (a) {
    case 0: {
      hdr("Door");
      oledA.setFont(u8g2_font_ncenB14_tr);
      ctext(oledA, 35, doorLocked ? "LOCKED" : "UNLOCKED");
      oledA.setFont(u8g2_font_5x7_tr);
      snprintf(b, sizeof b, "Out %dcm   In %dcm", (int)dist1, (int)dist2);
      ctext(oledA, 47, b);
      oledA.drawStr(0, 62, "#=open/close   C=lock");
    } break;
    case 1: {
      snprintf(b, sizeof b, "Cards (%d/16)", nCards);
      hdr(b);
      oledA.setFont(u8g2_font_6x10_tr);
      if (sub == 1) {
        ctext(oledA, 34, "Tap a card on reader");
        const char *dots[4] = {"", ".", "..", "..."};
        ctext(oledA, 48, dots[(frame / 5) % 4]);
        oledA.setFont(u8g2_font_5x7_tr); ctext(oledA, 62, "* to cancel");
      } else {
        int top = lsel < 4 ? 0 : lsel - 3;
        LOCK();
        for (int row = 0; row < 4; row++) {
          int idx = top + row; if (idx > nCards) break;
          int y = 24 + row * 12; bool s = (idx == lsel);
          if (s) { oledA.drawBox(0, y - 9, 128, 11); oledA.setDrawColor(0); }
          if (idx == 0) oledA.drawStr(4, y, "[+] Add new card");
          else { snprintf(b, sizeof b, "%d: %s", idx, cards[idx - 1]); oledA.drawStr(4, y, b); }
          oledA.setDrawColor(1);
        }
        UNLOCK();
      }
    } break;
    case 2: {
      hdr("Radar");
      oledA.setFont(u8g2_font_5x7_tr);
      for (int s = 0; s < 2; s++) {
        int cm = s ? dist2 : dist1, y = 16 + s * 14;
        oledA.drawStr(0, y + 7, s ? "IN" : "OUT");
        oledA.drawFrame(20, y, 80, 9);
        oledA.drawBox(21, y + 1, 78 - 78 * min(cm, 200) / 200, 7);
        snprintf(b, sizeof b, "%d", cm); oledA.drawStr(104, y + 7, b);
      }
      oledA.drawStr(0, 54, irBlocked ? "IR: BLOCKED" : "IR: clear");
      if (irBlocked) oledA.drawDisc(70, 51, 3); else oledA.drawCircle(70, 51, 3);
      oledA.drawStr(80, 62, linkUp() ? "Uno OK" : "Uno ?");
    } break;
    case 3: {
      hdr("Door log");
      oledA.setFont(u8g2_font_5x7_tr);
      LOCK();
      if (!logCount) oledA.drawStr(2, 24, "No events yet");
      for (int i = 0; i < 5 && lscroll + i < logCount; i++) oledA.drawStr(0, 20 + i * 9, logBuf[lscroll + i]);
      UNLOCK();
    } break;
    case 4: {
      wpDraw(oledA, 0, frame, wallpaper);
      oledA.setDrawColor(0); oledA.drawBox(0, 52, 128, 12); oledA.setDrawColor(1);
      oledA.setFont(u8g2_font_6x10_tr);
      snprintf(b, sizeof b, "< %s >  #=save", wpNames[wallpaper]);
      ctext(oledA, 62, b);
    } break;
    case 5: {
      oledA.drawFrame(0, 0, 128, 56);
      for (int i = 0; i < sn.len; i++) oledA.drawBox(sn.x[i] * 8 + 1, sn.y[i] * 8 + 1, 6, 6);
      oledA.drawFrame(sn.fx * 8 + 2, sn.fy * 8 + 2, 4, 4);
      oledA.setFont(u8g2_font_5x7_tr);
      snprintf(b, sizeof b, "Score %d", sn.score); oledA.drawStr(0, 63, b);
      if (sn.dead) {
        oledA.setDrawColor(0); oledA.drawBox(14, 20, 100, 18); oledA.setDrawColor(1);
        oledA.drawFrame(14, 20, 100, 18);
        oledA.setFont(u8g2_font_6x10_tr); ctext(oledA, 33, "GAME OVER  #=retry");
      }
    } break;
    case 6: {
      hdr("Settings");
      oledA.setFont(u8g2_font_6x10_tr);
      if (sub == 0) {
        int top = ssel < 4 ? 0 : ssel - 3;
        for (int row = 0; row < 4; row++) {
          int idx = top + row, y = 24 + row * 12;
          bool s = (idx == ssel);
          if (s) { oledA.drawBox(0, y - 9, 128, 11); oledA.setDrawColor(0); }
          settingText(idx, b, sizeof b); oledA.drawStr(4, y, b);
          oledA.setDrawColor(1);
        }
      } else {
        if (!scanDone) ctext(oledA, 38, "Scanning...");
        else if (!scanCount) ctext(oledA, 38, "No networks found");
        else {
          int top = lsel < 4 ? 0 : lsel - 3;
          for (int row = 0; row < 4; row++) {
            int idx = top + row; if (idx >= scanCount) break;
            int y = 24 + row * 12; bool s = (idx == lsel);
            if (s) { oledA.drawBox(0, y - 9, 128, 11); oledA.setDrawColor(0); }
            oledA.drawStr(4, y, scanSsid[idx]);
            oledA.setDrawColor(1);
          }
        }
      }
    } break;
    case 7: {
      hdr("Info");
      oledA.setFont(u8g2_font_5x7_tr);
      snprintf(b, sizeof b, "IP: %s", ipStr); oledA.drawStr(0, 21, b);
      snprintf(b, sizeof b, "WiFi: %.20s", wSsid[0] ? wSsid : "(none)"); oledA.drawStr(0, 30, b);
      snprintf(b, sizeof b, "Uno link: %s", linkUp() ? "OK" : "DOWN"); oledA.drawStr(0, 39, b);
      snprintf(b, sizeof b, "Heap: %u KB  Cards: %d", (unsigned)(ESP.getFreeHeap() / 1024), nCards); oledA.drawStr(0, 48, b);
      snprintf(b, sizeof b, "Uptime: %lu min", millis() / 60000UL); oledA.drawStr(0, 57, b);
    } break;
  }
}

void drawA() {
  I2C_LOCK();
  oledA.clearBuffer();
  if (saver) wpDraw(oledA, 0, frame, wallpaper);
  else if (inActive) drawInput();
  else if (cur < 0) drawHome();
  else drawApp(cur);
  if (!saver) drawToast();
  oledA.sendBuffer();
  I2C_UNLOCK();
}

// ---------------- drawing: OLED B (runs in its own task) ----------------
void drawBar(int y, const char *lab, int cm) {
  char b[8];
  oledB.setFont(u8g2_font_5x7_tr); oledB.drawStr(0, y + 7, lab);
  oledB.drawFrame(26, y, 74, 9);
  oledB.drawBox(27, y + 1, 72 - 72 * min(cm, 200) / 200, 7);
  snprintf(b, sizeof b, "%d", cm); oledB.drawStr(104, y + 7, b);
}

void drawB() {
  oledB.clearBuffer();
  char b[40], tm[8];
  if (saver || cur == 4 || oledBView == 3) {
    wpDraw(oledB, 128, frame, wallpaper);
  } else if (cur == 5) {
    oledB.setFont(u8g2_font_6x10_tr); ctext(oledB, 12, "SNAKE");
    oledB.setFont(u8g2_font_logisoso24_tr);
    snprintf(b, sizeof b, "%d", sn.score); ctext(oledB, 50, b);
  } else {
    switch (oledBView) {
      case 0:
        getTimeStr(tm, sizeof tm);
        oledB.setFont(u8g2_font_logisoso24_tr); ctext(oledB, 28, tm);
        oledB.setFont(u8g2_font_5x7_tr);
        snprintf(b, sizeof b, "DOOR: %s", doorLocked ? "LOCKED" : "UNLOCKED"); ctext(oledB, 42, b);
        snprintf(b, sizeof b, "%s %s", netState == NET_STA ? "WiFi" : netState == NET_AP ? "AP" : "--", ipStr); ctext(oledB, 52, b);
        ctext(oledB, 62, lastEvent);
        break;
      case 1:
        oledB.setFont(u8g2_font_6x10_tr); oledB.drawStr(0, 9, "SENSORS"); oledB.drawHLine(0, 11, 128);
        drawBar(16, "OUT", dist1); drawBar(30, "IN", dist2);
        oledB.setFont(u8g2_font_5x7_tr);
        oledB.drawStr(0, 52, irBlocked ? "IR beam: BLOCKED" : "IR beam: clear");
        oledB.drawStr(0, 62, linkUp() ? "Uno link: OK" : "Uno link: DOWN");
        break;
      case 2:
        oledB.setFont(u8g2_font_6x10_tr); oledB.drawStr(0, 9, "EVENTS"); oledB.drawHLine(0, 11, 128);
        oledB.setFont(u8g2_font_5x7_tr);
        LOCK();
        for (int i = 0; i < 6 && i < logCount; i++) oledB.drawStr(0, 21 + i * 8, logBuf[i]);
        UNLOCK();
        break;
    }
  }
  oledB.sendBuffer();
}

void oledBTask(void *) {
  for (;;) {
    I2C_LOCK(); drawB(); I2C_UNLOCK();
    vTaskDelay(((saver || cur == 4 || oledBView == 3) ? 40 : 250) / portTICK_PERIOD_MS);
  }
}

// ---------------- boot ----------------
void bootSplash() {
  for (int p = 0; p <= 100; p += 20) {
    I2C_LOCK();
    oledA.clearBuffer();
    oledA.setFont(u8g2_font_ncenB14_tr); ctext(oledA, 28, "SELESTE");
    oledA.setFont(u8g2_font_6x10_tr); ctext(oledA, 44, "OS v1.0 - ESP32-C6");
    oledA.drawFrame(14, 52, 100, 8); oledA.drawBox(14, 52, p, 8);
    oledA.sendBuffer();
    oledB.clearBuffer();
    oledB.setFont(u8g2_font_6x10_tr); ctext(oledB, 26, "Seleste Technologies");
    oledB.setFont(u8g2_font_5x7_tr); ctext(oledB, 40, "booting...");
    oledB.sendBuffer();
    I2C_UNLOCK();
    delay(80);
  }
}

void setup() {
  Serial.begin(115200);
  mtx = xSemaphoreCreateRecursiveMutex();
#if OLED2_SHARED_BUS
  i2cMtx = xSemaphoreCreateRecursiveMutex();
#endif
  prefs.begin("sel", false);
  loadAll();
  for (int i = 0; i < 4; i++) { pinMode(rowPins[i], OUTPUT); digitalWrite(rowPins[i], HIGH); pinMode(colPins[i], INPUT_PULLUP); }
  Serial1.begin(9600, SERIAL_8N1, LINK_RX, LINK_TX);

  Wire.setPins(OLED_A_SDA, OLED_A_SCL);
#if OLED2_SHARED_BUS
  oledB.setI2CAddress(0x3D * 2);
#endif
  oledA.begin(); oledA.setBusClock(400000);
  oledB.begin();
  applyBright();
  bootSplash();
  snakeReset();
  lastAct = millis();
  sendLink("S4");

  xTaskCreate(linkTask, "link", 3072, NULL, 2, NULL);
  xTaskCreate(netTask, "net", 10240, NULL, 1, NULL);
  xTaskCreate(oledBTask, "oledB", 4096, NULL, 1, NULL);
}

void loop() {
  char k = getKey();
  if (k) {
    lastAct = millis();
    if (saver) saver = false;                 // first key only wakes the screens
    else { sendLink("S0"); handleKey(k); }
  }
  if (!saver && millis() - lastAct > 40000 && !(cur == 5 && !sn.dead)) saver = true;
  visitorCheck();
  appTick();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50) { lastDraw = millis(); frame++; drawA(); }
  delay(2);
}
