#include <Servo.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>

/*
 * ===== Pin assignments (D1 Mini) =====
 *
 * SERVO_PIN   -> D5 (GPIO14)  - Servo signal
 * LED_PIN     -> D4 (GPIO2)   - External LED
 * MODE_A_PIN  -> D6 (GPIO12)  - 3-pos switch pole A (common)
 * MODE_B_PIN  -> D7 (GPIO13)  - 3-pos switch pole B (common)
 *
 * Switch logic:
 *   LOW + LOW   = Lazy
 *   HIGH + HIGH = Playful
 *   mixed       = Zoomies
 */

// ===== Wi-Fi access point =====
// Password must be at least 8 characters for WPA2 SoftAP.
const char* AP_SSID = "CatToy-Setup";
const char* AP_PASS = "cattoy123";

ESP8266WebServer server(80);

const int SERVO_PIN  = D5;
const int LED_PIN    = D4;
const int MODE_A_PIN = D6;
const int MODE_B_PIN = D7;

const char* CONFIG_PATH = "/config.json";

// ===== Servo safeguards =====
int SERVO_MIN_DEG  = 25;
int SERVO_MAX_DEG  = 155;
int SERVO_REST_DEG = 90;

const bool AUTO_CENTER_REST = true;
const int LAZY_STEP_DEG    = 2;
const int PLAYFUL_STEP_DEG = 3;
const int ZOOMIES_STEP_DEG = 4;
const int DART_EXTRA_STEP_DEG   = 2;

// Smaller delay = faster motion
int LAZY_STEP_DELAY_MS    = 10;
int PLAYFUL_STEP_DELAY_MS = 6;
int ZOOMIES_STEP_DELAY_MS = 4;
int AUTO_REST_MIN_MINUTES = 2;
int AUTO_REST_MAX_MINUTES = 5;

const unsigned long STARTUP_WARMUP_MS = 5000UL;
const unsigned long MODE_SWITCH_RESTART_DELAY_MS = 1500UL;

Servo wandServo;

enum PlayMode {
  MODE_LAZY,
  MODE_PLAYFUL,
  MODE_ZOOMIES
};

enum ControllerState {
  STATE_STARTUP,
  STATE_RESTING,
  STATE_SESSION
};

ControllerState controllerState = STATE_STARTUP;
int currentServoDeg = 90;

// ===== LED blink state =====
bool ledState = false;
unsigned long lastLedToggleMs = 0;

// ===== Runtime flags =====
bool settingsChanged = false;
bool littleFsReady = false;
bool forceSessionRequested = false;
bool parkRequested = false;
bool modeSwitchRestartPending = false;

// ===== Runtime timing =====
unsigned long startupEndMs = 0;
unsigned long nextAutoSessionMs = 0;
unsigned long lastSessionEndMs = 0;
unsigned long sessionCounter = 0;

// ===== Helpers =====
int clampAngle(int angle) {
  if (angle < SERVO_MIN_DEG) return SERVO_MIN_DEG;
  if (angle > SERVO_MAX_DEG) return SERVO_MAX_DEG;
  return angle;
}

int min2(int a, int b) { return (a < b) ? a : b; }
int max2(int a, int b) { return (a > b) ? a : b; }

int pctOf(int base, int pct) {
  return (base * pct) / 100;
}

int randRange(int minVal, int maxVal) {
  return minVal + random(maxVal - minVal + 1);
}

unsigned long randRangeUL(unsigned long minVal, unsigned long maxVal) {
  if (maxVal <= minVal) return minVal;
  unsigned long span = maxVal - minVal;
  return minVal + (unsigned long)random((long)span + 1L);
}

bool validateServoWindow(int newMin, int newMax) {
  if (newMin < 0 || newMin > 170) return false;
  if (newMax < 10 || newMax > 180) return false;
  if (newMax <= newMin) return false;
  if ((newMax - newMin) < 20) return false;
  return true;
}

bool validateStepDelayMs(int value) {
  return value >= 2 && value <= 25;
}

bool validateRestWindowMinutes(int minMinutes, int maxMinutes) {
  if (minMinutes < 1 || minMinutes > 240) return false;
  if (maxMinutes < 1 || maxMinutes > 240) return false;
  if (maxMinutes < minMinutes) return false;
  return true;
}

bool extractJsonInt(const String& json, const char* key, int& valueOut) {
  String quotedKey = String("\"") + key + "\"";
  int keyPos = json.indexOf(quotedKey);
  if (keyPos < 0) return false;

  int colonPos = json.indexOf(':', keyPos + quotedKey.length());
  if (colonPos < 0) return false;

  int valueStart = colonPos + 1;
  while (valueStart < json.length() && isspace(json[valueStart])) {
    valueStart++;
  }

  int valueEnd = valueStart;
  if (valueEnd < json.length() && json[valueEnd] == '-') {
    valueEnd++;
  }

  while (valueEnd < json.length() && isdigit(json[valueEnd])) {
    valueEnd++;
  }

  if (valueEnd == valueStart) return false;

  valueOut = json.substring(valueStart, valueEnd).toInt();
  return true;
}

