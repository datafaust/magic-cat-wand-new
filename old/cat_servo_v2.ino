#include <Servo.h>

/*
 * ===== Pin assignments (D1 Mini) =====
 *
 * SERVO_PIN   -> D5 (GPIO14)  - Servo signal
 * LED_PIN     -> D4 (GPIO2)   - Status LED
 * MODE_A_PIN  -> D6 (GPIO12)  - 3-pos switch pole A (common)
 * MODE_B_PIN  -> D7 (GPIO13)  - 3-pos switch pole B (common)
 */

// ===== MASTER TOGGLES =====
const bool TEST_MODE  = true;   // true = short cycles for code/testing

const int SERVO_PIN  = D5;
const int LED_PIN    = D4;
const int MODE_A_PIN = D6;
const int MODE_B_PIN = D7;

// ==== Servo geometry ====
// NOTE: REST will be auto-centered in setup() unless you disable AUTO_CENTER_REST.
int SERVO_MIN_DEG   = 25;
int SERVO_MAX_DEG   = 155;
int SERVO_REST_DEG  = 100;

const bool AUTO_CENTER_REST = true;   // recommended

// Motion tuning
// Teases use small steps; Darts use bigger steps for faster “zips”.
const int SERVO_STEP_DEG = 2;   // default (teases + general moves)
const int DART_STEP_DEG  = 5;   // faster darts (bigger jumps)

// ===== Startup warmup =====
const unsigned long STARTUP_WARMUP_MS = 5000UL;

Servo wandServo;

enum PlayMode { MODE_LAZY, MODE_PLAYFUL, MODE_ZOOMIES };

bool sessionActive = false;
unsigned long lastSessionEndMs = 0;
int currentServoDeg = 90;

// ===== Auto-cycle =====
enum AutoPhase { AUTO_RUN, AUTO_LONG_REST };

AutoPhase autoPhase = AUTO_RUN;
unsigned long autoPhaseEndMs = 0;
unsigned long nextAutoSessionMs = 0;
unsigned long sessionCounter = 0;

// Macro cycle defaults
unsigned long AUTO_RUN_WINDOW_MS       = 45UL * 60UL * 1000UL;
unsigned long AUTO_LONG_REST_MIN_MS    = 15UL * 60UL * 1000UL;
unsigned long AUTO_LONG_REST_MAX_MS    = 30UL * 60UL * 1000UL;

// In TEST_MODE, shrink everything so you can see behavior quickly
unsigned long TEST_RUN_WINDOW_MS       = 60UL * 1000UL;
unsigned long TEST_LONG_REST_MIN_MS    = 20UL * 1000UL;
unsigned long TEST_LONG_REST_MAX_MS    = 30UL * 1000UL;

// ===== Startup warmup state =====
unsigned long startupEndMs = 0;
bool firstSessionHasRun = false;

// ===== Helpers =====
int clampAngle(int angle) {
  if (angle < SERVO_MIN_DEG) return SERVO_MIN_DEG;
  if (angle > SERVO_MAX_DEG) return SERVO_MAX_DEG;
  return angle;
}

int min2(int a, int b) { return (a < b) ? a : b; }
int max2(int a, int b) { return (a > b) ? a : b; }

int pctOf(int base, int pct) {
  // integer percent to avoid floats
  return (base * pct) / 100;
}

// Compute max amplitude that won’t exceed MIN/MAX given current REST
int maxSafeAmplitude() {
  int leftRoom  = SERVO_REST_DEG - SERVO_MIN_DEG;
  int rightRoom = SERVO_MAX_DEG - SERVO_REST_DEG;
  int m = min2(leftRoom, rightRoom);
  return max2(0, m);
}

// ===== Smooth move (supports variable step size) =====
void moveServoSmooth(int fromDeg, int toDeg, int stepDelayMs, int stepDeg) {
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
      delay(stepDelayMs);
    }
  } else {
    for (int a = fromDeg; a > toDeg; a += step) {
      wandServo.write(clampAngle(a));
      delay(stepDelayMs);
    }
  }

  // Always land exactly on target
  wandServo.write(toDeg);
}

