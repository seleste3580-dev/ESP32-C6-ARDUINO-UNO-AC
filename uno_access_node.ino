/*
  SELESTE ACCESS NODE  -  Arduino UNO  (the "muscle")
  ---------------------------------------------------
  Owns: RC522 RFID, SG90 servo (door latch), passive buzzer, 3 LEDs,
        2x HC-SR04, IR obstacle sensor, exit push button.
  Talks to the ESP32-C6 ("brain") over SoftwareSerial, 9600 baud, text lines.

  Uno -> ESP32                       ESP32 -> Uno
    H:1            hello/boot          G   grant: unlock + relock timer + happy beep
    C:<UIDHEX>     card scanned        X   deny: sad beep + red flash
    D:<out>,<in>   distances (cm)      L   lock now
    I:0|1          IR blocked?         S<n> play sound n (0 click,1 ok,2 bad,3 alarm,4 boot)
    B:1            exit button         R<sec> relock time in seconds
    S:0|1          1 = locked          M0|M1 unmute / mute

  Libraries: MFRC522 (Miguel Balboa), Servo, SoftwareSerial, SPI
*/
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <SoftwareSerial.h>

// ---------------- PINS ----------------
#define PIN_LINK_RX 2    // <- ESP32-C6 GPIO18 (TX)
#define PIN_LINK_TX 3    // -> ESP32-C6 GPIO21 (RX)  THROUGH 1k/2k DIVIDER
#define PIN_BUZZ    4
#define PIN_SERVO   5
#define PIN_LED_G   6
#define PIN_LED_R   7
#define PIN_LED_Y   8
#define PIN_RST     9    // RC522 RST
#define PIN_SS      10   // RC522 SDA/SS  (MOSI 11, MISO 12, SCK 13 are hardware SPI)
#define PIN_TRIG1   A0   // outside sensor
#define PIN_ECHO1   A1
#define PIN_TRIG2   A2   // inside sensor
#define PIN_ECHO2   A3
#define PIN_IR      A4   // IR obstacle OUT (LOW = obstacle)
#define PIN_BTN     A5   // exit button to GND

const uint8_t ANG_LOCKED = 10;
const uint8_t ANG_OPEN   = 100;

SoftwareSerial link(PIN_LINK_RX, PIN_LINK_TX);
MFRC522 rfid(PIN_SS, PIN_RST);
Servo door;

bool locked = true, muted = false, passed = false, irBlocked = false, servoOn = false;
bool irCand = false;
unsigned long relockMs = 5000;
unsigned long unlockedAt = 0, irClearAt = 0, irCandSince = 0, lastDist = 0;
unsigned long lastCard = 0, servoDetachAt = 0, lastBtn = 0, lastStatus = 0, yellowUntil = 0, redUntil = 0;
char line[16];
uint8_t ll = 0;

// ---------------- sound ----------------
void snd(uint8_t id) {
  if (muted) return;
  switch (id) {
    case 0: tone(PIN_BUZZ, 2000, 15); break;
    case 1: tone(PIN_BUZZ, 1500, 90); delay(100); tone(PIN_BUZZ, 2200, 140); delay(150); break;
    case 2: tone(PIN_BUZZ, 300, 200); delay(230); tone(PIN_BUZZ, 220, 250); delay(280); break;
    case 3: for (uint8_t i = 0; i < 6; i++) { tone(PIN_BUZZ, 1800, 80); delay(100); tone(PIN_BUZZ, 1200, 80); delay(100); } break;
    case 4: tone(PIN_BUZZ, 1000, 100); delay(120); tone(PIN_BUZZ, 1500, 100); delay(120); tone(PIN_BUZZ, 2000, 140); delay(160); break;
  }
}

// ---------------- door ----------------
void moveServo(uint8_t a) {
  if (!servoOn) { door.attach(PIN_SERVO); servoOn = true; }
  door.write(a);
  servoDetachAt = millis() + 700;
}

void sendStatus() {
  link.print(F("S:"));
  link.println(locked ? 1 : 0);
  lastStatus = millis();
}

void setLock(bool l) {
  bool changed = (l != locked);
  locked = l;
  if (changed) moveServo(l ? ANG_LOCKED : ANG_OPEN);
  digitalWrite(PIN_LED_G, l ? LOW : HIGH);
  digitalWrite(PIN_LED_R, l ? HIGH : LOW);
  if (!l) { unlockedAt = millis(); passed = false; }
  if (changed) sendStatus();
}