int maxSafeAmplitude() {
  int leftRoom  = SERVO_REST_DEG - SERVO_MIN_DEG;
  int rightRoom = SERVO_MAX_DEG - SERVO_REST_DEG;
  int safeRoom = min2(leftRoom, rightRoom);
  return max2(0, safeRoom);
}

unsigned long minutesToMs(int minutes) {
  return (unsigned long)minutes * 60UL * 1000UL;
}

PlayMode readModeSwitch() {
  int a = digitalRead(MODE_A_PIN);
  int b = digitalRead(MODE_B_PIN);

  if (a == LOW && b == LOW)   return MODE_LAZY;
  if (a == HIGH && b == HIGH) return MODE_PLAYFUL;
  return MODE_ZOOMIES;
}

const char* modeName(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY:     return "Lazy";
    case MODE_PLAYFUL:  return "Playful";
    case MODE_ZOOMIES:  return "Zoomies";
  }
  return "Unknown";
}

const char* controllerStateName(ControllerState state) {
  switch (state) {
    case STATE_STARTUP: return "Startup warmup";
    case STATE_RESTING: return "Resting";
    case STATE_SESSION: return "In session";
  }
  return "Unknown";
}

int ledToggleIntervalMs(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY:     return 420;
    case MODE_PLAYFUL:  return 240;
    case MODE_ZOOMIES:  return 140;
  }
  return 240;
}

int stepDelayMsForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY:     return LAZY_STEP_DELAY_MS;
    case MODE_PLAYFUL:  return PLAYFUL_STEP_DELAY_MS;
    case MODE_ZOOMIES:  return ZOOMIES_STEP_DELAY_MS;
  }
  return PLAYFUL_STEP_DELAY_MS;
}

int stepDegForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY:     return LAZY_STEP_DEG;
    case MODE_PLAYFUL:  return PLAYFUL_STEP_DEG;
    case MODE_ZOOMIES:  return ZOOMIES_STEP_DEG;
  }
  return PLAYFUL_STEP_DEG;
}

unsigned long sessionDurationMsForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY:     return randRangeUL(20000UL, 35000UL);
    case MODE_PLAYFUL:  return randRangeUL(30000UL, 50000UL);
    case MODE_ZOOMIES:  return randRangeUL(40000UL, 65000UL);
  }
  return 30000UL;
}

unsigned long restDurationMs() {
  return randRangeUL(minutesToMs(AUTO_REST_MIN_MINUTES), minutesToMs(AUTO_REST_MAX_MINUTES));
}

int teaseSpeedForMode(PlayMode mode) {
  return stepDelayMsForMode(mode);
}

int dartSpeedForMode(PlayMode mode) {
  int faster = stepDelayMsForMode(mode) - 1;
  return max2(2, faster);
}

int teaseAmpForMode(PlayMode mode) {
  int maxAmp = maxSafeAmplitude();
  int loPct = 0;
  int hiPct = 0;

  switch (mode) {
    case MODE_LAZY:     loPct = 5;  hiPct = 16; break;
    case MODE_PLAYFUL:  loPct = 10; hiPct = 28; break;
    case MODE_ZOOMIES:  loPct = 15; hiPct = 34; break;
  }

  int lo = max2(4, pctOf(maxAmp, loPct));
  int hi = max2(lo + 2, pctOf(maxAmp, hiPct));
  return randRange(lo, hi);
}

int dartAmpForMode(PlayMode mode) {
  int maxAmp = maxSafeAmplitude();
  int loPct = 0;
  int hiPct = 0;

  switch (mode) {
    case MODE_LAZY:     loPct = 18; hiPct = 42; break;
    case MODE_PLAYFUL:  loPct = 35; hiPct = 70; break;
    case MODE_ZOOMIES:  loPct = 55; hiPct = 92; break;
  }

  int lo = max2(10, pctOf(maxAmp, loPct));
  int hi = max2(lo + 2, pctOf(maxAmp, hiPct));
  return randRange(lo, hi);
}

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void updateLed(PlayMode mode, unsigned long now) {
  int interval = ledToggleIntervalMs(mode);

  if (now - lastLedToggleMs >= (unsigned long)interval) {
    ledState = !ledState;
    setLed(ledState);
    lastLedToggleMs = now;
  }
}

void serviceNetwork() {
  server.handleClient();
  yield();
}

void writeServoAngle(int angle) {
  currentServoDeg = clampAngle(angle);
  wandServo.write(currentServoDeg);
}

bool shouldInterruptSession(PlayMode modeAtStart) {
  if (parkRequested) return true;

  PlayMode currentMode = readModeSwitch();
  if (currentMode != modeAtStart) {
    modeSwitchRestartPending = true;
    return true;
  }

  if (settingsChanged) return true;

  return false;
}

