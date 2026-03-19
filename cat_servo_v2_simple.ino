#include <Servo.h>

/*
 * ===== Pin assignments (D1 Mini) =====
 *
 * SERVO_PIN   -> D5 (GPIO14)  - Servo signal
 * LED_PIN     -> D4 (GPIO2)   - External LED
 * MODE_A_PIN  -> D6 (GPIO12)  - 3-pos switch pole A (common)
 * MODE_B_PIN  -> D7 (GPIO13)  - 3-pos switch pole B (common)
 *
 * Switch logic (same as your current wiring):
 *   LOW + LOW   = Beginner
 *   HIGH + HIGH = Intermediate
 *   mixed       = Advanced
 */

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

// Smaller = faster motion
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
    case MODE_BEGINNER:     return "BEGINNER";
    case MODE_INTERMEDIATE: return "INTERMEDIATE";
    case MODE_ADVANCED:     return "ADVANCED";
  }
  return "UNKNOWN";
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
  // External LED on D4 to GND: HIGH = on, LOW = off
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

// Map old Pi-style servo value [-1.0 .. +1.0] to safe degrees
// We use tenths to avoid float-heavy random generation:
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
      delay(stepDelayMs);
    }
  } else {
    for (int a = fromDeg; a > toDeg; a += step) {
      wandServo.write(clampAngle(a));
      updateLed(mode, millis());
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

// Wait while keeping LED alive.
// Returns true if the switch mode changed during the wait.
bool delayWithLedAndBreak(unsigned long ms, PlayMode modeAtStart) {
  unsigned long start = millis();

  while (millis() - start < ms) {
    PlayMode currentMode = readModeSwitch();
    updateLed(currentMode, millis());

    if (currentMode != modeAtStart) {
      return true;
    }

    delay(10);
  }

  return false;
}

void runOneMove(PlayMode mode) {
  int servoValue10 = 0;
  unsigned long sleepMs = 0;

  // Mirror your old Raspberry Pi logic:
  // beginner:     value in [-0.6, +0.6], sleep 0..3 sec
  // intermediate: value in [-1.0, +1.0], sleep 0..2 sec
  // advanced:     value in [-1.0, +1.0], sleep 0..1 sec
  switch (mode) {
    case MODE_BEGINNER:
      servoValue10 = random(-6, 7);          // -0.6 .. +0.6
      sleepMs = (unsigned long)random(0, 4) * 1000UL; // 0..3 sec
      break;

    case MODE_INTERMEDIATE:
      servoValue10 = random(-10, 11);        // -1.0 .. +1.0
      sleepMs = (unsigned long)random(0, 3) * 1000UL; // 0..2 sec
      break;

    case MODE_ADVANCED:
      servoValue10 = random(-10, 11);        // -1.0 .. +1.0
      sleepMs = (unsigned long)random(0, 2) * 1000UL; // 0..1 sec
      break;
  }

  int targetDeg = servoValueTenthsToAngle(servoValue10);

  Serial.print("Mode: ");
  Serial.print(modeName(mode));
  Serial.print(" | Servo value: ");
  Serial.print(servoValue10 / 10.0);
  Serial.print(" | Target angle: ");
  Serial.print(targetDeg);
  Serial.print(" | Sleeping for: ");
  Serial.print(sleepMs / 1000.0);
  Serial.println(" sec");

  moveToAngle(targetDeg, stepDelayMsForMode(mode), mode);

  // During the wait, keep blinking LED and allow stage change to interrupt wait
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

  Serial.println("Cat toy started.");
  Serial.println("Modes: LOW+LOW=Beginner, HIGH+HIGH=Intermediate, mixed=Advanced");
}

void loop() {
  PlayMode mode = readModeSwitch();

  // Keep LED blinking continuously for current stage
  updateLed(mode, millis());

  // Run one randomized move, then loop again and re-read mode
  runOneMove(mode);
}