// ---------------- sensors ----------------
int readCm(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW); delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  unsigned long d = pulseIn(echo, HIGH, 25000UL);
  if (d == 0) return 400;
  int cm = d / 58;
  return cm > 400 ? 400 : cm;
}

void pollSensors() {
  // IR with simple 40 ms debounce
  bool now = (digitalRead(PIN_IR) == LOW);
  if (now != irCand) { irCand = now; irCandSince = millis(); }
  if (irCand != irBlocked && millis() - irCandSince > 40) {
    irBlocked = irCand;
    if (!irBlocked) irClearAt = millis();
    link.print(F("I:")); link.println(irBlocked ? 1 : 0);
  }
  // exit button
  if (digitalRead(PIN_BTN) == LOW && millis() - lastBtn > 500) {
    lastBtn = millis();
    link.println(F("B:1"));
  }
  // distances every 300 ms
  if (millis() - lastDist > 300) {
    lastDist = millis();
    int d1 = readCm(PIN_TRIG1, PIN_ECHO1);
    delay(30);                                   // avoid cross-talk between the two sensors
    int d2 = readCm(PIN_TRIG2, PIN_ECHO2);
    link.print(F("D:")); link.print(d1); link.print(','); link.println(d2);
    if (d1 < 40) yellowUntil = millis() + 400;
  }
}

void handleRelock() {
  if (locked) return;
  if (irBlocked) { passed = true; unlockedAt = millis(); }   // someone in the doorway: hold open
  if (millis() - unlockedAt > relockMs) setLock(true);
  else if (passed && !irBlocked && millis() - irClearAt > 1500) setLock(true);  // walked through -> relock early
}

// ---------------- RFID ----------------
void scanRfid() {
  if (millis() - lastCard < 1500) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  char uid[21];
  uint8_t n = rfid.uid.size > 10 ? 10 : rfid.uid.size;
  for (uint8_t i = 0; i < n; i++) sprintf(uid + i * 2, "%02X", rfid.uid.uidByte[i]);
  link.print(F("C:")); link.println(uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  lastCard = millis();
  yellowUntil = millis() + 300;
  snd(0);
}

// ---------------- commands from ESP32 ----------------
void handleCmd(char *l) {
  switch (l[0]) {
    case 'G': setLock(false); unlockedAt = millis(); snd(1); break;
    case 'X': redUntil = millis() + 600; snd(2); break;
    case 'L': setLock(true); break;
    case 'S': snd(atoi(l + 1)); break;
    case 'R': { long s = atol(l + 1); if (s >= 1 && s <= 120) relockMs = s * 1000UL; } break;
    case 'M': muted = (l[1] == '1'); break;
  }
}

void readLink() {
  while (link.available()) {
    char c = link.read();
    if (c == '\n') { line[ll] = 0; if (ll) handleCmd(line); ll = 0; }
    else if (c != '\r' && ll < sizeof(line) - 1) line[ll++] = c;
  }
}

void updateLeds() {
  digitalWrite(PIN_LED_Y, millis() < yellowUntil ? HIGH : LOW);
  if (millis() < redUntil) digitalWrite(PIN_LED_R, (millis() / 80) % 2);
  else digitalWrite(PIN_LED_R, locked ? HIGH : LOW);
}

void setup() {
  pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_Y, OUTPUT);
  pinMode(PIN_TRIG1, OUTPUT); pinMode(PIN_ECHO1, INPUT);
  pinMode(PIN_TRIG2, OUTPUT); pinMode(PIN_ECHO2, INPUT);
  pinMode(PIN_IR, INPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_BUZZ, OUTPUT);
  Serial.begin(115200);          // USB debug only
  link.begin(9600);
  SPI.begin();
  rfid.PCD_Init();
  setLock(true);
  moveServo(ANG_LOCKED);
  snd(4);
  link.println(F("H:1"));
  sendStatus();
}

void loop() {
  readLink();
  scanRfid();
  pollSensors();
  handleRelock();
  updateLeds();
  if (servoOn && millis() > servoDetachAt) { door.detach(); servoOn = false; }
  if (millis() - lastStatus > 5000) sendStatus();   // heartbeat/sync
}
