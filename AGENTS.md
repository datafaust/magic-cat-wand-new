# AGENTS.md

## Purpose

This repository contains firmware and supporting assets for a Wi-Fi-enabled Arduino-based cat servo toy built around a **Wemos/LOLIN D1 mini (ESP8266)**.

The agent working in this repo should act like a careful embedded firmware maintainer. Priorities are:

1. **Functional correctness**
2. **Hardware safety**
3. **Simple, maintainable code**
4. **Low-friction local configuration**
5. **Minimal regressions**

This is not a generic web app repo. Changes here affect real hardware: a servo, a switch, LEDs, power behavior, and future scheduling logic. Favor conservative, testable changes.

---

## Product context

The toy is a servo-driven cat teaser controlled by a **D1 mini / ESP8266**.

Current and near-future capabilities include:

- Servo motion with bounded safe travel
- 3-position hardware mode switch
- External LED with mode-dependent blink patterns
- Local Wi-Fi configuration page served by the ESP8266
- User-editable motion parameters such as:
  - `SERVO_MIN_DEG`
  - `SERVO_MAX_DEG`
- Future roadmap:
  - persistent configuration storage
  - internet-assisted date/time sync
  - scheduled play windows
  - optional user-facing Wi-Fi onboarding

The repo should be maintained with those future features in mind, but current work should remain focused and incremental.

---

## Core engineering principles

### 1) Safety first
This toy drives a real servo and moving teaser wire. Avoid changes that can create:

- hard slams into end stops
- overly aggressive jitter
- blocking motion that prevents recovery
- invalid servo ranges
- boot loops due to misconfigured Wi-Fi or config loading

Never remove servo safeguards unless explicitly instructed.

### 2) Prefer simple over clever
Use straightforward logic. Avoid unnecessary abstraction, advanced metaprogramming, or “framework-like” patterns.

This code should remain understandable to a human maintaining Arduino firmware in the IDE.

### 3) Preserve behavior unless intentionally changing it
For any behavioral change, document:

- what changed
- why it changed
- what remains unchanged

Do not silently alter mode semantics, pin mappings, switch logic, or motion feel.

### 4) Local-first configuration
For configuration UX, prefer:

- local AP mode
- simple web UI
- minimal dependencies
- deterministic behavior even without internet

Do not make internet connectivity mandatory for basic toy operation.

### 5) Persistent settings should be deliberate
The preferred long-term persistence choice is **LittleFS**, not EEPROM, because the configuration surface is expected to grow.

Use a single structured config file rather than scattered ad hoc storage.

---

## Hardware assumptions

Unless explicitly changed, assume the following:

- **Board:** LOLIN/Wemos D1 mini (ESP8266)
- **Servo signal:** `D5`
- **LED pin:** `D4`
- **Mode switch poles/common:** `D6`, `D7`
- **3-position switch behavior:**
  - `LOW + LOW` = mode 1
  - `HIGH + HIGH` = mode 2
  - mixed = mode 3

Treat onboard LED behavior on `D4` as potentially inverted relative to the external LED, depending on board behavior.

Do not casually reassign pins. Any pin change should be accompanied by documentation updates.

---

## Current expected mode model

For the simplified motion model derived from the Raspberry Pi version:

- **Beginner**: narrower movement, slower cadence
- **Intermediate**: full safe movement, moderate cadence
- **Advanced**: full safe movement, fastest cadence

For session-based variants, preserve the declared semantics of:

- lazy
- playful
- zoomies

Do not mix mode schemes in the same file without a clear migration plan.

---

## Servo safety requirements

All servo-related code must respect these rules:

### Required safeguards
- Clamp all target angles to `SERVO_MIN_DEG` / `SERVO_MAX_DEG`
- Keep a valid rest position within the active window
- Prefer smooth stepping over instant snaps
- Re-center rest if the servo window changes and auto-centering is enabled
- Validate user-submitted min/max values before applying them

### Validation rules
At minimum:
- `SERVO_MAX_DEG > SERVO_MIN_DEG`
- enforce a minimum spread between min and max
- clamp user input to a sane global range
- reject obviously dangerous or nonsensical values

### Motion behavior
Prefer motion that is:
- cat-like
- variable
- bounded
- mechanically reasonable

Do not introduce frantic motion just because the servo can move quickly.

---

## Wi-Fi and web UI expectations

Current wireless configuration should remain **local-first**.

### Preferred short-term model
- D1 mini runs as a local AP
- small config page served locally
- user connects directly to toy
- parameters can be edited without reflashing

### Web UI requirements
- Keep HTML/CSS/JS minimal and embedded unless there is a strong reason not to
- Favor simple forms over JavaScript-heavy UI
- Ensure the toy remains usable even if the page is never visited
- Do not make web requests depend on external cloud services

### Responsiveness
Because motion code may block, the firmware should continue to service the web server during:
- smooth servo moves
- idle waits
- loops with delays

Favor cooperative servicing patterns such as:
- `server.handleClient()`
- `yield()`
- short delay intervals rather than long blocking stalls

---

## Persistence strategy

### Preferred long-term storage
Use **LittleFS** for persisted user configuration.

### Why
The repo is expected to grow beyond two numeric settings and likely needs structured configuration such as:
- servo bounds
- scheduling enabled/disabled
- schedule entries
- timezone
- behavior preferences
- maybe Wi-Fi onboarding metadata

### Expected config shape
Store configuration in one structured file, for example:

- `/config.json`

The code should:
1. load config at boot
2. validate it
3. apply defaults when missing or invalid
4. save only on deliberate user actions

### Important
Do not persist on every loop iteration or every minor runtime event.

Write to flash only when needed.

---

## Scheduling and time roadmap

Future support may include:
- real date/time
- scheduled play windows
- internet time sync

