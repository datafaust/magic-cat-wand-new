#include <Servo.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <time.h>

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

const char* LEGACY_CONFIG_PATH = "/config.json";
const char* MOTION_CONFIG_PATH = "/motion.json";
const char* WIFI_CONFIG_PATH = "/wifi.json";
const char* SCHEDULE_CONFIG_PATH = "/schedule.json";

const int SERVO_PIN  = D5;
const int LED_PIN    = D4;
const int MODE_A_PIN = D6;
const int MODE_B_PIN = D7;
const bool LED_FLASH = false;

const size_t WIFI_SSID_MAX_LEN = 32;
const size_t WIFI_PASS_MAX_LEN = 63;
const size_t TIMEZONE_MAX_LEN = 63;

const unsigned long STARTUP_WARMUP_MS = 5000UL;
const unsigned long BOOT_GESTURE_WINDOW_MS = 4000UL;
const unsigned long MODE_SWITCH_RESTART_DELAY_MS = 1500UL;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000UL;
const unsigned long NTP_SYNC_TIMEOUT_MS = 20000UL;
const unsigned long TIME_VALID_MIN_EPOCH = 1704067200UL;  // 2024-01-01 UTC
const unsigned long BOOT_GESTURE_STABLE_MS = 250UL;

const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";

const int DEFAULT_SERVO_MIN_DEG = 25;
const int DEFAULT_SERVO_MAX_DEG = 155;
const int DEFAULT_LAZY_STEP_DELAY_MS = 10;
const int DEFAULT_PLAYFUL_STEP_DELAY_MS = 6;
const int DEFAULT_ZOOMIES_STEP_DELAY_MS = 4;
const int DEFAULT_AUTO_REST_MIN_MINUTES = 2;
const int DEFAULT_AUTO_REST_MAX_MINUTES = 5;
const bool DEFAULT_SCHEDULE_ENABLED = false;
const int DEFAULT_SCHEDULE_START_MINUTE = 9 * 60;
const int DEFAULT_SCHEDULE_END_MINUTE = 17 * 60;
const bool DEFAULT_SCHEDULE_SECOND_ENABLED = false;
const int DEFAULT_SCHEDULE_SECOND_START_MINUTE = 18 * 60;
const int DEFAULT_SCHEDULE_SECOND_END_MINUTE = 19 * 60;
const char* DEFAULT_TIMEZONE_TZ = "EST5EDT,M3.2.0/2,M11.1.0/2";

ESP8266WebServer server(80);
Servo wandServo;

// ===== Servo safeguards =====
int SERVO_MIN_DEG  = DEFAULT_SERVO_MIN_DEG;
int SERVO_MAX_DEG  = DEFAULT_SERVO_MAX_DEG;
int SERVO_REST_DEG = 90;

const bool AUTO_CENTER_REST = true;
const int LAZY_STEP_DEG    = 2;
const int PLAYFUL_STEP_DEG = 3;
const int ZOOMIES_STEP_DEG = 4;
const int DART_EXTRA_STEP_DEG = 2;

// Smaller delay = faster motion
int LAZY_STEP_DELAY_MS    = DEFAULT_LAZY_STEP_DELAY_MS;
int PLAYFUL_STEP_DELAY_MS = DEFAULT_PLAYFUL_STEP_DELAY_MS;
int ZOOMIES_STEP_DELAY_MS = DEFAULT_ZOOMIES_STEP_DELAY_MS;
int AUTO_REST_MIN_MINUTES = DEFAULT_AUTO_REST_MIN_MINUTES;
int AUTO_REST_MAX_MINUTES = DEFAULT_AUTO_REST_MAX_MINUTES;

char WIFI_SSID[WIFI_SSID_MAX_LEN + 1] = "";
char WIFI_PASS[WIFI_PASS_MAX_LEN + 1] = "";
char TIMEZONE_TZ[TIMEZONE_MAX_LEN + 1] = "EST5EDT,M3.2.0/2,M11.1.0/2";
bool SCHEDULE_ENABLED = DEFAULT_SCHEDULE_ENABLED;
int SCHEDULE_START_MINUTE = DEFAULT_SCHEDULE_START_MINUTE;
int SCHEDULE_END_MINUTE = DEFAULT_SCHEDULE_END_MINUTE;
bool SCHEDULE_SECOND_ENABLED = DEFAULT_SCHEDULE_SECOND_ENABLED;
int SCHEDULE_SECOND_START_MINUTE = DEFAULT_SCHEDULE_SECOND_START_MINUTE;
int SCHEDULE_SECOND_END_MINUTE = DEFAULT_SCHEDULE_SECOND_END_MINUTE;

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

enum ScheduleDecisionReason {
  SCHEDULE_DECISION_DISABLED,
  SCHEDULE_DECISION_TIME_INVALID,
  SCHEDULE_DECISION_IN_WINDOW,
  SCHEDULE_DECISION_OUTSIDE_WINDOW
};

struct TimezoneOption {
  const char* label;
  const char* tz;
};