bool delayResponsive(unsigned long ms, PlayMode modeAtStart) {
  unsigned long start = millis();

  while (millis() - start < ms) {
    PlayMode currentMode = readModeSwitch();
    updateLed(currentMode, millis());
    serviceNetwork();

    if (shouldInterruptSession(modeAtStart)) return true;

    delay(10);
  }

  return false;
}

bool moveServoSmooth(int fromDeg, int toDeg, int stepDelayMs, int stepDeg, PlayMode modeAtStart) {
  fromDeg = clampAngle(fromDeg);
  toDeg   = clampAngle(toDeg);

  if (fromDeg == toDeg) {
    writeServoAngle(toDeg);
    return false;
  }

  stepDeg = max2(1, stepDeg);
  int step = (toDeg > fromDeg) ? stepDeg : -stepDeg;

  if (step > 0) {
    for (int a = fromDeg; a < toDeg; a += step) {
      writeServoAngle(a);
      updateLed(modeAtStart, millis());
      serviceNetwork();
      if (shouldInterruptSession(modeAtStart)) return true;
      delay(stepDelayMs);
    }
  } else {
    for (int a = fromDeg; a > toDeg; a += step) {
      writeServoAngle(a);
      updateLed(modeAtStart, millis());
      serviceNetwork();
      if (shouldInterruptSession(modeAtStart)) return true;
      delay(stepDelayMs);
    }
  }

  writeServoAngle(toDeg);
  return false;
}

void moveServoSmoothPark(int fromDeg, int toDeg, int stepDelayMs, int stepDeg, PlayMode ledMode) {
  fromDeg = clampAngle(fromDeg);
  toDeg   = clampAngle(toDeg);

  if (fromDeg == toDeg) {
    writeServoAngle(toDeg);
    return;
  }

  stepDeg = max2(1, stepDeg);
  int step = (toDeg > fromDeg) ? stepDeg : -stepDeg;

  if (step > 0) {
    for (int a = fromDeg; a < toDeg; a += step) {
      writeServoAngle(a);
      updateLed(ledMode, millis());
      serviceNetwork();
      delay(stepDelayMs);
    }
  } else {
    for (int a = fromDeg; a > toDeg; a += step) {
      writeServoAngle(a);
      updateLed(ledMode, millis());
      serviceNetwork();
      delay(stepDelayMs);
    }
  }

  writeServoAngle(toDeg);
}

bool moveToAngle(int targetDeg, int stepDelayMs, int stepDeg, PlayMode modeAtStart) {
  targetDeg = clampAngle(targetDeg);
  return moveServoSmooth(currentServoDeg, targetDeg, stepDelayMs, stepDeg, modeAtStart);
}

bool moveToRest(PlayMode modeAtStart, int stepDelayMs) {
  return moveToAngle(SERVO_REST_DEG, stepDelayMs, LAZY_STEP_DEG, modeAtStart);
}

void applyServoWindow(int newMin, int newMax) {
  SERVO_MIN_DEG = newMin;
  SERVO_MAX_DEG = newMax;

  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  writeServoAngle(currentServoDeg);
  settingsChanged = true;
}

void applyStepDelays(int lazyMs, int playfulMs, int zoomiesMs) {
  LAZY_STEP_DELAY_MS = lazyMs;
  PLAYFUL_STEP_DELAY_MS = playfulMs;
  ZOOMIES_STEP_DELAY_MS = zoomiesMs;
  settingsChanged = true;
}

void applyRestWindowMinutes(int minMinutes, int maxMinutes) {
  AUTO_REST_MIN_MINUTES = minMinutes;
  AUTO_REST_MAX_MINUTES = maxMinutes;
  settingsChanged = true;
}

bool saveConfig() {
  if (!littleFsReady) return false;

  File file = LittleFS.open(CONFIG_PATH, "w");
  if (!file) {
    Serial.println("Failed to open config file for write.");
    return false;
  }

  String json = "{\"servoMinDeg\":";
  json += String(SERVO_MIN_DEG);
  json += ",\"servoMaxDeg\":";
  json += String(SERVO_MAX_DEG);
  json += ",\"lazyStepDelayMs\":";
  json += String(LAZY_STEP_DELAY_MS);
  json += ",\"playfulStepDelayMs\":";
  json += String(PLAYFUL_STEP_DELAY_MS);
  json += ",\"zoomiesStepDelayMs\":";
  json += String(ZOOMIES_STEP_DELAY_MS);
  json += ",\"autoRestMinMinutes\":";
  json += String(AUTO_REST_MIN_MINUTES);
  json += ",\"autoRestMaxMinutes\":";
  json += String(AUTO_REST_MAX_MINUTES);
  json += "}\n";

  size_t written = file.print(json);
  file.close();

  if (written != json.length()) {
    Serial.println("Failed to write full config file.");
    return false;
  }

  return true;
}