### Design expectations
- Basic toy behavior must work without internet
- Scheduling features should degrade gracefully when time is unavailable
- Persist **timezone and schedules**, not “current time” as a source of truth
- When internet time is introduced, use it as authoritative time input
- Keep schedule logic separated from low-level motion primitives

Avoid implementing scheduling in a way that tangles together:
- Wi-Fi onboarding
- clock sync
- motion generation
- web routing
- persistence

Those should remain separable concerns.

---

## Code organization guidance

Favor a repo structure that separates concerns clearly. A good target shape is:

- `firmware/`
  - main sketch / primary firmware entry
  - motion logic
  - config handling
  - web UI handlers
  - storage helpers
  - scheduling helpers
- `docs/`
  - wiring notes
  - behavior notes
  - setup instructions
  - configuration flow
- `test-notes/`
  - manual hardware test scripts/checklists
  - regression notes
- `assets/`
  - diagrams, screenshots, images if needed

If the codebase grows, prefer splitting firmware into multiple `.h/.cpp` files rather than allowing one sketch to become unmanageably large.

Suggested conceptual modules:

- `motion`
- `modes`
- `config`
- `web`
- `storage`
- `schedule`
- `pins`

Do not over-modularize prematurely. Small, purposeful modules are better than many tiny files.

---

## Documentation standards

Every meaningful hardware-affecting change should update documentation.

At minimum, keep current:
- pin mapping
- switch mode logic
- config fields
- persistence behavior
- setup/upload instructions
- any known caveats

When adding settings exposed to users, document:
- default value
- allowed range
- whether it persists across reboot
- whether it requires internet

---

## Change management rules

When making changes, the agent should:

1. Read the existing code before editing
2. Preserve working behavior unless asked to change it
3. Make the smallest reasonable change
4. Explain assumptions clearly in commit/PR notes
5. Avoid combining unrelated refactors with feature work

### Avoid
- opportunistic rewrites
- renaming everything without need
- mixing cleanup, architecture changes, and new features in one patch
- introducing dependencies unless clearly justified

---

## Testing expectations

This repo is hardware-oriented, so automated tests may be limited. Still, every change should include a concrete validation plan.

### Minimum validation for firmware changes
Document how to verify:
- sketch compiles for D1 mini / ESP8266
- servo still centers safely on boot
- switch still selects the expected mode
- LED behavior still matches mode behavior
- web page loads and applies settings
- invalid config input is rejected safely
- persisted config loads correctly after reboot, when persistence exists

### For motion changes
Note:
- expected qualitative behavior
- whether movement range increased or decreased
- whether pause/cadence changed
- whether switch responsiveness changed

### For persistence changes
Test:
- first boot without config
- save config
- reboot
- corrupt or missing config handling
- default fallback behavior

---

## Security and privacy

This is a local embedded device, but basic security hygiene still matters.

### Required practices
- Do not hardcode secrets that should be private in production examples
- Do not commit real home Wi-Fi credentials
- Use placeholder/default credentials for development
- Keep local config interfaces simple and bounded
- Validate all incoming web parameters

### For future internet features
- Do not over-engineer cloud auth yet
- Keep onboarding flows explicit
- Prefer local control unless remote access is a real requirement

---

## Performance constraints

The ESP8266 is limited. Optimize for reliability, not sophistication.

### Prefer
- short strings or `F()`-wrapped literals where helpful
- modest HTML pages
- simple parsing
- clear control flow
- limited dynamic allocation

### Avoid
- large in-memory HTML templates without reason
- excessive use of `String` in hot paths if it becomes unstable
- long blocking delays that freeze networking entirely
- unnecessary libraries

That said, do not micro-optimize prematurely. Clarity comes first unless memory or stability issues appear.

---

## What the agent should do when implementing a feature

For each feature, the agent should think in this order:

1. What hardware or runtime behavior can this break?
2. What is the smallest safe implementation?
3. How does this fit the future roadmap?
4. What config or docs must change too?
5. How will the user verify it works on a D1 mini?

A good answer is usually:
- minimal
- explicit
- safe
- testable

---

## What the agent should not assume

Do not assume:
- persistent internet access
- cloud backend availability
- a desktop-class runtime
- plentiful RAM
- that reflashing is an acceptable way for end users to tune the toy
- that the current sketch is the final architecture

Also do not assume the user wants a fully abstracted “platform.” This is a focused firmware project.

---

## Preferred implementation direction

When expanding this project, bias toward this progression:

1. stable local servo firmware
2. local AP config page
3. persisted config via LittleFS
4. structured config model
5. optional station-mode onboarding and internet time
6. scheduled play
7. only then broader configuration features

This progression should guide tradeoffs.

---

## PR / patch checklist

Before finalizing a change, verify:

- [ ] Compiles for ESP8266 / D1 mini target
- [ ] Pin assignments remain correct
- [ ] Switch logic still matches hardware
- [ ] Servo bounds remain validated and clamped
- [ ] LED behavior still works and matches intended mode/state
- [ ] Web server remains responsive enough during motion
- [ ] New settings have sane defaults
- [ ] Persistence behavior is documented
- [ ] Docs/comments were updated where needed
- [ ] No unrelated refactor was mixed in

---

## Preferred tone for code and docs

Write code and docs that are:

- practical
- concise
- explicit
- readable by a hardware tinkerer using Arduino IDE

Avoid:
- excessive jargon
- over-commenting obvious lines
- vague TODOs without context
- cleverness that hides behavior

---

## Final instruction to the agent

Treat this repository as a real hardware product in progress, not a playground.

Every edit should preserve the toy’s core qualities:

- safe motion
- understandable behavior
- easy local configuration
- clean future path toward persistence and scheduling