void moveTo(int targetDeg, int stepDelayMs) {
  targetDeg = clampAngle(targetDeg);
  moveServoSmooth(currentServoDeg, targetDeg, stepDelayMs, SERVO_STEP_DEG);
  currentServoDeg = targetDeg;
}

void moveToStep(int targetDeg, int stepDelayMs, int stepDeg) {
  targetDeg = clampAngle(targetDeg);
  moveServoSmooth(currentServoDeg, targetDeg, stepDelayMs, stepDeg);
  currentServoDeg = targetDeg;
}

int randRange(int minVal, int maxVal) {
  return minVal + random(maxVal - minVal + 1);
}

unsigned long randRangeUL(unsigned long minVal, unsigned long maxVal) {
  if (maxVal <= minVal) return minVal;
  unsigned long span = (maxVal - minVal);
  return minVal + (unsigned long)random((long)span + 1L);
}

PlayMode readModeSwitch() {
  int a = digitalRead(MODE_A_PIN);
  int b = digitalRead(MODE_B_PIN);

  if (a == LOW && b == LOW) return MODE_LAZY;
  if (a == HIGH && b == HIGH) return MODE_PLAYFUL;
  return MODE_ZOOMIES;
}

// ===== LED rhythms (different by mode) =====
void blinkLedDuringSession(PlayMode mode) {
  int offMs, onMs;

  if (TEST_MODE) {
    // Still mode-dependent in test mode so you can verify behavior easily
    switch (mode) {
      case MODE_LAZY:    offMs = 180; onMs = 180; break;
      case MODE_PLAYFUL: offMs = 120; onMs = 120; break;
      case MODE_ZOOMIES: offMs = 80;  onMs = 80;  break;
    }
  } else {
    // Normal mode: clearly distinct, but not overly frantic
    switch (mode) {
      case MODE_LAZY:    offMs = 260; onMs = 260; break;
      case MODE_PLAYFUL: offMs = 170; onMs = 170; break;
      case MODE_ZOOMIES: offMs = 110; onMs = 110; break;
    }
  }

  digitalWrite(LED_PIN, LOW);
  delay(offMs);
  digitalWrite(LED_PIN, HIGH);
  delay(onMs);
}

// ===== Startup heartbeat (non-blocking) =====
void startupHeartbeat(unsigned long now) {
  static unsigned long lastToggleMs = 0;
  static bool ledOn = true;

  unsigned long interval = ledOn ? 800UL : 200UL;

  if (now - lastToggleMs >= interval) {
    ledOn = !ledOn;
    digitalWrite(LED_PIN, ledOn ? HIGH : LOW);
    lastToggleMs = now;
  }
}

// ===== Mode timing (time-based sessions + jittered rests) =====
unsigned long sessionDurationMsForMode(PlayMode mode) {
  if (TEST_MODE) return randRangeUL(10000UL, 15000UL);

  switch (mode) {
    case MODE_LAZY:    return randRangeUL(45000UL, 70000UL);
    case MODE_PLAYFUL: return randRangeUL(60000UL, 90000UL);
    case MODE_ZOOMIES: return randRangeUL(75000UL, 120000UL);
  }
  return 60000UL;
}

unsigned long restDurationMsForMode(PlayMode mode) {
  if (TEST_MODE) return randRangeUL(5000UL, 10000UL);

  switch (mode) {
    case MODE_LAZY:    return randRangeUL(6UL * 60UL * 1000UL, 10UL * 60UL * 1000UL);
    case MODE_PLAYFUL: return randRangeUL(3UL * 60UL * 1000UL, 7UL * 60UL * 1000UL);
    case MODE_ZOOMIES: return randRangeUL(2UL * 60UL * 1000UL, 6UL * 60UL * 1000UL);
  }
  return 5UL * 60UL * 1000UL;
}