void loadConfig() {
  if (!littleFsReady) return;

  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("No saved config found. Using defaults.");
    return;
  }

  File file = LittleFS.open(CONFIG_PATH, "r");
  if (!file) {
    Serial.println("Failed to open config file for read. Using defaults.");
    return;
  }

  String json = file.readString();
  file.close();

  int savedMin = 0;
  int savedMax = 0;
  int savedLazyDelay = LAZY_STEP_DELAY_MS;
  int savedPlayfulDelay = PLAYFUL_STEP_DELAY_MS;
  int savedZoomiesDelay = ZOOMIES_STEP_DELAY_MS;
  int savedRestMinMinutes = AUTO_REST_MIN_MINUTES;
  int savedRestMaxMinutes = AUTO_REST_MAX_MINUTES;

  if (!extractJsonInt(json, "servoMinDeg", savedMin) ||
      !extractJsonInt(json, "servoMaxDeg", savedMax)) {
    Serial.println("Config file missing required fields. Using defaults.");
    return;
  }

  if (!validateServoWindow(savedMin, savedMax)) {
    Serial.println("Saved servo window is invalid. Using defaults.");
    return;
  }

  applyServoWindow(savedMin, savedMax);

  bool hasLazyDelay = extractJsonInt(json, "lazyStepDelayMs", savedLazyDelay) ||
                      extractJsonInt(json, "beginnerStepDelayMs", savedLazyDelay);
  bool hasPlayfulDelay = extractJsonInt(json, "playfulStepDelayMs", savedPlayfulDelay) ||
                         extractJsonInt(json, "intermediateStepDelayMs", savedPlayfulDelay);
  bool hasZoomiesDelay = extractJsonInt(json, "zoomiesStepDelayMs", savedZoomiesDelay) ||
                         extractJsonInt(json, "advancedStepDelayMs", savedZoomiesDelay);

  if (hasLazyDelay && hasPlayfulDelay && hasZoomiesDelay &&
      validateStepDelayMs(savedLazyDelay) &&
      validateStepDelayMs(savedPlayfulDelay) &&
      validateStepDelayMs(savedZoomiesDelay)) {
    applyStepDelays(savedLazyDelay, savedPlayfulDelay, savedZoomiesDelay);
  } else if (hasLazyDelay || hasPlayfulDelay || hasZoomiesDelay) {
    Serial.println("Saved speed settings invalid. Using default speed settings.");
  }

  bool hasRestMinMinutes = extractJsonInt(json, "autoRestMinMinutes", savedRestMinMinutes);
  bool hasRestMaxMinutes = extractJsonInt(json, "autoRestMaxMinutes", savedRestMaxMinutes);

  if (hasRestMinMinutes && hasRestMaxMinutes &&
      validateRestWindowMinutes(savedRestMinMinutes, savedRestMaxMinutes)) {
    applyRestWindowMinutes(savedRestMinMinutes, savedRestMaxMinutes);
  } else if (hasRestMinMinutes || hasRestMaxMinutes) {
    Serial.println("Saved rest window is invalid. Using default rest window.");
  }

  Serial.print("Loaded servo window from LittleFS: ");
  Serial.print(SERVO_MIN_DEG);
  Serial.print(" to ");
  Serial.println(SERVO_MAX_DEG);
  Serial.print("Loaded step delays (ms): ");
  Serial.print(LAZY_STEP_DELAY_MS);
  Serial.print(", ");
  Serial.print(PLAYFUL_STEP_DELAY_MS);
  Serial.print(", ");
  Serial.println(ZOOMIES_STEP_DELAY_MS);
  Serial.print("Loaded rest window (minutes): ");
  Serial.print(AUTO_REST_MIN_MINUTES);
  Serial.print(" to ");
  Serial.println(AUTO_REST_MAX_MINUTES);
}

bool canAutoStartSessions() {
  // Future scheduling should gate autonomous starts here without changing
  // session behavior or low-level motion code.
  return true;
}

bool doTease(PlayMode modeAtStart) {
  int amp = teaseAmpForMode(modeAtStart);
  int left  = clampAngle(SERVO_REST_DEG - amp);
  int right = clampAngle(SERVO_REST_DEG + amp);
  int speed = teaseSpeedForMode(modeAtStart);

  int base = (random(2) == 0) ? left : right;
  int hops = randRange(1, 3);

  for (int i = 0; i < hops; i++) {
    int jitter = randRange(-8, 8);
    if (moveToAngle(clampAngle(base + jitter), speed, stepDegForMode(modeAtStart), modeAtStart)) {
      return true;
    }

    if (delayResponsive((unsigned long)randRange(50, 160), modeAtStart)) {
      return true;
    }
  }

  return delayResponsive((unsigned long)randRange(60, 220), modeAtStart);
}

