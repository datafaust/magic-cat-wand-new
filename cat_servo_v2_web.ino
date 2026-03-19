#include <Servo.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

/*
 * ===== Pin assignments (D1 Mini) =====
 *
 * SERVO_PIN   -> D5 (GPIO14)  - Servo signal
 * LED_PIN     -> D4 (GPIO2)   - External LED
 * MODE_A_PIN  -> D6 (GPIO12)  - 3-pos switch pole A (common)
 * MODE_B_PIN  -> D7 (GPIO13)  - 3-pos switch pole B (common)
 *
 * Switch logic:
 *   LOW + LOW   = Beginner
 *   HIGH + HIGH = Intermediate
 *   mixed       = Advanced
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

// ===== Servo safeguards =====
int SERVO_MIN_DEG  = 25;
int SERVO_MAX_DEG  = 155;
int SERVO_REST_DEG = 90;

const bool AUTO_CENTER_REST = true;
const int SERVO_STEP_DEG    = 2;

// Smaller delay = faster motion
const int BEGINNER_STEP_DELAY_MS     = 10;
const int INTERMEDIATE_STEP_DELAY_MS = 8;
const int ADVANCED_STEP_DELAY_MS     = 6;

Servo wandServo;

enum PlayMode {
  MODE_BEGINNER,
  MODE_INTERMEDIATE,
  MODE_ADVANCED
};

int currentServoDeg = 90;

// ===== LED blink state =====
bool ledState = false;
unsigned long lastLedToggleMs = 0;

// ===== Runtime flags =====
bool settingsChanged = false;

// ===== Helpers =====
int clampAngle(int angle) {
  if (angle < SERVO_MIN_DEG) return SERVO_MIN_DEG;
  if (angle > SERVO_MAX_DEG) return SERVO_MAX_DEG;
  return angle;
}

int min2(int a, int b) { return (a < b) ? a : b; }
int max2(int a, int b) { return (a > b) ? a : b; }

int maxSafeAmplitude() {
  int leftRoom  = SERVO_REST_DEG - SERVO_MIN_DEG;
  int rightRoom = SERVO_MAX_DEG - SERVO_REST_DEG;
  return min2(leftRoom, rightRoom);
}

PlayMode readModeSwitch() {
  int a = digitalRead(MODE_A_PIN);
  int b = digitalRead(MODE_B_PIN);

  if (a == LOW && b == LOW)   return MODE_BEGINNER;
  if (a == HIGH && b == HIGH) return MODE_INTERMEDIATE;
  return MODE_ADVANCED;
}

const char* modeName(PlayMode mode) {
  switch (mode) {
    case MODE_BEGINNER:     return "Beginner";
    case MODE_INTERMEDIATE: return "Intermediate";
    case MODE_ADVANCED:     return "Advanced";
  }
  return "Unknown";
}

int ledToggleIntervalMs(PlayMode mode) {
  // Reasonable differences, not too frantic
  switch (mode) {
    case MODE_BEGINNER:     return 420; // slowest
    case MODE_INTERMEDIATE: return 240; // medium
    case MODE_ADVANCED:     return 140; // fastest
  }
  return 240;
}

int stepDelayMsForMode(PlayMode mode) {
  switch (mode) {
    case MODE_BEGINNER:     return BEGINNER_STEP_DELAY_MS;
    case MODE_INTERMEDIATE: return INTERMEDIATE_STEP_DELAY_MS;
    case MODE_ADVANCED:     return ADVANCED_STEP_DELAY_MS;
  }
  return INTERMEDIATE_STEP_DELAY_MS;
}

void setLed(bool on) {
  // External LED from D4 -> resistor -> GND
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

// Map Pi-style servo value [-1.0 .. +1.0] to safe degrees.
// Use tenths to avoid float-heavy randomness:
//   -10 = -1.0
//    0  =  0.0
//   10  = +1.0
int servoValueTenthsToAngle(int value10) {
  value10 = constrain(value10, -10, 10);

  int amp = maxSafeAmplitude();
  int target = SERVO_REST_DEG + (value10 * amp) / 10;
  return clampAngle(target);
}

void moveServoSmooth(int fromDeg, int toDeg, int stepDelayMs, int stepDeg, PlayMode mode) {
  fromDeg = clampAngle(fromDeg);
  toDeg   = clampAngle(toDeg);

  if (fromDeg == toDeg) {
    wandServo.write(toDeg);
    return;
  }

  stepDeg = max2(1, stepDeg);
  int step = (toDeg > fromDeg) ? stepDeg : -stepDeg;

  if (step > 0) {
    for (int a = fromDeg; a < toDeg; a += step) {
      wandServo.write(clampAngle(a));
      updateLed(mode, millis());
      serviceNetwork();
      delay(stepDelayMs);
    }
  } else {
    for (int a = fromDeg; a > toDeg; a += step) {
      wandServo.write(clampAngle(a));
      updateLed(mode, millis());
      serviceNetwork();
      delay(stepDelayMs);
    }
  }

  wandServo.write(toDeg);
}

void moveToAngle(int targetDeg, int stepDelayMs, PlayMode mode) {
  targetDeg = clampAngle(targetDeg);
  moveServoSmooth(currentServoDeg, targetDeg, stepDelayMs, SERVO_STEP_DEG, mode);
  currentServoDeg = targetDeg;
}

// Wait while keeping LED + web server alive.
// Returns early if the switch mode changes or settings were updated.
bool delayWithLedAndBreak(unsigned long ms, PlayMode modeAtStart) {
  unsigned long start = millis();

  while (millis() - start < ms) {
    PlayMode currentMode = readModeSwitch();
    updateLed(currentMode, millis());
    serviceNetwork();

    if (currentMode != modeAtStart) return true;

    if (settingsChanged) {
      settingsChanged = false;
      return true;
    }

    delay(10);
  }

  return false;
}

void applyServoWindow(int newMin, int newMax) {
  SERVO_MIN_DEG = newMin;
  SERVO_MAX_DEG = newMax;

  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  currentServoDeg = clampAngle(currentServoDeg);
  wandServo.write(currentServoDeg);
  settingsChanged = true;
}

// ===== Web UI =====
String htmlPage() {
  PlayMode mode = readModeSwitch();

  String page;
  page.reserve(2200);

  page += F(
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Cat Toy Control</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:680px;margin:24px auto;padding:0 16px;line-height:1.45;}"
    ".card{border:1px solid #ddd;border-radius:12px;padding:16px;margin-bottom:16px;}"
    "label{display:block;margin-top:12px;font-weight:600;}"
    "input[type=number]{width:100%;padding:10px;font-size:16px;box-sizing:border-box;}"
    "button{margin-top:16px;padding:12px 16px;font-size:16px;border-radius:10px;border:0;cursor:pointer;}"
    ".meta{color:#444;font-size:14px;}"
    ".ok{color:#0a7a2f;font-weight:600;}"
    "</style></head><body>"
    "<h1>Cat Toy Control</h1>"
    "<div class='card'>"
  );

  page += F("<div><strong>Current mode:</strong> ");
  page += modeName(mode);
  page += F("</div><div><strong>Current servo window:</strong> ");
  page += String(SERVO_MIN_DEG);
  page += F("&deg; to ");
  page += String(SERVO_MAX_DEG);
  page += F("&deg;</div><div><strong>Current rest:</strong> ");
  page += String(SERVO_REST_DEG);
  page += F("&deg;</div></div>");

  page += F("<div class='card'><form action='/set' method='get'>");
  page += F("<label for='min'>SERVO_MIN_DEG</label>");
  page += F("<input id='min' name='min' type='number' min='0' max='170' value='");
  page += String(SERVO_MIN_DEG);
  page += F("'>");

  page += F("<label for='max'>SERVO_MAX_DEG</label>");
  page += F("<input id='max' name='max' type='number' min='10' max='180' value='");
  page += String(SERVO_MAX_DEG);
  page += F("'>");

  page += F("<button type='submit'>Apply</button>");
  page += F("</form>");
  page += F("<p class='meta'>For safety this sketch requires max &gt; min and at least 20 degrees of spread. "
            "When you apply a new window, REST is re-centered automatically.</p>");
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
  if (!server.hasArg("min") || !server.hasArg("max")) {
    server.send(400, "text/plain", "Missing min or max parameter.");
    return;
  }

  int newMin = server.arg("min").toInt();
  int newMax = server.arg("max").toInt();

  // Safety checks
  if (newMin < 0 || newMin > 170) {
    server.send(400, "text/plain", "SERVO_MIN_DEG must be between 0 and 170.");
    return;
  }

  if (newMax < 10 || newMax > 180) {
    server.send(400, "text/plain", "SERVO_MAX_DEG must be between 10 and 180.");
    return;
  }

  if (newMax <= newMin) {
    server.send(400, "text/plain", "SERVO_MAX_DEG must be greater than SERVO_MIN_DEG.");
    return;
  }

  if ((newMax - newMin) < 20) {
    server.send(400, "text/plain", "Keep at least 20 degrees between min and max.");
    return;
  }

  applyServoWindow(newMin, newMax);

  Serial.print("Updated servo window: ");
  Serial.print(SERVO_MIN_DEG);
  Serial.print(" to ");
  Serial.println(SERVO_MAX_DEG);

  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// ===== Motion logic =====
void runOneMove(PlayMode mode) {
  int servoValue10 = 0;
  unsigned long sleepMs = 0;

  // Mirrors your earlier Pi behavior:
  // beginner:     value in [-0.6, +0.6], sleep 0..3 sec
  // intermediate: value in [-1.0, +1.0], sleep 0..2 sec
  // advanced:     value in [-1.0, +1.0], sleep 0..1 sec
  switch (mode) {
    case MODE_BEGINNER:
      servoValue10 = random(-6, 7);                 // -0.6 .. +0.6
      sleepMs = (unsigned long)random(0, 4) * 1000UL;
      break;

    case MODE_INTERMEDIATE:
      servoValue10 = random(-10, 11);               // -1.0 .. +1.0
      sleepMs = (unsigned long)random(0, 3) * 1000UL;
      break;

    case MODE_ADVANCED:
      servoValue10 = random(-10, 11);               // -1.0 .. +1.0
      sleepMs = (unsigned long)random(0, 2) * 1000UL;
      break;
  }

  int targetDeg = servoValueTenthsToAngle(servoValue10);

  Serial.print("Mode: ");
  Serial.print(modeName(mode));
  Serial.print(" | Target angle: ");
  Serial.print(targetDeg);
  Serial.print(" | Window: ");
  Serial.print(SERVO_MIN_DEG);
  Serial.print("-");
  Serial.print(SERVO_MAX_DEG);
  Serial.print(" | Sleep: ");
  Serial.print(sleepMs);
  Serial.println(" ms");

  moveToAngle(targetDeg, stepDelayMsForMode(mode), mode);
  delayWithLedAndBreak(sleepMs, mode);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A0));

  pinMode(MODE_A_PIN, INPUT_PULLUP);
  pinMode(MODE_B_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  currentServoDeg = clampAngle(SERVO_REST_DEG);

  wandServo.attach(SERVO_PIN);
  wandServo.write(currentServoDeg);

  ledState = false;
  setLed(false);
  lastLedToggleMs = millis();

  // Start local AP + web server
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
  server.on("/set", handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  PlayMode mode = readModeSwitch();

  updateLed(mode, millis());
  serviceNetwork();

  runOneMove(mode);
}