// ===== Faster speeds safely =====
// smaller delay => faster
int teaseSpeedForMode(PlayMode mode) {
  if (TEST_MODE) return 5;
  switch (mode) {
    case MODE_LAZY:    return 12;
    case MODE_PLAYFUL: return 7;
    case MODE_ZOOMIES: return 5;
  }
  return 7;
}

int dartSpeedForMode(PlayMode mode) {
  // faster cross darts (safe-ish). If you start seeing resets, bump these up by +1 or +2.
  if (TEST_MODE) return 2;
  switch (mode) {
    case MODE_LAZY:    return 6;
    case MODE_PLAYFUL: return 3;
    case MODE_ZOOMIES: return 2;
  }
  return 3;
}

// ===== Range scaling (based on actual safe travel) =====
// Teases: small % of safe range
int teaseAmpForMode(PlayMode mode) {
  int maxAmp = maxSafeAmplitude();

  int loPct, hiPct;
  switch (mode) {
    case MODE_LAZY:    loPct = 5;  hiPct = 15; break;
    case MODE_PLAYFUL: loPct = 8;  hiPct = 22; break;
    case MODE_ZOOMIES: loPct = 10; hiPct = 25; break;
  }

  int lo = max2(4, pctOf(maxAmp, loPct));
  int hi = max2(lo + 2, pctOf(maxAmp, hiPct));
  return randRange(lo, hi);
}

// Darts: big % of safe range
int dartAmpForMode(PlayMode mode) {
  int maxAmp = maxSafeAmplitude();

  int loPct, hiPct;
  switch (mode) {
    case MODE_LAZY:    loPct = 25; hiPct = 55; break;
    case MODE_PLAYFUL: loPct = 40; hiPct = 75; break;
    case MODE_ZOOMIES: loPct = 60; hiPct = 95; break;
  }

  int lo = max2(10, pctOf(maxAmp, loPct));
  int hi = max2(lo + 2, pctOf(maxAmp, hiPct));
  return randRange(lo, hi);
}

// ===== Motion primitives =====
void doTease(PlayMode mode) {
  int amp = teaseAmpForMode(mode);

  int left  = clampAngle(SERVO_REST_DEG - amp);
  int right = clampAngle(SERVO_REST_DEG + amp);

  int speed = teaseSpeedForMode(mode);

  int base = (random(2) == 0) ? left : right;
  int hops = randRange(TEST_MODE ? 1 : 1, TEST_MODE ? 2 : 3);

  for (int i = 0; i < hops; i++) {
    int jitter = randRange(-8, 8);
    moveTo(clampAngle(base + jitter), speed);
    delay(randRange(TEST_MODE ? 20 : 50, TEST_MODE ? 80 : 160));
  }

  delay(randRange(TEST_MODE ? 30 : 60, TEST_MODE ? 120 : 220));
}

void doBigDart(PlayMode mode) {
  int amp = dartAmpForMode(mode);

  int left  = clampAngle(SERVO_REST_DEG - amp);
  int right = clampAngle(SERVO_REST_DEG + amp);

  int speed = dartSpeedForMode(mode);

  bool startLeft = (random(2) == 0);
  int start = startLeft ? left : right;
  int end   = startLeft ? right : left;

  // Faster “zip” moves: use bigger step size for darts
  moveToStep(start, speed, DART_STEP_DEG);
  delay(randRange(TEST_MODE ? 8 : 20, TEST_MODE ? 35 : 75));
  moveToStep(end, speed, DART_STEP_DEG);

  // small overshoot/jitter to feel “alive”
  if (random(3) == 0) {
    moveToStep(clampAngle(end + randRange(-10, 10)), speed, DART_STEP_DEG);
  }

  delay(randRange(TEST_MODE ? 20 : 60, TEST_MODE ? 100 : 220));
}