bool doBigDart(PlayMode modeAtStart) {
  int amp = dartAmpForMode(modeAtStart);
  int left  = clampAngle(SERVO_REST_DEG - amp);
  int right = clampAngle(SERVO_REST_DEG + amp);
  int speed = dartSpeedForMode(modeAtStart);
  int dartStepDeg = stepDegForMode(modeAtStart) + DART_EXTRA_STEP_DEG;

  bool startLeft = (random(2) == 0);
  int start = startLeft ? left : right;
  int end   = startLeft ? right : left;

  if (moveToAngle(start, speed, dartStepDeg, modeAtStart)) return true;
  if (delayResponsive((unsigned long)randRange(20, 75), modeAtStart)) return true;
  if (moveToAngle(end, speed, dartStepDeg, modeAtStart)) return true;

  if (random(3) == 0) {
    if (moveToAngle(clampAngle(end + randRange(-10, 10)), speed, dartStepDeg, modeAtStart)) {
      return true;
    }
  }

  return delayResponsive((unsigned long)randRange(60, 220), modeAtStart);
}

bool hidePause(PlayMode modeAtStart) {
  switch (modeAtStart) {
    case MODE_LAZY:     return delayResponsive((unsigned long)randRange(2500, 4500), modeAtStart);
    case MODE_PLAYFUL:  return delayResponsive((unsigned long)randRange(2200, 4000), modeAtStart);
    case MODE_ZOOMIES:  return delayResponsive((unsigned long)randRange(1800, 3200), modeAtStart);
  }
  return false;
}

bool shortPause(PlayMode modeAtStart) {
  switch (modeAtStart) {
    case MODE_LAZY:     return delayResponsive((unsigned long)randRange(900, 2200), modeAtStart);
    case MODE_PLAYFUL:  return delayResponsive((unsigned long)randRange(600, 1800), modeAtStart);
    case MODE_ZOOMIES:  return delayResponsive((unsigned long)randRange(350, 1200), modeAtStart);
  }
  return false;
}

bool runChaosBurst(PlayMode modeAtStart, unsigned long chaosMs) {
  unsigned long endMs = millis() + chaosMs;

  while (millis() < endMs) {
    if (doBigDart(modeAtStart)) return true;
    if (doTease(modeAtStart)) return true;
  }

  return false;
}

bool runTimedSession(PlayMode modeAtStart, unsigned long sessionMs, bool allowMidChaos) {
  unsigned long startMs = millis();
  unsigned long endMs   = startMs + sessionMs;

  unsigned long warmupMs = min((unsigned long)15000, sessionMs / 4);
  unsigned long finaleMs = min((unsigned long)12000, sessionMs / 4);

  if (moveToRest(modeAtStart, 10)) return true;
  if (delayResponsive(250, modeAtStart)) return true;

  unsigned long warmupEnd = startMs + warmupMs;
  while (millis() < warmupEnd) {
    if (doTease(modeAtStart)) return true;
    if (random(5) == 0 && doBigDart(modeAtStart)) return true;
    if (shortPause(modeAtStart)) return true;
  }

  unsigned long huntEnd = (endMs > finaleMs) ? (endMs - finaleMs) : endMs;
  unsigned long lastHideAt = millis();
  unsigned long nextHideAfter = randRangeUL(12000UL, 18000UL);
  bool chaosInserted = false;
  unsigned long chaosAt = startMs + (sessionMs / 2);

  while (millis() < huntEnd) {
    if (allowMidChaos && !chaosInserted && millis() >= chaosAt) {
      if (runChaosBurst(modeAtStart, 6000UL)) return true;
      chaosInserted = true;
      if (shortPause(modeAtStart)) return true;
    }

    unsigned long burstLen = randRangeUL(2500UL, 7000UL);
    unsigned long burstEnd = millis() + burstLen;
    if (burstEnd > huntEnd) burstEnd = huntEnd;

    int teasesUntilDart = randRange(2, (modeAtStart == MODE_ZOOMIES) ? 4 : 5);

    while (millis() < burstEnd) {
      if (teasesUntilDart > 0) {
        if (doTease(modeAtStart)) return true;
        teasesUntilDart--;
      } else {
        if (doBigDart(modeAtStart)) return true;
        teasesUntilDart = randRange(2, (modeAtStart == MODE_ZOOMIES) ? 4 : 5);
      }
    }

    unsigned long now = millis();
    if (now - lastHideAt >= nextHideAfter && random(3) == 0) {
      if (moveToRest(modeAtStart, 10)) return true;
      if (hidePause(modeAtStart)) return true;
      lastHideAt = millis();
      nextHideAfter = randRangeUL(12000UL, 18000UL);
    } else if (shortPause(modeAtStart)) {
      return true;
    }
  }

  while (millis() < endMs) {
    if (doTease(modeAtStart)) return true;
    if (doTease(modeAtStart)) return true;
    if (doBigDart((modeAtStart == MODE_LAZY) ? MODE_PLAYFUL : modeAtStart)) return true;
    if (moveToRest(modeAtStart, 9)) return true;
    if (delayResponsive((unsigned long)randRange(200, 450), modeAtStart)) return true;
  }

  if (moveToRest(modeAtStart, 10)) return true;
  return delayResponsive((unsigned long)randRange(1200, 2500), modeAtStart);
}