const TimezoneOption TIMEZONE_OPTIONS[] = {
  {"Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
  {"Central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
  {"Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
  {"Arizona", "MST7"},
  {"Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
  {"Alaska", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
  {"Hawaii", "HST10"}
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

bool hasSavedWifiCredentials = false;
bool wifiSetupModeForced = false;
bool bootGestureWindowClosed = false;
bool networkBootStarted = false;
bool stationConnectAttemptActive = false;
bool stationWasConnected = false;
bool timeSyncRequested = false;
bool timezoneApplied = false;
bool timeValid = false;
bool scheduleWindowWasOpen = false;
bool scheduleStartPending = false;
wl_status_t lastLoggedStationStatus = WL_IDLE_STATUS;
unsigned long lastStationStatusChangeMs = 0;

// ===== Runtime timing =====
unsigned long startupEndMs = 0;
unsigned long bootGestureWindowEndMs = 0;
unsigned long nextAutoSessionMs = 0;
unsigned long lastSessionEndMs = 0;
unsigned long sessionCounter = 0;
unsigned long wifiConnectAttemptStartMs = 0;
unsigned long lastWifiConnectAttemptMs = 0;
unsigned long ntpSyncStartMs = 0;

PlayMode bootGestureStableMode = MODE_ZOOMIES;
PlayMode lastBootGestureRawMode = MODE_ZOOMIES;
unsigned long lastBootGestureRawChangeMs = 0;
int bootGestureStage = 0;

ScheduleDecisionReason lastScheduleDecisionReason = SCHEDULE_DECISION_DISABLED;

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

bool validateScheduleMinute(int minuteValue) {
  return minuteValue >= 0 && minuteValue <= 1439;
}

bool validateTimezoneTz(const String& tz) {
  if (tz.length() == 0 || tz.length() > TIMEZONE_MAX_LEN) return false;

  for (size_t i = 0; i < sizeof(TIMEZONE_OPTIONS) / sizeof(TIMEZONE_OPTIONS[0]); i++) {
    if (tz.equals(TIMEZONE_OPTIONS[i].tz)) return true;
  }

  return false;
}

void copyBoundedString(char* dest, size_t destSize, const String& value) {
  size_t copyLen = value.length();
  if (copyLen >= destSize) copyLen = destSize - 1;
  memcpy(dest, value.c_str(), copyLen);
  dest[copyLen] = '\0';
}

String jsonEscape(const char* value) {
  String escaped;
  escaped.reserve(strlen(value) + 8);

  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (c == '\\' || c == '\"') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += F("\\n");
    } else if (c == '\r') {
      escaped += F("\\r");
    } else {
      escaped += c;
    }
  }

  return escaped;
}

String htmlEscape(const char* value) {
  String escaped;
  escaped.reserve(strlen(value) + 8);

  for (size_t i = 0; value[i] != '\0'; i++) {
    char c = value[i];
    if (c == '&') escaped += F("&amp;");
    else if (c == '<') escaped += F("&lt;");
    else if (c == '>') escaped += F("&gt;");
    else if (c == '\"') escaped += F("&quot;");
    else if (c == '\'') escaped += F("&#39;");
    else escaped += c;
  }

  return escaped;
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

bool extractJsonBool(const String& json, const char* key, bool& valueOut) {
  String quotedKey = String("\"") + key + "\"";
  int keyPos = json.indexOf(quotedKey);
  if (keyPos < 0) return false;

  int colonPos = json.indexOf(':', keyPos + quotedKey.length());
  if (colonPos < 0) return false;

  int valueStart = colonPos + 1;
  while (valueStart < json.length() && isspace(json[valueStart])) {
    valueStart++;
  }

  if (json.startsWith("true", valueStart)) {
    valueOut = true;
    return true;
  }

  if (json.startsWith("false", valueStart)) {
    valueOut = false;
    return true;
  }

  return false;
}

bool extractJsonString(const String& json, const char* key, String& valueOut) {
  String quotedKey = String("\"") + key + "\"";
  int keyPos = json.indexOf(quotedKey);
  if (keyPos < 0) return false;

  int colonPos = json.indexOf(':', keyPos + quotedKey.length());
  if (colonPos < 0) return false;

  int valueStart = colonPos + 1;
  while (valueStart < json.length() && isspace(json[valueStart])) {
    valueStart++;
  }

  if (valueStart >= json.length() || json[valueStart] != '\"') return false;
  valueStart++;

  String result;
  for (int i = valueStart; i < json.length(); i++) {
    char c = json[i];
    if (c == '\\') {
      i++;
      if (i >= json.length()) return false;
      char escaped = json[i];
      if (escaped == 'n') result += '\n';
      else if (escaped == 'r') result += '\r';
      else result += escaped;
    } else if (c == '\"') {
      valueOut = result;
      return true;
    } else {
      result += c;
    }
  }

  return false;
}

bool readJsonFileIfExists(const char* path, String& jsonOut) {
  if (!littleFsReady || !LittleFS.exists(path)) return false;

  File file = LittleFS.open(path, "r");
  if (!file) {
    Serial.print("Failed to open ");
    Serial.print(path);
    Serial.println(" for read.");
    return false;
  }

  jsonOut = file.readString();
  file.close();
  return true;
}

bool writeJsonFile(const char* path, const String& json) {
  if (!littleFsReady) return false;

  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.print("Failed to open ");
    Serial.print(path);
    Serial.println(" for write.");
    return false;
  }

  size_t written = file.print(json);
  file.close();

  if (written != json.length()) {
    Serial.print("Failed to write full JSON file: ");
    Serial.println(path);
    return false;
  }

  return true;
}

bool deleteFileIfExists(const char* path) {
  if (!littleFsReady || !LittleFS.exists(path)) return true;

  if (LittleFS.remove(path)) return true;

  Serial.print("Failed to remove ");
  Serial.println(path);
  return false;
}

int maxSafeAmplitude() {
  int leftRoom = SERVO_REST_DEG - SERVO_MIN_DEG;
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

  if (a == LOW && b == LOW) return MODE_LAZY;
  if (a == HIGH && b == HIGH) return MODE_PLAYFUL;
  return MODE_ZOOMIES;
}

const char* modeName(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY: return "Lazy";
    case MODE_PLAYFUL: return "Playful";
    case MODE_ZOOMIES: return "Zoomies";
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

const char* scheduleDecisionReasonName(ScheduleDecisionReason reason) {
  switch (reason) {
    case SCHEDULE_DECISION_DISABLED: return "Schedule disabled";
    case SCHEDULE_DECISION_TIME_INVALID: return "Time invalid, auto-start suspended";
    case SCHEDULE_DECISION_IN_WINDOW: return "Inside allowed schedule window";
    case SCHEDULE_DECISION_OUTSIDE_WINDOW: return "Outside allowed schedule window";
  }
  return "Unknown";
}

const char* stationStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "Idle";
    case WL_NO_SSID_AVAIL: return "SSID not found";
    case WL_SCAN_COMPLETED: return "Scan completed";
    case WL_CONNECTED: return "Connected";
    case WL_CONNECT_FAILED: return "Connect failed";
    case WL_CONNECTION_LOST: return "Connection lost";
    case WL_DISCONNECTED: return "Disconnected";
    case WL_WRONG_PASSWORD: return "Wrong password";
    default: return "Unknown";
  }
}

int ledToggleIntervalMs(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY: return 420;
    case MODE_PLAYFUL: return 240;
    case MODE_ZOOMIES: return 140;
  }
  return 240;
}

int stepDelayMsForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY: return LAZY_STEP_DELAY_MS;
    case MODE_PLAYFUL: return PLAYFUL_STEP_DELAY_MS;
    case MODE_ZOOMIES: return ZOOMIES_STEP_DELAY_MS;
  }
  return PLAYFUL_STEP_DELAY_MS;
}

int stepDegForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY: return LAZY_STEP_DEG;
    case MODE_PLAYFUL: return PLAYFUL_STEP_DEG;
    case MODE_ZOOMIES: return ZOOMIES_STEP_DEG;
  }
  return PLAYFUL_STEP_DEG;
}

unsigned long sessionDurationMsForMode(PlayMode mode) {
  switch (mode) {
    case MODE_LAZY: return randRangeUL(30000UL, 45000UL);
    case MODE_PLAYFUL: return randRangeUL(40000UL, 60000UL);
    case MODE_ZOOMIES: return randRangeUL(50000UL, 75000UL);
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
    case MODE_LAZY: loPct = 5; hiPct = 16; break;
    case MODE_PLAYFUL: loPct = 10; hiPct = 28; break;
    case MODE_ZOOMIES: loPct = 15; hiPct = 34; break;
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
    case MODE_LAZY: loPct = 18; hiPct = 42; break;
    case MODE_PLAYFUL: loPct = 35; hiPct = 70; break;
    case MODE_ZOOMIES: loPct = 55; hiPct = 92; break;
  }

  int lo = max2(10, pctOf(maxAmp, loPct));
  int hi = max2(lo + 2, pctOf(maxAmp, hiPct));
  return randRange(lo, hi);
}

String wifiStatusText() {
  if (!hasSavedWifiCredentials) return "AP only (no saved Wi-Fi)";
  if (wifiSetupModeForced) return "AP only (boot gesture setup mode)";

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    return String("AP + STA connected to ") + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")";
  }

  if (stationConnectAttemptActive) return "AP + STA connecting";
  return "AP + STA waiting to reconnect";
}

String timeStatusText() {
  if (!hasSavedWifiCredentials) return "Time unavailable (no saved Wi-Fi)";
  if (wifiSetupModeForced) return "Time unavailable in setup mode";
  if (timeValid) return "Local time valid";
  if (WiFi.status() == WL_CONNECTED && timeSyncRequested) return "Waiting for NTP time";
  if (WiFi.status() == WL_CONNECTED) return "Wi-Fi connected, time sync not started";
  return "Waiting for Wi-Fi before time sync";
}

