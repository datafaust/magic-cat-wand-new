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
  bool scheduleSecondEnabled = (server.arg("scheduleSecondEnabled") == "1");
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