void finishSession(PlayMode modeAtEnd) {
  parkRequested = false;

  if (settingsChanged) {
    settingsChanged = false;
  }

  moveServoSmoothPark(currentServoDeg, SERVO_REST_DEG, 12, LAZY_STEP_DEG, modeAtEnd);

  controllerState = STATE_RESTING;
  lastSessionEndMs = millis();
  sessionCounter++;

  if (modeSwitchRestartPending) {
    nextAutoSessionMs = lastSessionEndMs + MODE_SWITCH_RESTART_DELAY_MS;
    modeSwitchRestartPending = false;

    Serial.print("Mode switch detected. Restarting in ");
    Serial.print((nextAutoSessionMs - lastSessionEndMs) / 1000.0);
    Serial.println(" s");
  } else {
    nextAutoSessionMs = lastSessionEndMs + restDurationMs();

    Serial.print("Session complete. Next auto session in ");
    Serial.print((nextAutoSessionMs - lastSessionEndMs) / 1000UL);
    Serial.println(" s");
  }
}

void runSessionForMode(PlayMode modeAtStart) {
  controllerState = STATE_SESSION;
  forceSessionRequested = false;

  unsigned long sessionMs = sessionDurationMsForMode(modeAtStart);
  bool allowMidChaos = (((sessionCounter + 1UL) % 3UL) == 0UL);

  Serial.print("Starting ");
  Serial.print(modeName(modeAtStart));
  Serial.print(" session for ");
  Serial.print(sessionMs / 1000UL);
  Serial.println(" s");

  runTimedSession(modeAtStart, sessionMs, allowMidChaos);
  finishSession(modeAtStart);
}