String localTimeDisplayText() {
  if (!timeValid || !isTimeValid()) return "Not available yet";

  time_t now = time(nullptr);
  struct tm localTime;
  if (localtime_r(&now, &localTime) == nullptr) return "Not available yet";

  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localTime);
  return String(buf);
}

String wifiDiagnosticsHtml(unsigned long now) {
  String html;
  html.reserve(640);

  wl_status_t status = WiFi.status();
  html += F("<div><strong>Saved SSID:</strong> ");
  html += (WIFI_SSID[0] != '\0') ? htmlEscape(WIFI_SSID) : String("(none)");
  html += F("</div><div><strong>STA status:</strong> ");
  html += stationStatusName(status);
  html += F("</div><div><strong>Last STA status change:</strong> ");
  html += String((now - lastStationStatusChangeMs) / 1000UL);
  html += F(" s ago</div>");

  if (stationConnectAttemptActive) {
    html += F("<div><strong>Current connect attempt:</strong> ");
    html += String((now - wifiConnectAttemptStartMs) / 1000UL);
    html += F(" s elapsed</div>");
  } else if (hasSavedWifiCredentials && !wifiSetupModeForced && networkBootStarted && status != WL_CONNECTED) {
    unsigned long retryMs = 0;
    if (lastWifiConnectAttemptMs > 0) {
      unsigned long elapsed = now - lastWifiConnectAttemptMs;
      retryMs = (elapsed >= WIFI_RETRY_INTERVAL_MS) ? 0 : (WIFI_RETRY_INTERVAL_MS - elapsed);
    }

    html += F("<div><strong>Next reconnect attempt:</strong> ");
    html += String(retryMs / 1000UL);
    html += F(" s</div>");
  }

  if (status == WL_CONNECTED) {
    html += F("<div><strong>STA IP:</strong> ");
    html += WiFi.localIP().toString();
    html += F("</div>");
  }

  html += F("<div><strong>NTP sync requested:</strong> ");
  html += timeSyncRequested ? "yes" : "no";
  html += F("</div>");

  return html;
}

String formatMinuteOfDay(int minuteOfDay) {
  if (!validateScheduleMinute(minuteOfDay)) return "--:--";

  int hour = minuteOfDay / 60;
  int minute = minuteOfDay % 60;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
  return String(buf);
}

String scheduleWindowLabel(int startMinute, int endMinute) {
  String label = formatMinuteOfDay(startMinute);
  label += " to ";
  label += formatMinuteOfDay(endMinute);

  if (startMinute == endMinute) {
    label += " (all day)";
  } else if (startMinute > endMinute) {
    label += " (overnight)";
  }

  return label;
}

bool parseTimeArg(const String& value, int& minuteOut) {
  if (value.length() != 5 || value[2] != ':') return false;

  int hour = value.substring(0, 2).toInt();
  int minute = value.substring(3, 5).toInt();

  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;

  minuteOut = hour * 60 + minute;
  return true;
}

const char* timezoneLabelForTz(const char* tz) {
  for (size_t i = 0; i < sizeof(TIMEZONE_OPTIONS) / sizeof(TIMEZONE_OPTIONS[0]); i++) {
    if (strcmp(tz, TIMEZONE_OPTIONS[i].tz) == 0) return TIMEZONE_OPTIONS[i].label;
  }

  return "Custom";
}

bool configHasSavedWifiCredentials() {
  return WIFI_SSID[0] != '\0';
}

void applyWifiCredentials(const String& ssid, const String& password) {
  copyBoundedString(WIFI_SSID, sizeof(WIFI_SSID), ssid);
  copyBoundedString(WIFI_PASS, sizeof(WIFI_PASS), password);
  hasSavedWifiCredentials = configHasSavedWifiCredentials();
}

void applyTimezoneSetting(const String& tz) {
  copyBoundedString(TIMEZONE_TZ, sizeof(TIMEZONE_TZ), tz);
  timezoneApplied = false;
}

void applyScheduleConfig(bool enabled, int startMinute, int endMinute,
                         bool secondEnabled, int secondStartMinute, int secondEndMinute) {
  SCHEDULE_ENABLED = enabled;
  SCHEDULE_START_MINUTE = startMinute;
  SCHEDULE_END_MINUTE = endMinute;
  SCHEDULE_SECOND_ENABLED = secondEnabled;
  SCHEDULE_SECOND_START_MINUTE = secondStartMinute;
  SCHEDULE_SECOND_END_MINUTE = secondEndMinute;
}

void refreshScheduleRuntimeStateAfterConfigChange(bool allowImmediateStart) {
  scheduleWindowWasOpen = isScheduleWindowOpenNow();

  if (!allowImmediateStart) {
    scheduleStartPending = false;
    return;
  }

  if (scheduleWindowWasOpen &&
      controllerState == STATE_RESTING && !forceSessionRequested && !parkRequested) {
    nextAutoSessionMs = millis();
    scheduleStartPending = false;
  } else {
    scheduleStartPending = scheduleWindowWasOpen;
  }
}

void applyDefaultConfig() {
  applyServoWindow(DEFAULT_SERVO_MIN_DEG, DEFAULT_SERVO_MAX_DEG);
  applyStepDelays(DEFAULT_LAZY_STEP_DELAY_MS, DEFAULT_PLAYFUL_STEP_DELAY_MS, DEFAULT_ZOOMIES_STEP_DELAY_MS);
  applyRestWindowMinutes(DEFAULT_AUTO_REST_MIN_MINUTES, DEFAULT_AUTO_REST_MAX_MINUTES);
  applyWifiCredentials("", "");
  applyTimezoneSetting(DEFAULT_TIMEZONE_TZ);
  applyScheduleConfig(DEFAULT_SCHEDULE_ENABLED,
                      DEFAULT_SCHEDULE_START_MINUTE,
                      DEFAULT_SCHEDULE_END_MINUTE,
                      DEFAULT_SCHEDULE_SECOND_ENABLED,
                      DEFAULT_SCHEDULE_SECOND_START_MINUTE,
                      DEFAULT_SCHEDULE_SECOND_END_MINUTE);
}

