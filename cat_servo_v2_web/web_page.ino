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
    "body{font-family:Arial,sans-serif;max-width:780px;margin:24px auto;padding:0 16px;line-height:1.45;background:"
  );
  page += UI_PAGE_BACKGROUND_COLOR;
  page += F(";}"
    "button,input,select{font-family:Arial,sans-serif;}"
    ".card{border:1px solid "
  );
  page += UI_CARD_BORDER_COLOR;
  page += F(";border-radius:12px;padding:16px;margin-bottom:16px;background:#111;color:#f5e7a1;}"
    ".hint{background:#111;border-left:4px solid #d4af37;}"
    ".meta{color:#d8d8d8;font-size:14px;}"
    ".actions form{display:inline-block;margin:8px 8px 0 0;}"
    ".schedule-actions{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:16px;}"
    ".schedule-end{display:flex;justify-content:flex-end;margin-top:16px;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
  );
  page += "input[type=number],input[type=time],input[type=text],input[type=password],select{width:100%;padding:10px;font-size:16px;box-sizing:border-box;border-radius:";
  page += String(UI_INPUT_BORDER_RADIUS_PX);
  page += F("px;}");
  page += F(
    "button{margin-top:16px;padding:12px 16px;font-size:16px;border-radius:10px;border:0;cursor:pointer;background:"
  );
  page += UI_PRIMARY_BUTTON_COLOR;
  page += F(";color:#111;}"
    "h1{text-align:center;color:#d4af37;}"
    ".secondary{background:#ececec;color:#111;}"
    ".danger{background:#b64332;color:#fff;}"
    ".status{display:grid;grid-template-columns:1fr;gap:6px;}"
    "details summary{cursor:pointer;font-weight:700;}"
    "details summary::-webkit-details-marker{display:none;}"
    "details[open] summary{margin-bottom:12px;}"
    "</style>"
    "<script>"
    "function setSecondScheduleVisible(show){"
    "var block=document.getElementById('secondScheduleBlock');"
    "var flag=document.getElementById('scheduleSecondEnabled');"
    "var add=document.getElementById('addSecondScheduleButton');"
    "if(!block||!flag||!add)return;"
    "block.style.display=show?'block':'none';"
    "flag.value=show?'1':'0';"
    "add.style.display=show?'none':'inline-block';"
    "}"
    "</script></head><body>"
    "<h1>Cat Toy Control</h1>"
  );

  page += F("<div class='card'><details><summary>Cat Toy Control Information</summary><div class='status'>");
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
  page += F("</div><div><strong>Daily schedule:</strong> ");
  page += scheduleWindowLabel(SCHEDULE_START_MINUTE, SCHEDULE_END_MINUTE);
  page += F("</div><div><strong>Second schedule:</strong> ");
  if (SCHEDULE_SECOND_ENABLED) {
    page += scheduleWindowLabel(SCHEDULE_SECOND_START_MINUTE, SCHEDULE_SECOND_END_MINUTE);
  } else {
    page += F("Disabled");
  }
  page += F("</div></details></div>");
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

  page += F("<div class='card actions'><strong>Manual control</strong>"
            "<p class='meta'>Start now forces the next session immediately, even outside the saved schedule. Park ends the current session as soon as the sketch reaches an interrupt check and returns the servo to rest.</p>"
            "<form action='/start' method='post'><button type='submit'>Start Session Now</button></form>"
            "<form action='/park' method='post'><button type='submit'>Park At Rest</button></form></div>");

  page += F("<div class='card'><details><summary>Servo Configuration</summary><p class='meta'>Servo limits must keep max &gt; min with at least 20 degrees of spread. Rest remains auto-centered in the safe window.</p><form action='/set' method='get'>");
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

  page += F("<button type='submit'>Save Motion Settings</button>");
  page += F("</form></details></div>");

  page += F("<div class='card'><strong>Wi-Fi</strong>");
  page += F("<p class='meta'>Use the boot gesture Lazy -> Playful -> Lazy during the first 4 seconds after boot to keep the toy in AP-only Wi-Fi setup mode for that boot.</p>");
  page += F("<form id='wifiForm' action='/wifi' method='post'>");
  page += F("<label for='wifiSsid'>Home Wi-Fi SSID</label>");
  page += F("<input id='wifiSsid' name='wifiSsid' type='text' maxlength='32' value='");
  page += htmlEscape(WIFI_SSID);
  page += F("'>");

  page += F("<label for='wifiPass'>Home Wi-Fi password</label>");
  page += F("<input id='wifiPass' name='wifiPass' type='password' maxlength='63' value='");
  page += htmlEscape(WIFI_PASS);
  page += F("'>");
  page += F("</form>");
  page += F("<div class='schedule-actions'><button type='submit' form='wifiForm'>Save Wi-Fi</button>");
  page += F("<form action='/clear-wifi' method='post'><button class='danger' type='submit'>Clear Saved Wi-Fi</button></form></div></div>");

  page += F("<div class='card'><strong>Schedule</strong>");
  page += F("<p class='meta'>Schedule is currently ");
  page += (SCHEDULE_ENABLED ? F("enabled") : F("disabled"));
  page += F(". Save Schedule updates the timezone and daily windows without changing that state.</p>");
  page += F("<p class='meta'>This toy supports a maximum of two saved daily schedules.</p>");
  page += F("<p class='meta'>Each window treats matching start and end times as all day. Overnight windows are supported for both windows.</p>");
  page += F("<form id='scheduleForm' action='/schedule' method='post'>");
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

  page += F("<label for='scheduleStart'>Daily start</label>");
  page += F("<input id='scheduleStart' name='scheduleStart' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_START_MINUTE);
  page += F("'>");

  page += F("<label for='scheduleEnd'>Daily end</label>");
  page += F("<input id='scheduleEnd' name='scheduleEnd' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_END_MINUTE);
  page += F("'>");

  page += F("<input id='scheduleSecondEnabled' name='scheduleSecondEnabled' type='hidden' value='");
  page += (SCHEDULE_SECOND_ENABLED ? F("1") : F("0"));
  page += F("'>");

  page += F("<div class='schedule-end'><button class='secondary' id='addSecondScheduleButton' type='button' onclick='setSecondScheduleVisible(true)'");
  if (SCHEDULE_SECOND_ENABLED) page += F(" style='display:none'");
  page += F(">&#43; &#128197;</button></div>");

  page += F("<div id='secondScheduleBlock'");
  if (!SCHEDULE_SECOND_ENABLED) page += F(" style='display:none'");
  page += F(">");
  page += F("<div class='schedule-actions'><strong>Second schedule</strong><div style='margin-left:auto'><button class='secondary' type='button' onclick='setSecondScheduleVisible(false)'>&#8722; &#128197;</button></div></div>");
  page += F("<label for='scheduleSecondStart'>Daily start</label>");
  page += F("<input id='scheduleSecondStart' name='scheduleSecondStart' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_SECOND_START_MINUTE);
  page += F("'>");

  page += F("<label for='scheduleSecondEnd'>Daily end</label>");
  page += F("<input id='scheduleSecondEnd' name='scheduleSecondEnd' type='time' value='");
  page += formatMinuteOfDay(SCHEDULE_SECOND_END_MINUTE);
  page += F("'>");
  page += F("</div>");

  page += F("</form>");
  page += F("<div class='schedule-actions'><button type='submit' form='scheduleForm'>Save Schedule</button>");
  page += F("<form action='/schedule-toggle' method='post'>");
  page += F("<button type='submit'>");
  page += (SCHEDULE_ENABLED ? F("Disable Schedule") : F("Enable Schedule"));
  page += F("</button></form></div>");
  page += F("</div>");

  page += F("<div class='card'><strong>Maintenance</strong>");
  page += F("<p class='meta'>This deletes all saved motion, Wi-Fi, and schedule data from LittleFS, including the legacy config file, and resets this boot to default settings.</p>");
  page += F("<form action='/clear-data' method='post' onsubmit=\"return confirm('Clear all saved settings and return to defaults?');\">");
  page += F("<button class='danger' type='submit'>Clear All Saved Data</button>");
  page += F("</form></div>");

  page += F("<div class='card meta'>Connect to Wi-Fi <strong>");
  page += AP_SSID;
  page += F("</strong> and browse to <strong>http://192.168.4.1</strong>. The local AP stays available even when station Wi-Fi is configured.</div>");

  page += F("</body></html>");
  return page;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}