void hidePause(PlayMode mode) {
  if (TEST_MODE) { delay(randRange(400, 900)); return; }
  switch (mode) {
    case MODE_LAZY:    delay(randRange(2500, 4500)); break;
    case MODE_PLAYFUL: delay(randRange(3000, 6000)); break;
    case MODE_ZOOMIES: delay(randRange(2000, 4500)); break;
  }
}

void shortPause(PlayMode mode) {
  if (TEST_MODE) { delay(randRange(150, 450)); return; }
  switch (mode) {
    case MODE_LAZY:    delay(randRange(900, 2200)); break;
    case MODE_PLAYFUL: delay(randRange(500, 2000)); break;
    case MODE_ZOOMIES: delay(randRange(350, 1200)); break;
  }
}

void runChaosBurst(unsigned long chaosMs) {
  unsigned long endMs = millis() + chaosMs;
  while (millis() < endMs) {
    doBigDart(MODE_ZOOMIES);
    doTease(MODE_ZOOMIES);
  }
}

// ===== Time-based session runner =====
void runTimedSession(PlayMode mode, unsigned long sessionMs, bool allowMidChaos) {
  unsigned long startMs = millis();
  unsigned long endMs   = startMs + sessionMs;

  unsigned long warmupMs = TEST_MODE ? 900UL : (unsigned long)randRange(10000, 15000);
  unsigned long finaleMs = TEST_MODE ? 900UL : (unsigned long)randRange(10000, 20000);

  if (warmupMs + finaleMs + 2000UL > sessionMs) {
    warmupMs = min(warmupMs, sessionMs / 4);
    finaleMs = min(finaleMs, sessionMs / 4);
  }

  moveTo(SERVO_REST_DEG, TEST_MODE ? 6 : 10);
  delay(TEST_MODE ? 120 : 250);

  unsigned long warmupEnd = startMs + warmupMs;
  while (millis() < warmupEnd) {
    doTease(mode);
    if (random(5) == 0) doBigDart(mode);
    shortPause(mode);
  }

  unsigned long huntEnd = (endMs > finaleMs) ? (endMs - finaleMs) : endMs;

  unsigned long lastHideAt = millis();
  unsigned long nextHideAfter = TEST_MODE ? 1200UL : (unsigned long)randRange(15000, 20000);

  bool chaosInserted = false;
  unsigned long chaosAt = startMs + (sessionMs / 2);

  while (millis() < huntEnd) {
    if (allowMidChaos && !chaosInserted && millis() >= chaosAt) {
      runChaosBurst(TEST_MODE ? 1200UL : 10000UL);
      chaosInserted = true;
      shortPause(mode);
    }

    unsigned long burstLen = TEST_MODE ? (unsigned long)randRange(800, 1800)
                                      : (unsigned long)randRange(2500, 7000);
    unsigned long burstEnd = millis() + burstLen;
    if (burstEnd > huntEnd) burstEnd = huntEnd;

    int teasesUntilDart = randRange(2, (mode == MODE_ZOOMIES) ? 4 : 5);

    while (millis() < burstEnd) {
      if (teasesUntilDart > 0) {
        doTease(mode);
        teasesUntilDart--;
      } else {
        doBigDart(mode);
        teasesUntilDart = randRange(2, (mode == MODE_ZOOMIES) ? 4 : 5);
      }
    }

    blinkLedDuringSession(mode);

    unsigned long now = millis();
    if (now - lastHideAt >= nextHideAfter && random(3) == 0) {
      moveTo(SERVO_REST_DEG, TEST_MODE ? 6 : 10);
      hidePause(mode);
      lastHideAt = millis();
      nextHideAfter = TEST_MODE ? 1200UL : (unsigned long)randRange(15000, 20000);
    } else {
      shortPause(mode);
    }
  }

  unsigned long finaleEnd = endMs;
  while (millis() < finaleEnd) {
    doTease(mode);
    doTease(mode);
    doBigDart((mode == MODE_LAZY) ? MODE_PLAYFUL : mode);

    blinkLedDuringSession(mode);

    moveTo(SERVO_REST_DEG, TEST_MODE ? 6 : 9);
    delay(randRange(TEST_MODE ? 60 : 200, TEST_MODE ? 140 : 450));
  }

  moveTo(SERVO_REST_DEG, TEST_MODE ? 6 : 10);
  delay(TEST_MODE ? 250 : randRange(1200, 2500));
}