bool clearAllSavedConfig() {
  bool ok = true;
  ok = deleteFileIfExists(MOTION_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(WIFI_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(SCHEDULE_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(LEGACY_CONFIG_PATH) && ok;
  return ok;
}

void setLed(bool on) {
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void logScheduleDecisionIfChanged(ScheduleDecisionReason reason) {
  if (reason == lastScheduleDecisionReason) return;

  lastScheduleDecisionReason = reason;
  Serial.print("Schedule gate: ");
  Serial.println(scheduleDecisionReasonName(reason));
}

bool isTimeValid() {
  time_t now = time(nullptr);
  return now >= (time_t)TIME_VALID_MIN_EPOCH;
}

int currentLocalMinuteOfDay() {
  time_t now = time(nullptr);
  struct tm localTime;
  if (localtime_r(&now, &localTime) == nullptr) return -1;

  return (localTime.tm_hour * 60) + localTime.tm_min;
}

bool isMinuteWithinScheduleWindow(int currentMinute, int startMinute, int endMinute) {
  if (startMinute == endMinute) return true;

  if (startMinute < endMinute) {
    return currentMinute >= startMinute && currentMinute < endMinute;
  }

  return currentMinute >= startMinute || currentMinute < endMinute;
}

bool isScheduleWindowSetOpenNow(int currentMinute, int startMinute, int endMinute,
                                bool secondEnabled, int secondStartMinute, int secondEndMinute) {
  if (isMinuteWithinScheduleWindow(currentMinute, startMinute, endMinute)) return true;
  if (!secondEnabled) return false;

  return isMinuteWithinScheduleWindow(currentMinute, secondStartMinute, secondEndMinute);
}

bool isScheduleWindowOpenNow() {
  if (!SCHEDULE_ENABLED) return true;
  if (!timeValid || !isTimeValid()) return false;

  int currentMinute = currentLocalMinuteOfDay();
  if (currentMinute < 0) return false;

  return isScheduleWindowSetOpenNow(currentMinute,
                                    SCHEDULE_START_MINUTE,
                                    SCHEDULE_END_MINUTE,
                                    SCHEDULE_SECOND_ENABLED,
                                    SCHEDULE_SECOND_START_MINUTE,
                                    SCHEDULE_SECOND_END_MINUTE);
}

void applyTimezoneIfNeeded() {
  if (timezoneApplied || TIMEZONE_TZ[0] == '\0') return;

  setenv("TZ", TIMEZONE_TZ, 1);
  tzset();
  timezoneApplied = true;

  Serial.print("Timezone applied: ");
  Serial.print(timezoneLabelForTz(TIMEZONE_TZ));
  Serial.print(" (");
  Serial.print(TIMEZONE_TZ);
  Serial.println(")");
}

void resetTimeState(const char* reason) {
  bool hadValidTime = timeValid;
  timeValid = false;
  timeSyncRequested = false;
  ntpSyncStartMs = 0;
  scheduleWindowWasOpen = false;
  scheduleStartPending = false;

  if (reason != nullptr) {
    Serial.print("Time state reset: ");
    Serial.println(reason);
  }

  if (hadValidTime) {
    logScheduleDecisionIfChanged(SCHEDULE_DECISION_TIME_INVALID);
  }
}

void beginTimeSync() {
  if (timeSyncRequested || WiFi.status() != WL_CONNECTED) return;

  configTime(TIMEZONE_TZ, NTP_SERVER_1, NTP_SERVER_2);
  timezoneApplied = true;
  timeSyncRequested = true;
  ntpSyncStartMs = millis();
  timeValid = false;

  Serial.println("NTP sync started.");
}

void startStationConnectionAttempt() {
  if (!hasSavedWifiCredentials || wifiSetupModeForced) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  stationConnectAttemptActive = true;
  networkBootStarted = true;
  wifiConnectAttemptStartMs = millis();
  lastWifiConnectAttemptMs = wifiConnectAttemptStartMs;

  Serial.print("Station connect started for SSID: ");
  Serial.println(WIFI_SSID);
}

void startNetworkBootIfConfigured() {
  if (networkBootStarted || bootGestureWindowClosed == false) return;
  if (!hasSavedWifiCredentials) {
    networkBootStarted = true;
    Serial.println("No saved Wi-Fi credentials. Staying in AP-only mode.");
    return;
  }

  if (wifiSetupModeForced) {
    networkBootStarted = true;
    Serial.println("Boot gesture requested setup mode. Staying in AP-only mode.");
    return;
  }

  startStationConnectionAttempt();
}

void serviceStationConnection() {
  wl_status_t status = WiFi.status();

  if (status != lastLoggedStationStatus) {
    lastLoggedStationStatus = status;
    lastStationStatusChangeMs = millis();
    Serial.print("Station status changed: ");
    Serial.println(stationStatusName(status));
  }

  if (status == WL_CONNECTED) {
    if (!stationWasConnected) {
      stationWasConnected = true;
      stationConnectAttemptActive = false;
      Serial.print("Station connected. IP: ");
      Serial.println(WiFi.localIP());
      beginTimeSync();
    }
    return;
  }

  if (stationWasConnected) {
    stationWasConnected = false;
    Serial.println("Station disconnected.");
    resetTimeState("Wi-Fi disconnected");
  }

  if (!hasSavedWifiCredentials || wifiSetupModeForced || !networkBootStarted) return;

  if (stationConnectAttemptActive && millis() - wifiConnectAttemptStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
    stationConnectAttemptActive = false;
    Serial.println("Station connect timed out. Will retry while keeping AP available.");
  }

  if (!stationConnectAttemptActive && millis() - lastWifiConnectAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    startStationConnectionAttempt();
  }
}

void serviceTimeState() {
  if (WiFi.status() != WL_CONNECTED) return;

  // Re-apply any pending timezone change even while an NTP sync is already in flight.
  applyTimezoneIfNeeded();

  if (!timeSyncRequested) {
    beginTimeSync();
    return;
  }

  bool nowValid = isTimeValid();
  if (nowValid && !timeValid) {
    timeValid = true;

    time_t now = time(nullptr);
    struct tm localTime;
    localtime_r(&now, &localTime);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &localTime);

    Serial.print("Local time is valid: ");
    Serial.println(buf);
    return;
  }

  if (!nowValid && timeSyncRequested && ntpSyncStartMs != 0 &&
      millis() - ntpSyncStartMs >= NTP_SYNC_TIMEOUT_MS) {
    Serial.println("NTP time still invalid after timeout; scheduled auto-start remains suspended.");
    ntpSyncStartMs = millis();
  }
}

void serviceBootGesture(unsigned long now) {
  if (bootGestureWindowClosed) return;

  PlayMode rawMode = readModeSwitch();
  if (rawMode != lastBootGestureRawMode) {
    lastBootGestureRawMode = rawMode;
    lastBootGestureRawChangeMs = now;
  }

  if (rawMode != bootGestureStableMode &&
      now - lastBootGestureRawChangeMs >= BOOT_GESTURE_STABLE_MS) {
    bootGestureStableMode = rawMode;
    Serial.print("Boot gesture stable mode: ");
    Serial.println(modeName(bootGestureStableMode));

    if (bootGestureStage == 0 && bootGestureStableMode == MODE_LAZY) {
      bootGestureStage = 1;
    } else if (bootGestureStage == 1 && bootGestureStableMode == MODE_PLAYFUL) {
      bootGestureStage = 2;
    } else if (bootGestureStage == 2 && bootGestureStableMode == MODE_LAZY) {
      bootGestureStage = 3;
      wifiSetupModeForced = true;
      networkBootStarted = true;
      Serial.println("Boot gesture detected: entering Wi-Fi setup mode for this boot.");
    }
  }

  if (now >= bootGestureWindowEndMs) {
    bootGestureWindowClosed = true;
    Serial.println("Boot gesture window closed.");
  }
}

void serviceRuntimeState() {
  unsigned long now = millis();

  serviceBootGesture(now);
  startNetworkBootIfConfigured();
  serviceStationConnection();
  serviceTimeState();

  bool scheduleWindowOpenNow = isScheduleWindowOpenNow();
  if (scheduleWindowOpenNow && !scheduleWindowWasOpen) {
    if (controllerState == STATE_RESTING && !forceSessionRequested && !parkRequested) {
      nextAutoSessionMs = millis();
    } else {
      scheduleStartPending = true;
    }
  }
  scheduleWindowWasOpen = scheduleWindowOpenNow;

  if (scheduleStartPending && controllerState == STATE_RESTING &&
      !forceSessionRequested && !parkRequested && scheduleWindowOpenNow) {
    nextAutoSessionMs = millis();
    scheduleStartPending = false;
  }

  if (!scheduleWindowOpenNow) {
    scheduleStartPending = false;
  }
}

void updateLed(PlayMode mode, unsigned long now) {
  if (!LED_FLASH) {
    if (!ledState) {
      ledState = true;
      setLed(true);
    }
    lastLedToggleMs = now;
    return;
  }

  int interval = ledToggleIntervalMs(mode);

  if (now - lastLedToggleMs >= (unsigned long)interval) {
    ledState = !ledState;
    setLed(ledState);
    lastLedToggleMs = now;
  }
}

void serviceNetwork() {
  server.handleClient();
  serviceRuntimeState();
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
  toDeg = clampAngle(toDeg);

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
  toDeg = clampAngle(toDeg);

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

bool saveMotionConfig() {
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

  return writeJsonFile(MOTION_CONFIG_PATH, json);
}

bool saveWifiConfig() {
  String json = "{\"wifiSsid\":\"";
  json += jsonEscape(WIFI_SSID);
  json += "\",\"wifiPass\":\"";
  json += jsonEscape(WIFI_PASS);
  json += "\"}\n";

  return writeJsonFile(WIFI_CONFIG_PATH, json);
}

bool saveScheduleConfig() {
  String json = "{\"timezoneTz\":\"";
  json += jsonEscape(TIMEZONE_TZ);
  json += "\",\"scheduleEnabled\":";
  json += SCHEDULE_ENABLED ? "true" : "false";
  json += ",\"scheduleStartMinute\":";
  json += String(SCHEDULE_START_MINUTE);
  json += ",\"scheduleEndMinute\":";
  json += String(SCHEDULE_END_MINUTE);
  json += ",\"scheduleSecondEnabled\":";
  json += SCHEDULE_SECOND_ENABLED ? "true" : "false";
  json += ",\"scheduleSecondStartMinute\":";
  json += String(SCHEDULE_SECOND_START_MINUTE);
  json += ",\"scheduleSecondEndMinute\":";
  json += String(SCHEDULE_SECOND_END_MINUTE);
  json += "}\n";

  return writeJsonFile(SCHEDULE_CONFIG_PATH, json);
}

void loadMotionConfig() {
  if (!littleFsReady) return;

  String json;
  bool loadedFromLegacy = false;
  if (!readJsonFileIfExists(MOTION_CONFIG_PATH, json)) {
    if (!readJsonFileIfExists(LEGACY_CONFIG_PATH, json)) {
      Serial.println("No saved motion config found. Using defaults.");
      return;
    }
    loadedFromLegacy = true;
  }

  int savedMin = 0;
  int savedMax = 0;
  int savedLazyDelay = LAZY_STEP_DELAY_MS;
  int savedPlayfulDelay = PLAYFUL_STEP_DELAY_MS;
  int savedZoomiesDelay = ZOOMIES_STEP_DELAY_MS;
  int savedRestMinMinutes = AUTO_REST_MIN_MINUTES;
  int savedRestMaxMinutes = AUTO_REST_MAX_MINUTES;

  if (!extractJsonInt(json, "servoMinDeg", savedMin) ||
      !extractJsonInt(json, "servoMaxDeg", savedMax)) {
    Serial.println("Motion config missing required fields. Using defaults.");
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

  Serial.print("Loaded servo window from ");
  Serial.print(loadedFromLegacy ? LEGACY_CONFIG_PATH : MOTION_CONFIG_PATH);
  Serial.print(": ");
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

void loadWifiConfig() {
  if (!littleFsReady) return;

  String json;
  bool loadedFromLegacy = false;
  if (!readJsonFileIfExists(WIFI_CONFIG_PATH, json)) {
    if (!readJsonFileIfExists(LEGACY_CONFIG_PATH, json)) {
      Serial.println("No saved Wi-Fi config found. Using defaults.");
      return;
    }
    loadedFromLegacy = true;
  }

  String savedWifiSsid = WIFI_SSID;
  String savedWifiPass = WIFI_PASS;

  if (extractJsonString(json, "wifiSsid", savedWifiSsid)) {
    if (savedWifiSsid.length() > WIFI_SSID_MAX_LEN) {
      Serial.println("Saved Wi-Fi SSID too long. Ignoring saved SSID.");
      savedWifiSsid = "";
    }
  }

  if (extractJsonString(json, "wifiPass", savedWifiPass)) {
    if (savedWifiPass.length() > WIFI_PASS_MAX_LEN) {
      Serial.println("Saved Wi-Fi password too long. Ignoring saved password.");
      savedWifiPass = "";
    }
  }

  applyWifiCredentials(savedWifiSsid, savedWifiPass);
  Serial.print("Loaded Wi-Fi SSID from ");
  Serial.print(loadedFromLegacy ? LEGACY_CONFIG_PATH : WIFI_CONFIG_PATH);
  Serial.print(": ");
  Serial.println(hasSavedWifiCredentials ? WIFI_SSID : "(none)");
}

void loadScheduleConfig() {
  if (!littleFsReady) return;

  String json;
  bool loadedFromLegacy = false;
  if (!readJsonFileIfExists(SCHEDULE_CONFIG_PATH, json)) {
    if (!readJsonFileIfExists(LEGACY_CONFIG_PATH, json)) {
      Serial.println("No saved schedule config found. Using defaults.");
      return;
    }
    loadedFromLegacy = true;
  }

  int savedScheduleStartMinute = SCHEDULE_START_MINUTE;
  int savedScheduleEndMinute = SCHEDULE_END_MINUTE;
  bool savedScheduleSecondEnabled = SCHEDULE_SECOND_ENABLED;
  int savedScheduleSecondStartMinute = SCHEDULE_SECOND_START_MINUTE;
  int savedScheduleSecondEndMinute = SCHEDULE_SECOND_END_MINUTE;
  bool savedScheduleEnabled = SCHEDULE_ENABLED;
  String savedTimezoneTz = TIMEZONE_TZ;

  if (extractJsonString(json, "timezoneTz", savedTimezoneTz)) {
    if (!validateTimezoneTz(savedTimezoneTz)) {
      Serial.println("Saved timezone invalid. Using default timezone.");
      savedTimezoneTz = TIMEZONE_TZ;
    }
  }

  bool hasScheduleEnabled = extractJsonBool(json, "scheduleEnabled", savedScheduleEnabled);
  bool hasScheduleStart = extractJsonInt(json, "scheduleStartMinute", savedScheduleStartMinute);
  bool hasScheduleEnd = extractJsonInt(json, "scheduleEndMinute", savedScheduleEndMinute);
  bool hasScheduleSecondEnabled = extractJsonBool(json, "scheduleSecondEnabled", savedScheduleSecondEnabled);
  bool hasScheduleSecondStart = extractJsonInt(json, "scheduleSecondStartMinute", savedScheduleSecondStartMinute);
  bool hasScheduleSecondEnd = extractJsonInt(json, "scheduleSecondEndMinute", savedScheduleSecondEndMinute);

  if (hasScheduleStart && !validateScheduleMinute(savedScheduleStartMinute)) {
    Serial.println("Saved schedule start minute invalid. Using default schedule start.");
    savedScheduleStartMinute = SCHEDULE_START_MINUTE;
  }

  if (hasScheduleEnd && !validateScheduleMinute(savedScheduleEndMinute)) {
    Serial.println("Saved schedule end minute invalid. Using default schedule end.");
    savedScheduleEndMinute = SCHEDULE_END_MINUTE;
  }

  if (hasScheduleSecondStart && !validateScheduleMinute(savedScheduleSecondStartMinute)) {
    Serial.println("Saved second schedule start minute invalid. Using default second schedule start.");
    savedScheduleSecondStartMinute = SCHEDULE_SECOND_START_MINUTE;
  }

  if (hasScheduleSecondEnd && !validateScheduleMinute(savedScheduleSecondEndMinute)) {
    Serial.println("Saved second schedule end minute invalid. Using default second schedule end.");
    savedScheduleSecondEndMinute = SCHEDULE_SECOND_END_MINUTE;
  }

  applyTimezoneSetting(savedTimezoneTz);

  if (hasScheduleEnabled || hasScheduleStart || hasScheduleEnd ||
      hasScheduleSecondEnabled || hasScheduleSecondStart || hasScheduleSecondEnd) {
    applyScheduleConfig(savedScheduleEnabled,
                        savedScheduleStartMinute,
                        savedScheduleEndMinute,
                        savedScheduleSecondEnabled,
                        savedScheduleSecondStartMinute,
                        savedScheduleSecondEndMinute);
  }

  Serial.print("Loaded timezone: ");
  Serial.print(loadedFromLegacy ? LEGACY_CONFIG_PATH : SCHEDULE_CONFIG_PATH);
  Serial.print(" -> ");
  Serial.println(TIMEZONE_TZ);
  Serial.print("Loaded schedule: ");
  Serial.print(SCHEDULE_ENABLED ? "enabled " : "disabled ");
  Serial.print(formatMinuteOfDay(SCHEDULE_START_MINUTE));
  Serial.print(" to ");
  Serial.print(formatMinuteOfDay(SCHEDULE_END_MINUTE));
  if (SCHEDULE_SECOND_ENABLED) {
    Serial.print(" and ");
    Serial.print(formatMinuteOfDay(SCHEDULE_SECOND_START_MINUTE));
    Serial.print(" to ");
    Serial.print(formatMinuteOfDay(SCHEDULE_SECOND_END_MINUTE));
  }
  Serial.println();
}

bool canAutoStartSessions() {
  if (!SCHEDULE_ENABLED) {
    logScheduleDecisionIfChanged(SCHEDULE_DECISION_DISABLED);
    return true;
  }

  if (!timeValid || !isTimeValid()) {
    logScheduleDecisionIfChanged(SCHEDULE_DECISION_TIME_INVALID);
    return false;
  }

  int currentMinute = currentLocalMinuteOfDay();
  if (currentMinute < 0) {
    logScheduleDecisionIfChanged(SCHEDULE_DECISION_TIME_INVALID);
    return false;
  }

  if (isScheduleWindowOpenNow()) {
    logScheduleDecisionIfChanged(SCHEDULE_DECISION_IN_WINDOW);
    return true;
  }

  logScheduleDecisionIfChanged(SCHEDULE_DECISION_OUTSIDE_WINDOW);
  return false;
}

bool doTease(PlayMode modeAtStart) {
  int amp = teaseAmpForMode(modeAtStart);
  int left = clampAngle(SERVO_REST_DEG - amp);
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
  int left = clampAngle(SERVO_REST_DEG - amp);
  int right = clampAngle(SERVO_REST_DEG + amp);
  int speed = dartSpeedForMode(modeAtStart);
  int dartStepDeg = stepDegForMode(modeAtStart) + DART_EXTRA_STEP_DEG;

  bool startLeft = (random(2) == 0);
  int start = startLeft ? left : right;
  int end = startLeft ? right : left;

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
    case MODE_LAZY: return delayResponsive((unsigned long)randRange(2500, 4500), modeAtStart);
    case MODE_PLAYFUL: return delayResponsive((unsigned long)randRange(2200, 4000), modeAtStart);
    case MODE_ZOOMIES: return delayResponsive((unsigned long)randRange(1800, 3200), modeAtStart);
  }
  return false;
}

bool shortPause(PlayMode modeAtStart) {
  switch (modeAtStart) {
    case MODE_LAZY: return delayResponsive((unsigned long)randRange(900, 2200), modeAtStart);
    case MODE_PLAYFUL: return delayResponsive((unsigned long)randRange(600, 1800), modeAtStart);
    case MODE_ZOOMIES: return delayResponsive((unsigned long)randRange(350, 1200), modeAtStart);
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
  unsigned long endMs = startMs + sessionMs;

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

String htmlSelectedAttr(bool selected) {
  return selected ? F(" selected") : String("");
}

String htmlPage() {
  PlayMode mode = readModeSwitch();
  unsigned long now = millis();

  String page;
  page.reserve(9800);

  page += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cat Toy Control</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:780px;margin:24px auto;padding:0 16px;line-height:1.45;}"
    ".card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-bottom:16px;}"
    ".hint{background:#f7f7f7;border-left:4px solid #4f6d4a;}"
    ".meta{color:#444;font-size:14px;}"
    ".actions form{display:inline-block;margin:8px 8px 0 0;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
    "input[type=number],input[type=time],input[type=text],input[type=password],select{width:100%;padding:10px;font-size:16px;box-sizing:border-box;}"
    "button{margin-top:16px;padding:12px 16px;font-size:16px;border-radius:10px;border:0;cursor:pointer;}"
    ".secondary{background:#ececec;color:#111;}"
    ".danger{background:#b64332;color:#fff;}"
    ".status{display:grid;grid-template-columns:1fr;gap:6px;}"
    "</style></head><body>"
    "<h1>Cat Toy Control</h1>"
  );

  page += F("<div class='card status'>");
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
  page += F(" min</div><div><strong>Wi-Fi state:</strong> ");
  page += wifiStatusText();
  page += F("</div><div><strong>Time state:</strong> ");
  page += timeStatusText();
  page += F("</div><div><strong>Local time:</strong> ");
  page += localTimeDisplayText();
  page += F("</div><div><strong>Timezone:</strong> ");
  page += timezoneLabelForTz(TIMEZONE_TZ);
  page += F("</div><div><strong>Schedule:</strong> ");
  page += scheduleDecisionReasonName(lastScheduleDecisionReason);
  page += F("</div><div><strong>Schedule window 1:</strong> ");
  page += scheduleWindowLabel(SCHEDULE_START_MINUTE, SCHEDULE_END_MINUTE);
  page += F("</div><div><strong>Schedule window 2:</strong> ");
  if (SCHEDULE_SECOND_ENABLED) {
    page += scheduleWindowLabel(SCHEDULE_SECOND_START_MINUTE, SCHEDULE_SECOND_END_MINUTE);
  } else {
    page += F("Disabled");
  }
  page += F("</div>");
  page += wifiDiagnosticsHtml(now);

  if (controllerState == STATE_STARTUP) {
    unsigned long warmupRemainingMs = (startupEndMs > now) ? (startupEndMs - now) : 0;
    unsigned long gestureRemainingMs = (bootGestureWindowEndMs > now) ? (bootGestureWindowEndMs - now) : 0;
    page += F("<div><strong>Warmup remaining:</strong> ");
    page += String(warmupRemainingMs / 1000UL);
    page += F(" s</div><div><strong>Boot gesture window:</strong> ");
    page += String(gestureRemainingMs / 1000UL);
    page += F(" s remaining</div>");
  } else {
    unsigned long nextInMs = (nextAutoSessionMs > now) ? (nextAutoSessionMs - now) : 0;
    page += F("<div><strong>Next auto session timer:</strong> ");
    page += String(nextInMs / 1000UL);
    page += F(" s</div>");
  }

  page += F("</div>");

  page += F("<div class='card hint'><strong>Runtime model</strong>"
            "<p class='meta'>Manual Start Session Now still works any time. The saved schedule only gates autonomous starts when local time is valid.</p>"
            "<p class='meta'>If saved Wi-Fi is available, the toy keeps the local AP UI up while it also attempts station Wi-Fi and NTP time in the background.</p></div>");

  page += F("<div class='card actions'><strong>Manual control</strong>"
            "<form action='/start' method='post'><button type='submit'>Start Session Now</button></form>"
            "<form action='/park' method='post'><button class='secondary' type='submit'>Park At Rest</button></form>"
            "<p class='meta'>Start now forces the next session immediately, even outside the saved schedule. Park ends the current session as soon as the sketch reaches an interrupt check and returns the servo to rest.</p></div>");

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

  page += F("<button type='submit'>Apply Motion Settings</button>");
  page += F("</form>");
  page += F("<p class='meta'>Servo limits must keep max &gt; min with at least 20 degrees of spread. Rest remains auto-centered in the safe window.</p>");
  page += F("</div>");

  page += F("<div class='card'><strong>Wi-Fi</strong>");
  page += F("<form action='/wifi' method='post'>");
  page += F("<label for='wifiSsid'>Home Wi-Fi SSID</label>");
  page += F("<input id='wifiSsid' name='wifiSsid' type='text' maxlength='32' value='");
  page += htmlEscape(WIFI_SSID);
  page += F("'>");

  page += F("<label for='wifiPass'>Home Wi-Fi password</label>");
  page += F("<input id='wifiPass' name='wifiPass' type='password' maxlength='63' value='");
  page += htmlEscape(WIFI_PASS);
  page += F("'>");
  page += F("<button type='submit'>Save Wi-Fi</button>");
  page += F("</form>");
  page += F("<form action='/clear-wifi' method='post'><button class='danger' type='submit'>Clear Saved Wi-Fi</button></form>");
  page += F("<p class='meta'>Use the boot gesture Lazy -> Playful -> Lazy during the first 4 seconds after boot to keep the toy in AP-only Wi-Fi setup mode for that boot.</p>");
  page += F("</div>");

  page += F("<div class='card'><strong>Schedule</strong>");
  page += F("<form action='/schedule' method='post'>");
  page += F("<label for='timezoneTz'>Timezone</label><select id='timezoneTz' name='timezoneTz'>");
  for (size_t i = 0; i < sizeof(TIMEZONE_OPTIONS) / sizeof(TIMEZONE_OPTIONS[0]); i++) {
    page += F("<option value='");
    page += TIMEZONE_OPTIONS[i].tz;
    page += F("'");
    page += htmlSelectedAttr(strcmp(TIMEZONE_TZ, TIMEZONE_OPTIONS[i].tz) == 0);
    page += F(">");
    page += TIMEZONE_OPTIONS[i].label;
    page += F("</option>");
  }
  page += F("</select>");

  page += F("<label for='scheduleStart'>Daily schedule window 1 start</label>");
  page += F("<input id='scheduleStart' name='scheduleStart' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_START_MINUTE);
  page += F("'>");

  page += F("<label for='scheduleEnd'>Daily schedule window 1 end</label>");
  page += F("<input id='scheduleEnd' name='scheduleEnd' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_END_MINUTE);
  page += F("'>");

  page += F("<label><input name='scheduleSecondEnabled' type='checkbox' value='1'");
  if (SCHEDULE_SECOND_ENABLED) page += F(" checked");
  page += F("> Enable second daily window</label>");

  page += F("<label for='scheduleSecondStart'>Daily schedule window 2 start</label>");
  page += F("<input id='scheduleSecondStart' name='scheduleSecondStart' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_SECOND_START_MINUTE);
  page += F("'>");

  page += F("<label for='scheduleSecondEnd'>Daily schedule window 2 end</label>");
  page += F("<input id='scheduleSecondEnd' name='scheduleSecondEnd' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_SECOND_END_MINUTE);
  page += F("'>");

  page += F("<button type='submit'>Save Schedule</button>");
  page += F("</form>");
  page += F("<form action='/schedule-toggle' method='post'>");
  page += F("<button class='secondary' type='submit'>");
  page += (SCHEDULE_ENABLED ? F("Disable Schedule") : F("Enable Schedule"));
  page += F("</button></form>");
  page += F("<p class='meta'>Schedule is currently ");
  page += (SCHEDULE_ENABLED ? F("enabled") : F("disabled"));
  page += F(". Save Schedule updates the timezone and daily windows without changing that state.</p>");
  page += F("<p class='meta'>Each window treats matching start and end times as all day. Overnight windows are supported for both windows.</p>");
  page += F("</div>");

  page += F("<div class='card'><strong>Maintenance</strong>");
  page += F("<form action='/clear-data' method='post' onsubmit=\"return confirm('Clear all saved settings and return to defaults?');\">");
  page += F("<button class='danger' type='submit'>Clear All Saved Data</button>");
  page += F("</form>");
  page += F("<p class='meta'>This deletes all saved motion, Wi-Fi, and schedule data from LittleFS, including the legacy config file, and resets this boot to default settings.</p>");
  page += F("</div>");

  page += F("<div class='card meta'>Connect to Wi-Fi <strong>");
  page += AP_SSID;
  page += F("</strong> and browse to <strong>http://192.168.4.1</strong>. The local AP stays available even when station Wi-Fi is configured.</div>");

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

  if (!saveMotionConfig()) {
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

void handleWifiSave() {
  String wifiSsid = server.arg("wifiSsid");
  String wifiPass = server.arg("wifiPass");

  if (wifiSsid.length() > WIFI_SSID_MAX_LEN) {
    server.send(400, "text/plain", "Wi-Fi SSID must be 32 characters or fewer.");
    return;
  }

  if (wifiPass.length() > WIFI_PASS_MAX_LEN) {
    server.send(400, "text/plain", "Wi-Fi password must be 63 characters or fewer.");
    return;
  }

  if (wifiSsid.length() > 0 && wifiPass.length() > 0 && wifiPass.length() < 8) {
    server.send(400, "text/plain", "Wi-Fi password must be at least 8 characters when Wi-Fi is saved.");
    return;
  }

  if (wifiSsid.length() == 0) {
    wifiPass = "";
  }

  applyWifiCredentials(wifiSsid, wifiPass);

  if (!saveWifiConfig()) {
    server.send(500, "text/plain", "Updated Wi-Fi settings for this boot, but failed to save to LittleFS.");
    return;
  }

  wifiSetupModeForced = false;
  bootGestureWindowClosed = true;
  networkBootStarted = false;
  stationConnectAttemptActive = false;
  stationWasConnected = false;

  if (hasSavedWifiCredentials) {
    WiFi.disconnect();
    resetTimeState("Wi-Fi config updated");
    Serial.println("Saved Wi-Fi settings updated. Starting AP+STA background connection.");
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    bool apOk = WiFi.softAP(AP_SSID, AP_PASS);
    resetTimeState("Wi-Fi credentials cleared by empty save");
    networkBootStarted = true;
    Serial.print("Wi-Fi credentials cleared by save. AP status: ");
    Serial.println(apOk ? "READY" : "FAILED");
  }

  logScheduleDecisionIfChanged(SCHEDULE_ENABLED ? SCHEDULE_DECISION_TIME_INVALID : SCHEDULE_DECISION_DISABLED);

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleScheduleSave() {
  if (!server.hasArg("timezoneTz") || !server.hasArg("scheduleStart") || !server.hasArg("scheduleEnd") ||
      !server.hasArg("scheduleSecondStart") || !server.hasArg("scheduleSecondEnd")) {
    server.send(400, "text/plain", "Missing one or more required schedule parameters.");
    return;
  }

  String timezoneTz = server.arg("timezoneTz");
  int scheduleStartMinute = 0;
  int scheduleEndMinute = 0;
  bool scheduleSecondEnabled = server.hasArg("scheduleSecondEnabled");
  int scheduleSecondStartMinute = 0;
  int scheduleSecondEndMinute = 0;

  if (!validateTimezoneTz(timezoneTz)) {
    server.send(400, "text/plain", "Timezone selection is invalid.");
    return;
  }

  if (!parseTimeArg(server.arg("scheduleStart"), scheduleStartMinute) ||
      !parseTimeArg(server.arg("scheduleEnd"), scheduleEndMinute) ||
      !parseTimeArg(server.arg("scheduleSecondStart"), scheduleSecondStartMinute) ||
      !parseTimeArg(server.arg("scheduleSecondEnd"), scheduleSecondEndMinute)) {
    server.send(400, "text/plain", "Schedule times must be valid HH:MM values.");
    return;
  }

  if (!validateScheduleMinute(scheduleStartMinute) || !validateScheduleMinute(scheduleEndMinute) ||
      !validateScheduleMinute(scheduleSecondStartMinute) || !validateScheduleMinute(scheduleSecondEndMinute)) {
    server.send(400, "text/plain", "Schedule times must stay within one day.");
    return;
  }

  applyTimezoneSetting(timezoneTz);
  applyScheduleConfig(SCHEDULE_ENABLED,
                      scheduleStartMinute,
                      scheduleEndMinute,
                      scheduleSecondEnabled,
                      scheduleSecondStartMinute,
                      scheduleSecondEndMinute);

  if (!saveScheduleConfig()) {
    server.send(500, "text/plain", "Updated schedule settings for this boot, but failed to save to LittleFS.");
    return;
  }

  if (timeValid && isTimeValid()) {
    applyTimezoneIfNeeded();
  }

  refreshScheduleRuntimeStateAfterConfigChange(SCHEDULE_ENABLED);

  canAutoStartSessions();
  Serial.println("Saved schedule settings updated.");

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleScheduleToggle() {
  applyScheduleConfig(!SCHEDULE_ENABLED,
                      SCHEDULE_START_MINUTE,
                      SCHEDULE_END_MINUTE,
                      SCHEDULE_SECOND_ENABLED,
                      SCHEDULE_SECOND_START_MINUTE,
                      SCHEDULE_SECOND_END_MINUTE);

  if (!saveScheduleConfig()) {
    server.send(500, "text/plain", "Updated schedule enabled state for this boot, but failed to save to LittleFS.");
    return;
  }

  refreshScheduleRuntimeStateAfterConfigChange(SCHEDULE_ENABLED);
  canAutoStartSessions();
  Serial.println(SCHEDULE_ENABLED ? "Schedule enabled." : "Schedule disabled.");

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleClearWifi() {
  applyWifiCredentials("", "");

  if (!saveWifiConfig()) {
    server.send(500, "text/plain", "Cleared Wi-Fi for this boot, but failed to save to LittleFS.");
    return;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS);

  wifiSetupModeForced = false;
  bootGestureWindowClosed = true;
  networkBootStarted = true;
  stationConnectAttemptActive = false;
  stationWasConnected = false;
  resetTimeState("Saved Wi-Fi cleared");

  Serial.print("Saved Wi-Fi cleared. AP status: ");
  Serial.println(apOk ? "READY" : "FAILED");

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleClearData() {
  if (!littleFsReady) {
    server.send(500, "text/plain", "LittleFS is not available, so saved data cannot be cleared safely.");
    return;
  }

  applyDefaultConfig();

  if (!clearAllSavedConfig()) {
    server.send(500, "text/plain", "Failed to remove one or more saved config files from LittleFS.");
    return;
  }

  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS);

  wifiSetupModeForced = false;
  bootGestureWindowClosed = true;
  networkBootStarted = true;
  stationConnectAttemptActive = false;
  stationWasConnected = false;
  forceSessionRequested = false;
  parkRequested = false;
  resetTimeState("All LittleFS config cleared");
  logScheduleDecisionIfChanged(SCHEDULE_DECISION_DISABLED);

  Serial.print("All saved config cleared. AP status: ");
  Serial.println(apOk ? "READY" : "FAILED");

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleStart() {
  forceSessionRequested = true;
  parkRequested = false;
  nextAutoSessionMs = millis();
  Serial.println("Manual Start Session Now requested.");
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

  lastBootGestureRawMode = readModeSwitch();
  bootGestureStableMode = lastBootGestureRawMode;
  lastBootGestureRawChangeMs = millis();

  littleFsReady = LittleFS.begin();
  if (!littleFsReady) {
    Serial.println("LittleFS mount failed. Continuing with defaults only.");
  } else {
    loadMotionConfig();
    loadWifiConfig();
    loadScheduleConfig();
  }

  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  currentServoDeg = clampAngle(SERVO_REST_DEG);

  wandServo.attach(SERVO_PIN);
  writeServoAngle(currentServoDeg);

  ledState = !LED_FLASH;
  setLed(!LED_FLASH);
  lastLedToggleMs = millis();

  startupEndMs = millis() + STARTUP_WARMUP_MS;
  bootGestureWindowEndMs = millis() + BOOT_GESTURE_WINDOW_MS;
  controllerState = STATE_STARTUP;
  nextAutoSessionMs = startupEndMs;
  settingsChanged = false;
  hasSavedWifiCredentials = configHasSavedWifiCredentials();
  lastScheduleDecisionReason = SCHEDULE_ENABLED ? SCHEDULE_DECISION_TIME_INVALID : SCHEDULE_DECISION_DISABLED;
  scheduleWindowWasOpen = false;
  scheduleStartPending = false;
  lastLoggedStationStatus = WiFi.status();
  lastStationStatusChangeMs = millis();

  WiFi.persistent(false);
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
  Serial.print("Saved Wi-Fi credentials: ");
  Serial.println(hasSavedWifiCredentials ? "present" : "not present");
  Serial.println("Boot gesture window active for 4 seconds: Lazy -> Playful -> Lazy enters setup mode.");

  server.on("/", handleRoot);
  server.on("/set", HTTP_GET, handleSet);
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/schedule", HTTP_POST, handleScheduleSave);
  server.on("/schedule-toggle", HTTP_POST, handleScheduleToggle);
  server.on("/clear-wifi", HTTP_POST, handleClearWifi);
  server.on("/clear-data", HTTP_POST, handleClearData);
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
