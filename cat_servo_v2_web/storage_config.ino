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

bool clearAllSavedConfig() {
  bool ok = true;
  ok = deleteFileIfExists(MOTION_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(WIFI_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(SCHEDULE_CONFIG_PATH) && ok;
  ok = deleteFileIfExists(LEGACY_CONFIG_PATH) && ok;
  return ok;
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