// ===== Session orchestration =====
void runSessionForMode(PlayMode mode) {
  sessionActive = true;
  digitalWrite(LED_PIN, HIGH);

  unsigned long sessionMs = sessionDurationMsForMode(mode);
  bool allowMidChaos = (((sessionCounter + 1UL) % 3UL) == 0UL);

  runTimedSession(mode, sessionMs, allowMidChaos);

  moveTo(SERVO_REST_DEG, 12);

  sessionActive = false;
  lastSessionEndMs = millis();

  sessionCounter++;
  nextAutoSessionMs = lastSessionEndMs + restDurationMsForMode(mode);
}

// ===== Arduino setup & loop =====
void setup() {
  randomSeed(analogRead(A0));

  pinMode(MODE_A_PIN, INPUT_PULLUP);
  pinMode(MODE_B_PIN, INPUT_PULLUP);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Auto-center REST to maximize usable range
  if (AUTO_CENTER_REST) {
    SERVO_REST_DEG = (SERVO_MIN_DEG + SERVO_MAX_DEG) / 2;
  }

  wandServo.attach(SERVO_PIN);

  currentServoDeg = clampAngle(SERVO_REST_DEG);
  wandServo.write(currentServoDeg);

  // === Deterministic startup behavior ===
  unsigned long now = millis();
  startupEndMs = now + STARTUP_WARMUP_MS;
  firstSessionHasRun = false;

  if (TEST_MODE) {
    AUTO_RUN_WINDOW_MS    = TEST_RUN_WINDOW_MS;
    AUTO_LONG_REST_MIN_MS = TEST_LONG_REST_MIN_MS;
    AUTO_LONG_REST_MAX_MS = TEST_LONG_REST_MAX_MS;
  }

  autoPhase = AUTO_RUN;
  autoPhaseEndMs = now + AUTO_RUN_WINDOW_MS;

  // schedule first session exactly after warmup
  nextAutoSessionMs = startupEndMs;

  delay(200);
}

void loop() {
  unsigned long now = millis();

  // ===== Startup warmup =====
  if (now < startupEndMs) {
    if (currentServoDeg != SERVO_REST_DEG) moveTo(SERVO_REST_DEG, 10);
    startupHeartbeat(now);
    delay(10);
    return;
  } else if (!firstSessionHasRun) {
    digitalWrite(LED_PIN, HIGH);
  }

  // Keep servo parked when idle
  if (!sessionActive && currentServoDeg != SERVO_REST_DEG) {
    moveTo(SERVO_REST_DEG, 15);
  }

  bool shouldStart = false;

  // Macro run/nap cycle
  if (autoPhase == AUTO_RUN && now >= autoPhaseEndMs) {
    autoPhase = AUTO_LONG_REST;
    unsigned long longRest = randRangeUL(AUTO_LONG_REST_MIN_MS, AUTO_LONG_REST_MAX_MS);
    autoPhaseEndMs = now + longRest;
    nextAutoSessionMs = autoPhaseEndMs;
  }

  if (autoPhase == AUTO_LONG_REST && now >= autoPhaseEndMs) {
    autoPhase = AUTO_RUN;
    autoPhaseEndMs = now + AUTO_RUN_WINDOW_MS;

    PlayMode mode = readModeSwitch();
    nextAutoSessionMs = now + restDurationMsForMode(mode);
  }

  if (!sessionActive && autoPhase == AUTO_RUN && now >= nextAutoSessionMs) {
    shouldStart = true;
  }

  if (shouldStart) {
    PlayMode mode = readModeSwitch();
    runSessionForMode(mode);
    firstSessionHasRun = true;
  }

  delay(20);
}
