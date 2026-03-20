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
const int UI_INPUT_BORDER_RADIUS_PX = 14;
const char* UI_PAGE_BACKGROUND_COLOR = "#000000";
const char* UI_PRIMARY_BUTTON_COLOR = "#d4af37";
const char* UI_CARD_BORDER_COLOR = "#d4af37";

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