String htmlPage() {
  PlayMode mode = readModeSwitch();
  unsigned long now = millis();

  String page;
  page.reserve(4200);

  page += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cat Toy Control</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;line-height:1.45;}"
    ".card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-bottom:16px;}"
    ".hint{background:#f7f7f7;border-left:4px solid #4f6d4a;}"
    ".meta{color:#444;font-size:14px;}"
    ".actions form{display:inline-block;margin:8px 8px 0 0;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
    "input[type=number]{width:100%;padding:10px;font-size:16px;box-sizing:border-box;}"
    "button{margin-top:16px;padding:12px 16px;font-size:16px;border-radius:10px;border:0;cursor:pointer;}"
    ".secondary{background:#ececec;color:#111;}"
    "</style></head><body>"
    "<h1>Cat Toy Control</h1>"
  );

  page += F("<div class='card'>");
  page += F("<div><strong>Current mode:</strong> ");
  page += modeName(mode);
  page += F("</div><div><strong>Runtime state:</strong> ");
  page += controllerStateName(controllerState);
  page += F("</div><div><strong>Current servo window:</strong> ");
  page += String(SERVO_MIN_DEG);
  page += F("&deg; to ");
  page += String(SERVO_MAX_DEG);
  page += F("&deg;</div><div><strong>Current rest:</strong> ");
  page += String(SERVO_REST_DEG);
  page += F("&deg;</div><div><strong>Current swing from rest:</strong> up to ");
  page += String(maxSafeAmplitude());
  page += F("&deg; each side</div><div><strong>Auto rest window:</strong> ");
  page += String(AUTO_REST_MIN_MINUTES);
  page += F(" to ");
  page += String(AUTO_REST_MAX_MINUTES);
  page += F(" min</div>");

  if (controllerState == STATE_STARTUP) {
    unsigned long warmupRemainingMs = (startupEndMs > now) ? (startupEndMs - now) : 0;
    page += F("<div><strong>Warmup remaining:</strong> ");
    page += String(warmupRemainingMs / 1000UL);
    page += F(" s</div>");
  } else {
    unsigned long nextInMs = (nextAutoSessionMs > now) ? (nextAutoSessionMs - now) : 0;
    page += F("<div><strong>Next auto session:</strong> ");
    page += String(nextInMs / 1000UL);
    page += F(" s</div>");
  }

  page += F("</div>");

  page += F("<div class='card hint'><strong>Runtime model</strong>"
            "<p class='meta'>This sketch now runs autonomous play sessions with rest periods between them. The local web UI changes settings and can trigger or park sessions, but basic play still works without visiting this page.</p>"
            "<p class='meta'>That session boundary is the future hook for planned or scheduled play windows.</p></div>");

  page += F("<div class='card actions'><strong>Manual control</strong>"
            "<form action='/start' method='post'><button type='submit'>Start Session Now</button></form>"
            "<form action='/park' method='post'><button class='secondary' type='submit'>Park At Rest</button></form>"
            "<p class='meta'>Start now forces the next session immediately. Park ends the current session as soon as the sketch reaches an interrupt check and returns the servo to rest.</p></div>");

  page += F("<div class='card hint'><strong>How to widen swings</strong>"
            "<p class='meta'>The toy swings inside the servo window set below. Lowering the minimum angle and/or raising the maximum angle gives the servo more room to travel. A wider gap usually means stronger-looking swings, as long as your hardware can move safely in that range.</p>"
            "<p class='meta'>Lazy stays gentler. Playful and Zoomies use more of the safe window and shorter rests.</p></div>");

  page += F("<div class='card hint'><strong>How to change swing speed</strong>"
            "<p class='meta'>These speed settings control how many milliseconds the sketch waits between small servo steps. Lower numbers move faster. Higher numbers move slower and softer.</p>"
            "<p class='meta'>Change these gradually. If you set them too low, motion can get jerky or mechanically harsh.</p></div>");

  page += F("<div class='card hint'><strong>How to slow down automatic play</strong>"
            "<p class='meta'>The rest window below controls how long the toy waits between sessions when left running on its own. The sketch picks a random rest time between the minimum and maximum you set.</p>"
            "<p class='meta'>Use a longer window if you want the toy available all day without constant stimulation.</p></div>");

  page += F("<div class='card'><form action='/set' method='get'>");
  page += F("<label for='min'>Left limit / minimum angle (SERVO_MIN_DEG)</label>");
  page += F("<input id='min' name='min' type='number' min='0' max='170' value='");
  page += String(SERVO_MIN_DEG);
  page += F("'>");

  page += F("<label for='max'>Right limit / maximum angle (SERVO_MAX_DEG)</label>");
  page += F("<input id='max' name='max' type='number' min='10' max='180' value='");
  page += String(SERVO_MAX_DEG);
  page += F("'>");

  page += F("<label for='lazyDelay'>Lazy speed delay in ms</label>");
  page += F("<input id='lazyDelay' name='lazyDelay' type='number' min='2' max='25' value='");
  page += String(LAZY_STEP_DELAY_MS);
  page += F("'>");

  page += F("<label for='playfulDelay'>Playful speed delay in ms</label>");
  page += F("<input id='playfulDelay' name='playfulDelay' type='number' min='2' max='25' value='");
  page += String(PLAYFUL_STEP_DELAY_MS);
  page += F("'>");

  page += F("<label for='zoomiesDelay'>Zoomies speed delay in ms</label>");
  page += F("<input id='zoomiesDelay' name='zoomiesDelay' type='number' min='2' max='25' value='");
  page += String(ZOOMIES_STEP_DELAY_MS);
  page += F("'>");

  page += F("<label for='restMinMinutes'>Minimum rest between sessions in minutes</label>");
  page += F("<input id='restMinMinutes' name='restMinMinutes' type='number' min='1' max='240' value='");
  page += String(AUTO_REST_MIN_MINUTES);
  page += F("'>");

  page += F("<label for='restMaxMinutes'>Maximum rest between sessions in minutes</label>");
  page += F("<input id='restMaxMinutes' name='restMaxMinutes' type='number' min='1' max='240' value='");
  page += String(AUTO_REST_MAX_MINUTES);
  page += F("'>");

  page += F("<button type='submit'>Apply</button>");
  page += F("</form>");
  page += F("<p class='meta'>For safety this sketch requires max &gt; min and at least 20 degrees of spread. "
            "When you apply a new window, the rest position is re-centered automatically and the setting is saved to LittleFS for the next boot.</p>");
  page += F("<p class='meta'>Speed delay range is 2-25 ms. Lower is faster.</p>");
  page += F("<p class='meta'>Rest window range is 1-240 minutes. Normal automatic sessions restart after a random pause inside that window. Mode-switch restarts stay fast.</p>");
  page += F("</div>");

  page += F("<div class='card meta'>Connect to Wi-Fi <strong>");
  page += AP_SSID;
  page += F("</strong> and browse to <strong>http://192.168.4.1</strong>.</div>");

  page += F("</body></html>");
  return page;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleSet() {
  bool hasLazyArg = server.hasArg("lazyDelay") || server.hasArg("beginnerDelay");
  bool hasPlayfulArg = server.hasArg("playfulDelay") || server.hasArg("intermediateDelay");
  bool hasZoomiesArg = server.hasArg("zoomiesDelay") || server.hasArg("advancedDelay");
  bool hasRestMinArg = server.hasArg("restMinMinutes");
  bool hasRestMaxArg = server.hasArg("restMaxMinutes");

  if (!server.hasArg("min") || !server.hasArg("max") ||
      !hasLazyArg || !hasPlayfulArg || !hasZoomiesArg ||
      !hasRestMinArg || !hasRestMaxArg) {
    server.send(400, "text/plain", "Missing one or more required parameters.");
    return;
  }

  int newMin = server.arg("min").toInt();
  int newMax = server.arg("max").toInt();
  int newLazyDelay = server.hasArg("lazyDelay") ? server.arg("lazyDelay").toInt()
                                                : server.arg("beginnerDelay").toInt();
  int newPlayfulDelay = server.hasArg("playfulDelay") ? server.arg("playfulDelay").toInt()
                                                      : server.arg("intermediateDelay").toInt();
  int newZoomiesDelay = server.hasArg("zoomiesDelay") ? server.arg("zoomiesDelay").toInt()
                                                      : server.arg("advancedDelay").toInt();
  int newRestMinMinutes = server.arg("restMinMinutes").toInt();
  int newRestMaxMinutes = server.arg("restMaxMinutes").toInt();

  if (!validateServoWindow(newMin, newMax)) {
    server.send(400, "text/plain",
                "Servo limits must stay in range, keep max > min, and keep at least 20 degrees between them.");
    return;
  }

  if (!validateStepDelayMs(newLazyDelay) ||
      !validateStepDelayMs(newPlayfulDelay) ||
      !validateStepDelayMs(newZoomiesDelay)) {
    server.send(400, "text/plain", "Each speed delay must be between 2 and 25 ms.");
    return;
  }

  if (!validateRestWindowMinutes(newRestMinMinutes, newRestMaxMinutes)) {
    server.send(400, "text/plain",
                "Rest window must stay between 1 and 240 minutes, with max greater than or equal to min.");
    return;
  }

  applyServoWindow(newMin, newMax);
  applyStepDelays(newLazyDelay, newPlayfulDelay, newZoomiesDelay);
  applyRestWindowMinutes(newRestMinMinutes, newRestMaxMinutes);

  if (!saveConfig()) {
    server.send(500, "text/plain",
                "Updated settings for this boot, but failed to save to LittleFS.");
    return;
  }

  Serial.print("Updated servo window: ");
  Serial.print(SERVO_MIN_DEG);
  Serial.print(" to ");
  Serial.println(SERVO_MAX_DEG);
  Serial.print("Updated step delays (ms): ");
  Serial.print(LAZY_STEP_DELAY_MS);
  Serial.print(", ");
  Serial.print(PLAYFUL_STEP_DELAY_MS);
  Serial.print(", ");
  Serial.println(ZOOMIES_STEP_DELAY_MS);
  Serial.print("Updated rest window (minutes): ");
  Serial.print(AUTO_REST_MIN_MINUTES);
  Serial.print(" to ");
  Serial.println(AUTO_REST_MAX_MINUTES);

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleStart() {
  forceSessionRequested = true;
  parkRequested = false;
  nextAutoSessionMs = millis();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handlePark() {
  parkRequested = true;
  forceSessionRequested = false;
  nextAutoSessionMs = millis() + restDurationMs();
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A0));

  pinMode(MODE_A_PIN, INPUT_PULLUP);
  pinMode(MODE_B_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  littleFsReady = LittleFS.begin();
  if (!littleFsReady) {
    Serial.println("LittleFS mount failed. Continuing with defaults only.");
  } else {
    loadConfig();
  }

  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  currentServoDeg = clampAngle(SERVO_REST_DEG);

  wandServo.attach(SERVO_PIN);
  writeServoAngle(currentServoDeg);

  ledState = false;
  setLed(false);
  lastLedToggleMs = millis();

  startupEndMs = millis() + STARTUP_WARMUP_MS;
  controllerState = STATE_STARTUP;
  nextAutoSessionMs = startupEndMs;
  settingsChanged = false;

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);

  Serial.println();
  Serial.println("Cat toy started.");
  Serial.print("SoftAP status: ");
  Serial.println(ok ? "READY" : "FAILED");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/start", HTTP_POST, handleStart);
  server.on("/park", HTTP_POST, handlePark);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  PlayMode mode = readModeSwitch();
  unsigned long now = millis();

  updateLed(mode, now);
  serviceNetwork();

  if (controllerState == STATE_STARTUP) {
    if (currentServoDeg != SERVO_REST_DEG) {
      writeServoAngle(SERVO_REST_DEG);
    }

    if (now >= startupEndMs) {
      controllerState = STATE_RESTING;
      nextAutoSessionMs = now;
    }

    delay(10);
    return;
  }

  if (controllerState != STATE_SESSION && settingsChanged) {
    settingsChanged = false;
  }

  if (parkRequested && controllerState != STATE_SESSION) {
    moveServoSmoothPark(currentServoDeg, SERVO_REST_DEG, 12, LAZY_STEP_DEG, mode);
    parkRequested = false;
  }

  if (controllerState != STATE_SESSION && currentServoDeg != SERVO_REST_DEG) {
    moveServoSmoothPark(currentServoDeg, SERVO_REST_DEG, 12, LAZY_STEP_DEG, mode);
  }

  bool shouldStart = false;

  if (forceSessionRequested) {
    shouldStart = true;
  } else if (controllerState == STATE_RESTING && now >= nextAutoSessionMs && canAutoStartSessions()) {
    shouldStart = true;
  }

  if (shouldStart) {
    runSessionForMode(mode);
  }

  delay(20);
}
