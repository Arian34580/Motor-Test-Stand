/*
  thrust_stand_web.ino  -  ESP32 thrust stand with a phone control panel
  =============================================================================
  The ESP32 makes its own Wi-Fi network. Join it from your phone, open
  http://192.168.4.1 in the browser, and you get a control panel with:
    - live thrust, throttle, RPM, voltage, current, power and ESC temperature
    - an auto test that steps the throttle up and records everything to CSV files
    - manual throttle
    - an emergency stop
  Recorded runs are stored on the ESP32 and downloaded from the same page.

  Two files, kept together in a folder named thrust_stand_web:
    thrust_stand_web.ino   this file
    page.h                 the control panel page

  Wi-Fi network "ThrustStand", password "thrust1234" (change them below).

  Uses the calibration saved by load_cell_calibrate.ino.
  Needs the esp32 board package 3.x. No extra libraries.

  Wiring (same as before)
    HX711:  VCC -> 3V3   GND -> GND   DT -> D26   SCK -> D27
    ESC:    signal -> D25   signal ground -> GND
            telemetry -> D32, plus a 4.7 kOhm resistor from D32 to 3V3
    Stop button (optional, normally open): between D33 and GND

  Arduino IDE, Tools menu:
    Board:             ESP32 Dev Module
    Partition Scheme:  Default 4MB with spiffs   (the recorded files live there)
    Erase All Flash Before Sketch Upload: Disabled   (keeps the calibration and runs)

  The motor stops when you press EMERGENCY STOP or the stop button, when the
  control panel stops talking to the ESP32 for LINK_TIMEOUT_MS, when current,
  ESC temperature or thrust go over the limits below, and when the load cell
  stops responding during a test. Keep a hand near the battery disconnect anyway.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <stdarg.h>
#include "page.h"                         // the control panel page (INDEX_HTML)

// ============================================================================ settings

// Wi-Fi network the ESP32 creates
const char *WIFI_NAME     = "ThrustStand";
const char *WIFI_PASSWORD = "";        // leave blank for no password

// Auto test: steps from 0 % up to MAX_THROTTLE_PCT
constexpr float STEP_PCT         = 5.0;    // throttle increase per step (%)
constexpr float STEP_HOLD_S      = 3.0;    // seconds held at each step
constexpr float SETTLE_S         = 1.0;    // seconds ignored at the start of each step when averaging
constexpr float MAX_THROTTLE_PCT = 50.0;   // highest throttle allowed, manual and auto (%). 100 = full range
constexpr float PREP_S           = 3.0;    // motor off before the test's tare (prop stops, ESC arms)
constexpr float RAMP_DOWN_S      = 2.0;    // ramp from the last step back down to 0 %
constexpr float ZERO_CHECK_S     = 3.0;    // motor off at the end, to measure zero drift

// Safety limits
constexpr uint32_t LINK_TIMEOUT_MS = 3000; // motor stops if the control panel goes quiet this long (0 = off)
constexpr float MAX_CURRENT_A      = 95.0; // motor stops above this current (0 = off)
constexpr int   MAX_ESC_TEMP_C     = 90;   // motor stops above this ESC temperature (0 = off)
constexpr float MAX_THRUST_G       = 0;    // motor stops above this thrust, to protect the load cell (0 = off)

// Measurement
constexpr int  MOTOR_POLES   = 14;         // magnets in the motor (for RPM)
constexpr bool INVERT_THRUST = false;      // set true if thrust reads negative
constexpr int  TARE_SAMPLES  = 16;         // readings averaged for a tare (1.6 s)

// Pins
constexpr int ESC_PIN         = 25;
constexpr int HX711_DOUT      = 26;
constexpr int HX711_SCK       = 27;
constexpr int TELEM_RX_PIN    = 32;
constexpr int TELEM_TX_PIN    = 23;        // not used, leave unconnected
constexpr int STOP_BUTTON_PIN = 33;        // -1 if no button is fitted

static_assert(STEP_PCT >= 0.5f && STEP_PCT <= 100.0f, "STEP_PCT must be between 0.5 and 100");
static_assert(MAX_THROTTLE_PCT > 0.0f && MAX_THROTTLE_PCT <= 100.0f, "MAX_THROTTLE_PCT must be 1-100");
static_assert(STEP_HOLD_S >= 1.0f, "STEP_HOLD_S must be at least 1 second");

// DShot300 timing in RMT ticks (40 MHz = 25 ns per tick)
constexpr uint32_t RMT_HZ    = 40000000;
constexpr uint16_t BIT_TICKS = 133;        // 3.33 us per bit
constexpr uint16_t T1H_TICKS = 100;        // "1" bit: high for 2.50 us
constexpr uint16_t T0H_TICKS = 50;         // "0" bit: high for 1.25 us
constexpr uint32_t TELEM_PERIOD_MS  = 50;  // ask the ESC for telemetry 20 times a second
constexpr uint32_t TELEM_TIMEOUT_MS = 10;  // a reply takes about 1 ms

constexpr int MAX_STEPS     = 202;
constexpr int ROW_QUEUE_LEN = 300;         // 30 s of readings waiting to be written
const char *FW_VERSION = "thrust_stand_web 1.0";

// ============================================================================ types

enum Mode : uint8_t { MODE_IDLE, MODE_MANUAL, MODE_TEST };
enum Phase : uint8_t { PH_NONE, PH_PREP, PH_TARE, PH_STEP, PH_RAMP, PH_ZERO };

// Published by the control task for the web pages (guarded by stateMux)
struct Live {
  float thrust, pct, rpm, volts, amps;
  int   tempC;
  bool  loadOk, tared, telemOk, estop, buttonDown;
  uint8_t mode, phase;
  int   step, steps;
  float phaseLeftS, testLeftS;
  uint32_t testId, endedId, msgId;
  char  runName[40], endReason[80], msg[120];
};

// Requests from the web pages to the control task (guarded by stateMux)
struct Commands {
  bool  estop, resetEstop, start, tare, manual, abort, note;
  float manualPct;
  char  runName[40], abortText[80], noteText[120];
};

// One recorded reading
struct Row {
  uint32_t ms;
  float pct, thrust, rpm, volts, amps;
  int16_t temp;
  uint8_t step;
  bool telem;
};

// Running totals for one auto test step
struct StepResult {
  float  pct, startS, durS;
  int    n, nT, maxTemp;
  double sum, sumSq, rpm, volts, amps, power;
};

// Everything the control task owns
struct Control {
  Mode  mode = MODE_IDLE;
  Phase phase = PH_NONE;
  float pct = 0;                       // throttle being sent (%)
  bool  estop = false;
  // stop button
  bool  btnDown = false, btnWas = false, btnTripped = false;
  uint32_t btnSince = 0;
  // load cell
  bool  loadOk = false, everOk = false, tared = false, bootTareStarted = false, warnedNoLoad = false;
  long  hist[3] = {0, 0, 0};
  int   histIdx = 0, nRead = 0;
  long  offset = 0;
  float thrust = NAN;
  uint32_t bootMs = 0, lastSampleMs = 0;
  int   tareLeft = 0, tareN = 0;
  int64_t tareSum = 0;
  // ESC telemetry
  uint8_t tbuf[10];
  int   tlen = 0;
  bool  waiting = false;
  uint32_t askedMs = 0, lastAskMs = 0, lastGoodMs = 0;
  float rpm = NAN, volts = NAN, amps = NAN;
  int   temp = 0, overI = 0, overT = 0;
  // auto test
  float steps[MAX_STEPS];
  int   nSteps = 0, stepIdx = 0;
  uint32_t phaseStartMs = 0, t0Ms = 0;
  float rampFrom = 0;
  double zSum = 0;
  int   zN = 0;
  uint32_t testId = 0, endedId = 0;
  char  runName[40] = "", endReason[80] = "";
  // status line shown on the page
  char  msg[120] = "Starting...";
  uint32_t msgId = 1;
};

// ============================================================================ globals

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE hxMux = portMUX_INITIALIZER_UNLOCKED;
Live live;
Commands cmd;
Control ctl;                               // control task only
volatile uint32_t lastLinkMs = 0;          // last request from the control panel
volatile uint32_t rowsDropped = 0;
QueueHandle_t rowQueue = nullptr;          // readings: control task -> recorder
StepResult results[MAX_STEPS];             // written during a test, read once it has ended
volatile int nResults = 0;
volatile float zeroAfter = NAN;
float calFactor = 1.0f;
bool calibrated = false;
rmt_data_t frame[16];

// Web server and recorder (loop task only)
WebServer server(80);
Preferences prefs;
bool fsOk = false;
File rawFile;
bool recOpen = false, recFailed = false;
uint32_t recId = 0, recRows = 0, savedCount = 0, printedMsgId = 0;
char recName[40] = "", lastRun[40] = "", lastResult[100] = "";


// ============================================================================ helpers

void setMsg(const char *fmt, ...) {        // control task only
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ctl.msg, sizeof ctl.msg, fmt, ap);
  va_end(ap);
  ctl.msgId++;
}

uint16_t dshotValue(float pct) {
  if (pct <= 0) return 0;                  // 0 = motor stop
  if (pct > MAX_THROTTLE_PCT) pct = MAX_THROTTLE_PCT;
  return 48 + (uint16_t)lroundf(pct * 19.99f);   // 48..2047
}

// The auto test's throttle steps: 0, STEP_PCT, 2 x STEP_PCT ... MAX_THROTTLE_PCT
int buildSteps(float *out) {
  int n = 0;
  float last = 0;
  for (int i = 0; n < MAX_STEPS; i++) {
    float p = roundf(i * STEP_PCT * 10.0f) / 10.0f;
    if (p > MAX_THROTTLE_PCT + 0.01f) break;
    if (out) out[n] = p;
    last = p;
    n++;
  }
  if (last < MAX_THROTTLE_PCT - 0.01f && n < MAX_STEPS) {
    if (out) out[n] = MAX_THROTTLE_PCT;
    n++;
  }
  return n;
}

float testSeconds(int nSteps) {
  return PREP_S + TARE_SAMPLES * 0.1f + nSteps * STEP_HOLD_S + RAMP_DOWN_S + ZERO_CHECK_S;
}

float settleS() {                          // never more than half a step
  return SETTLE_S < STEP_HOLD_S * 0.5f ? SETTLE_S : STEP_HOLD_S * 0.5f;
}

long median3(long a, long b, long c) {
  if (a > b) { long t = a; a = b; b = t; }
  if (b > c) b = c;
  return a > b ? a : b;
}

// ============================================================================ load cell (HX711)

bool hxReady() {
  return digitalRead(HX711_DOUT) == LOW;
}

long hxRead() {                            // call only when hxReady()
  long v = 0;
  // Interrupts off for the ~60 us read: if SCK stayed high for over 60 us the
  // HX711 would power down and the reading would be garbage.
  portENTER_CRITICAL(&hxMux);
  for (int i = 0; i < 24; i++) {
    digitalWrite(HX711_SCK, HIGH);
    delayMicroseconds(1);
    digitalWrite(HX711_SCK, LOW);
    delayMicroseconds(1);
    v = (v << 1) | digitalRead(HX711_DOUT);
  }
  digitalWrite(HX711_SCK, HIGH);           // 25th pulse: channel A, gain 128 next time
  delayMicroseconds(1);
  digitalWrite(HX711_SCK, LOW);
  portEXIT_CRITICAL(&hxMux);
  if (v & 0x800000) v -= 0x1000000;        // 24-bit two's complement
  return v;
}

void beginTare() {
  ctl.tareLeft = TARE_SAMPLES;
  ctl.tareN = 0;
  ctl.tareSum = 0;
}

// ============================================================================ ESC (DShot + telemetry)

void sendDshot(uint16_t value, bool askTelemetry) {
  uint16_t v = (value << 1) | (askTelemetry ? 1 : 0);
  uint16_t crc = (v ^ (v >> 4) ^ (v >> 8)) & 0x0F;
  uint16_t packet = (v << 4) | crc;
  for (int i = 0; i < 16; i++) {
    uint16_t high = (packet & (0x8000 >> i)) ? T1H_TICKS : T0H_TICKS;
    frame[i].level0 = 1;
    frame[i].duration0 = high;
    frame[i].level1 = 0;
    frame[i].duration1 = BIT_TICKS - high;
  }
  rmtWrite(ESC_PIN, frame, 16, RMT_WAIT_FOR_EVER);   // ~53 us
}

uint8_t kissCrc8(const uint8_t *d, int n) {          // KISS telemetry CRC, poly 0x07
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
  }
  return crc;
}

bool telemFresh(uint32_t now) {
  return ctl.lastGoodMs != 0 && now - ctl.lastGoodMs < 500;
}

// ============================================================================ control task

void endTest(const char *reason) {         // reason "" = completed normally
  strlcpy(ctl.endReason, reason, sizeof ctl.endReason);
  char *dash = strstr(ctl.endReason, " - ");  // keep the short part, e.g. "EMERGENCY STOP"
  if (dash) *dash = 0;
  ctl.endedId = ctl.testId;
}

// Motor off now. latch = true for an emergency stop: stays off until Reset.
void stopAll(const char *why, bool latch) {
  ctl.pct = 0;
  if (latch) ctl.estop = true;
  if (ctl.mode == MODE_TEST) endTest(why);  // the data recorded so far is kept
  ctl.mode = MODE_IDLE;
  ctl.phase = PH_NONE;
  setMsg("%s", why);
}

void startTest(const char *name, uint32_t now) {
  ctl.nSteps = buildSteps(ctl.steps);
  nResults = 0;
  rowsDropped = 0;
  zeroAfter = NAN;
  ctl.zSum = 0;
  ctl.zN = 0;
  ctl.testId++;
  strlcpy(ctl.runName, name, sizeof ctl.runName);
  ctl.endReason[0] = 0;
  ctl.mode = MODE_TEST;
  ctl.phase = PH_PREP;
  ctl.phaseStartMs = now;
  ctl.pct = 0;
  setMsg("Auto test: motor off for %g s, then tare.", PREP_S);
}

void startStep(uint32_t now) {
  ctl.phase = PH_STEP;
  ctl.phaseStartMs = now;
  ctl.pct = ctl.steps[ctl.stepIdx];
  StepResult &r = results[ctl.stepIdx];
  r = StepResult();
  r.pct = ctl.pct;
  r.startS = (now - ctl.t0Ms) / 1000.0f;
  r.maxTemp = -128;
  setMsg("Auto test: step %d of %d, %g %% throttle", ctl.stepIdx + 1, ctl.nSteps, ctl.pct);
}

void finishStep(uint32_t now) {
  results[ctl.stepIdx].durS = (now - ctl.phaseStartMs) / 1000.0f;
  nResults = ctl.stepIdx + 1;
}

void takeCommands(uint32_t now) {
  Commands k;
  portENTER_CRITICAL(&stateMux);
  k = cmd;
  memset(&cmd, 0, sizeof cmd);
  portEXIT_CRITICAL(&stateMux);

  if (k.estop) stopAll("EMERGENCY STOP - motor off. Tap Reset when it is safe.", true);
  if (k.abort) stopAll(k.abortText, false);
  if (k.note) setMsg("%s", k.noteText);
  if (k.resetEstop) {
    if (ctl.btnDown) setMsg("Release the stop button first.");
    else if (ctl.estop) {
      ctl.estop = false;
      setMsg("E-stop reset. Motor is off.");
    }
  }
  if (k.start) {
    if (ctl.estop) setMsg("E-stop is on - tap Reset first.");
    else if (ctl.mode == MODE_TEST) setMsg("A test is already running.");
    else if (!ctl.loadOk || !ctl.tared) setMsg("Load cell not ready - test not started.");
    else startTest(k.runName, now);
  }
  if (k.manual) {
    if (ctl.estop) setMsg("E-stop is on - tap Reset first.");
    else if (ctl.mode == MODE_TEST) setMsg("Auto test running - use EMERGENCY STOP to stop it.");
    else if (k.manualPct <= 0) {
      ctl.pct = 0;
      ctl.mode = MODE_IDLE;
      setMsg("Motor off.");
    } else {
      ctl.pct = k.manualPct > MAX_THROTTLE_PCT ? MAX_THROTTLE_PCT : k.manualPct;
      ctl.mode = MODE_MANUAL;
      setMsg("Manual throttle: %g %%", ctl.pct);
    }
  }
  if (k.tare) {
    if (ctl.mode != MODE_IDLE) setMsg("Turn the motor off before taring.");
    else if (!ctl.loadOk) setMsg("Load cell not responding - can't tare.");
    else {
      beginTare();
      setMsg("Taring - keep the stand still...");
    }
  }
}

void checkSafety(uint32_t now) {
  // stop button (debounced 20 ms, latches like EMERGENCY STOP)
  ctl.btnDown = STOP_BUTTON_PIN >= 0 && digitalRead(STOP_BUTTON_PIN) == LOW;
  if (ctl.btnDown) {
    if (!ctl.btnWas) {
      ctl.btnWas = true;
      ctl.btnSince = now;
    } else if (!ctl.btnTripped && now - ctl.btnSince >= 20) {
      ctl.btnTripped = true;
      stopAll("Stop button pressed - motor off. Tap Reset when it is safe.", true);
    }
  } else {
    ctl.btnWas = false;
    ctl.btnTripped = false;
  }
  // control panel connection
  if (LINK_TIMEOUT_MS > 0 && ctl.mode != MODE_IDLE &&
      (int32_t)(now - lastLinkMs) > (int32_t)LINK_TIMEOUT_MS) {
    stopAll("Lost contact with the control panel - motor stopped.", false);
  }
}

void runTest(uint32_t now) {
  if (ctl.mode != MODE_TEST) return;
  float t = (now - ctl.phaseStartMs) / 1000.0f;
  switch (ctl.phase) {
    case PH_PREP:
      ctl.pct = 0;
      if (t >= PREP_S) {
        beginTare();
        ctl.phase = PH_TARE;
        ctl.phaseStartMs = now;
        setMsg("Auto test: taring the load cell...");
      }
      break;
    case PH_TARE:
      if (ctl.tareLeft == 0) {
        ctl.t0Ms = now;
        ctl.stepIdx = 0;
        startStep(now);
      } else if (t > 5.0f) {
        stopAll("Tare failed - load cell not responding. Test stopped.", false);
      }
      break;
    case PH_STEP:
      if (t >= STEP_HOLD_S) {
        finishStep(now);
        if (++ctl.stepIdx < ctl.nSteps) {
          startStep(now);
        } else {
          ctl.phase = PH_RAMP;
          ctl.phaseStartMs = now;
          ctl.rampFrom = ctl.pct;
          setMsg("Auto test: ramping down");
        }
      }
      break;
    case PH_RAMP:
      if (t >= RAMP_DOWN_S) {
        ctl.pct = 0;
        ctl.phase = PH_ZERO;
        ctl.phaseStartMs = now;
        setMsg("Auto test: zero check (motor off)");
      } else {
        ctl.pct = ctl.rampFrom * (1.0f - t / RAMP_DOWN_S);
      }
      break;
    case PH_ZERO:
      ctl.pct = 0;
      if (t >= ZERO_CHECK_S) {
        zeroAfter = ctl.zN ? (float)(ctl.zSum / ctl.zN) : NAN;
        endTest("");
        ctl.mode = MODE_IDLE;
        ctl.phase = PH_NONE;
        setMsg("Auto test complete - saving the files...");
      }
      break;
    default:
      break;
  }
}

void sendFrame(uint32_t now) {
  bool ask = false;
  if (!ctl.waiting && now - ctl.lastAskMs >= TELEM_PERIOD_MS) {
    while (Serial2.available()) Serial2.read();   // start each reply from a clean buffer
    ctl.tlen = 0;
    ctl.waiting = true;
    ctl.askedMs = ctl.lastAskMs = now;
    ask = true;
  }
  sendDshot(dshotValue(ctl.pct), ask);
}

void readTelemetry(uint32_t now) {
  while (Serial2.available()) {
    uint8_t b = Serial2.read();
    if (!ctl.waiting) continue;            // ignore bytes nobody asked for
    ctl.tbuf[ctl.tlen++] = b;
    if (ctl.tlen < 10) continue;
    ctl.waiting = false;
    if (kissCrc8(ctl.tbuf, 9) != ctl.tbuf[9]) continue;
    ctl.temp  = (int8_t)ctl.tbuf[0];
    ctl.volts = ((ctl.tbuf[1] << 8) | ctl.tbuf[2]) / 100.0f;
    ctl.amps  = ((ctl.tbuf[3] << 8) | ctl.tbuf[4]) / 100.0f;
    ctl.rpm   = ((ctl.tbuf[7] << 8) | ctl.tbuf[8]) * 100.0f / (MOTOR_POLES / 2.0f);
    ctl.lastGoodMs = now;
    // limits count consecutive frames, so one bad frame can't trip them
    bool running = ctl.pct > 0;
    ctl.overI = (running && MAX_CURRENT_A > 0 && ctl.amps > MAX_CURRENT_A) ? ctl.overI + 1 : 0;
    ctl.overT = (running && MAX_ESC_TEMP_C > 0 && ctl.temp > MAX_ESC_TEMP_C) ? ctl.overT + 1 : 0;
    char why[80];
    if (ctl.overI >= 5) {
      snprintf(why, sizeof why, "Current %.1f A is over the %g A limit - motor stopped.", ctl.amps, MAX_CURRENT_A);
      stopAll(why, false);
      ctl.overI = 0;
    }
    if (ctl.overT >= 5) {
      snprintf(why, sizeof why, "ESC at %d C is over the %d C limit - motor stopped.", ctl.temp, MAX_ESC_TEMP_C);
      stopAll(why, false);
      ctl.overT = 0;
    }
  }
  if (ctl.waiting && now - ctl.askedMs > TELEM_TIMEOUT_MS) ctl.waiting = false;   // no reply
}

void readLoadCell(uint32_t now) {
  if (!hxReady()) {
    if (ctl.loadOk && now - ctl.lastSampleMs > 1000) {
      ctl.loadOk = false;
      if (ctl.mode == MODE_TEST) stopAll("Load cell stopped responding - test stopped.", false);
      else setMsg("Load cell not responding - check the HX711 wiring.");
    } else if (!ctl.everOk && !ctl.warnedNoLoad && now - ctl.bootMs > 1500) {
      ctl.warnedNoLoad = true;
      setMsg("Load cell not responding - check the HX711 wiring (DT D26, SCK D27, VCC 3V3, GND).");
    }
    return;
  }
  long raw = hxRead();
  ctl.lastSampleMs = now;
  if (!ctl.loadOk) {
    if (ctl.everOk) setMsg("Load cell responding again.");
    ctl.loadOk = ctl.everOk = true;
  }
  if (ctl.nRead++ == 0) ctl.hist[0] = ctl.hist[1] = ctl.hist[2] = raw;
  ctl.hist[ctl.histIdx] = raw;
  ctl.histIdx = (ctl.histIdx + 1) % 3;

  // At power-up: let the HX711 settle for 2 s, then tare
  if (!ctl.bootTareStarted) {
    if (now - ctl.bootMs < 2000) return;
    ctl.bootTareStarted = true;
    beginTare();
    setMsg("Taring the load cell - keep the stand still...");
  }
  if (ctl.tareLeft > 0) {
    ctl.tareSum += raw;
    ctl.tareN++;
    if (--ctl.tareLeft == 0) {
      ctl.offset = (long)(ctl.tareSum / ctl.tareN);
      bool first = !ctl.tared;
      ctl.tared = true;
      if (ctl.mode != MODE_TEST) {
        if (!first) setMsg("Tare done.");
        else if (calibrated) setMsg("Ready. Motor is off.");
        else setMsg("Load cell NOT calibrated - upload load_cell_calibrate first.");
      }
    }
  }
  // median of the last 3 readings rejects single spikes
  ctl.thrust = (median3(ctl.hist[0], ctl.hist[1], ctl.hist[2]) - ctl.offset) / calFactor;
  if (INVERT_THRUST) ctl.thrust = -ctl.thrust;
  if (!ctl.tared) return;

  if (MAX_THRUST_G > 0 && ctl.pct > 0 && fabsf(ctl.thrust) > MAX_THRUST_G) {
    char why[80];
    snprintf(why, sizeof why, "Thrust %.0f g is over the %g g limit - motor stopped.", ctl.thrust, MAX_THRUST_G);
    stopAll(why, false);
  }
  if (ctl.mode != MODE_TEST) return;

  bool tel = telemFresh(now);
  float t = (now - ctl.phaseStartMs) / 1000.0f;
  if (ctl.phase == PH_STEP && t >= settleS()) {
    StepResult &r = results[ctl.stepIdx];
    r.n++;
    r.sum += ctl.thrust;
    r.sumSq += (double)ctl.thrust * ctl.thrust;
    if (tel) {
      r.nT++;
      r.rpm += ctl.rpm;
      r.volts += ctl.volts;
      r.amps += ctl.amps;
      r.power += ctl.volts * ctl.amps;
      if (ctl.temp > r.maxTemp) r.maxTemp = ctl.temp;
    }
  }
  if (ctl.phase == PH_ZERO && t >= ZERO_CHECK_S * 0.5f) {
    ctl.zSum += ctl.thrust;
    ctl.zN++;
  }
  if (ctl.phase == PH_STEP || ctl.phase == PH_RAMP || ctl.phase == PH_ZERO) {
    Row row;
    row.ms = now - ctl.t0Ms;
    row.pct = ctl.pct;
    row.thrust = ctl.thrust;
    row.rpm = ctl.rpm;
    row.volts = ctl.volts;
    row.amps = ctl.amps;
    row.temp = ctl.temp;
    row.step = ctl.phase == PH_STEP ? ctl.stepIdx + 1 : 0;
    row.telem = tel;
    if (xQueueSend(rowQueue, &row, 0) != pdTRUE) rowsDropped++;
  }
}

void publish(uint32_t now) {
  float t = (now - ctl.phaseStartMs) / 1000.0f, left = 0, after = 0;
  if (ctl.mode == MODE_TEST) {
    float rest = RAMP_DOWN_S + ZERO_CHECK_S;
    switch (ctl.phase) {
      case PH_PREP: left = PREP_S - t; after = TARE_SAMPLES * 0.1f + ctl.nSteps * STEP_HOLD_S + rest; break;
      case PH_TARE: left = ctl.tareLeft * 0.1f; after = ctl.nSteps * STEP_HOLD_S + rest; break;
      case PH_STEP: left = STEP_HOLD_S - t; after = (ctl.nSteps - ctl.stepIdx - 1) * STEP_HOLD_S + rest; break;
      case PH_RAMP: left = RAMP_DOWN_S - t; after = ZERO_CHECK_S; break;
      case PH_ZERO: left = ZERO_CHECK_S - t; break;
      default: break;
    }
    if (left < 0) left = 0;
  }
  bool tel = telemFresh(now);
  portENTER_CRITICAL(&stateMux);
  live.thrust = (ctl.loadOk && ctl.tared) ? ctl.thrust : NAN;
  live.pct = ctl.pct;
  live.rpm = tel ? ctl.rpm : NAN;
  live.volts = tel ? ctl.volts : NAN;
  live.amps = tel ? ctl.amps : NAN;
  live.tempC = ctl.temp;
  live.loadOk = ctl.loadOk;
  live.tared = ctl.tared;
  live.telemOk = tel;
  live.estop = ctl.estop;
  live.buttonDown = ctl.btnDown;
  live.mode = ctl.mode;
  live.phase = ctl.phase;
  live.step = ctl.phase == PH_STEP ? ctl.stepIdx + 1 : 0;
  live.steps = ctl.nSteps;
  live.phaseLeftS = left;
  live.testLeftS = left + after;
  live.testId = ctl.testId;
  live.endedId = ctl.endedId;
  memcpy(live.runName, ctl.runName, sizeof live.runName);
  memcpy(live.endReason, ctl.endReason, sizeof live.endReason);
  if (live.msgId != ctl.msgId) {
    memcpy(live.msg, ctl.msg, sizeof live.msg);
    live.msgId = ctl.msgId;
  }
  portEXIT_CRITICAL(&stateMux);
}

// Runs on its own, every millisecond, whatever the web server is doing
void controlTask(void *) {
  ctl.bootMs = millis();
  for (;;) {
    uint32_t now = millis();
    takeCommands(now);
    checkSafety(now);
    runTest(now);
    sendFrame(now);
    readTelemetry(now);
    readLoadCell(now);
    publish(now);
    vTaskDelay(1);
  }
}

// ============================================================================ recorder (loop task)

const char *RAW_HEADER =
  "time_s,throttle_pct,thrust_g,thrust_N,rpm,voltage_V,current_A,power_W,"
  "efficiency_g_per_W,esc_temp_C,step\n";
const char *SUMMARY_HEADER =
  "step,throttle_pct,start_s,duration_s,n_samples,thrust_g,thrust_std_g,thrust_N,rpm,"
  "voltage_V,current_A,power_W,efficiency_g_per_W,esc_temp_max_C\n";

void requestAbort(const char *why) {
  portENTER_CRITICAL(&stateMux);
  cmd.abort = true;
  strlcpy(cmd.abortText, why, sizeof cmd.abortText);
  portEXIT_CRITICAL(&stateMux);
}

void postNote(const char *text) {
  portENTER_CRITICAL(&stateMux);
  cmd.note = true;
  strlcpy(cmd.noteText, text, sizeof cmd.noteText);
  portEXIT_CRITICAL(&stateMux);
}

bool writeRow(const Row &r) {
  char line[160], step[8] = "";
  if (r.step) snprintf(step, sizeof step, "%u", (unsigned)r.step);
  float thrustN = r.thrust * 0.00980665f;
  int n;
  if (r.telem) {
    float power = r.volts * r.amps;
    char eff[16] = "";
    if (power >= 1.0f && r.thrust >= 1.0f) snprintf(eff, sizeof eff, "%.3f", r.thrust / power);
    n = snprintf(line, sizeof line, "%.3f,%.1f,%.2f,%.4f,%.0f,%.2f,%.2f,%.2f,%s,%d,%s\n",
                 r.ms / 1000.0, r.pct, r.thrust, thrustN, r.rpm, r.volts, r.amps, power, eff, r.temp, step);
  } else {                                 // no telemetry: those cells stay empty
    n = snprintf(line, sizeof line, "%.3f,%.1f,%.2f,%.4f,,,,,,,%s\n",
                 r.ms / 1000.0, r.pct, r.thrust, thrustN, step);
  }
  return rawFile.write((const uint8_t *)line, n) == (size_t)n;
}

void writeSummary(const char *name) {
  char path[64];
  snprintf(path, sizeof path, "/%s_summary.csv", name);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return;
  f.print(SUMMARY_HEADER);
  for (int i = 0; i < nResults; i++) {
    const StepResult &r = results[i];
    char thr[16] = "", sd[16] = "", thrN[16] = "", rpm[16] = "", v[16] = "", a[16] = "",
         p[16] = "", eff[16] = "", tmax[8] = "", line[240];
    if (r.n > 0) {
      double mean = r.sum / r.n;
      double var = r.n > 1 ? (r.sumSq - r.n * mean * mean) / (r.n - 1) : 0.0;
      snprintf(thr, sizeof thr, "%.2f", mean);
      snprintf(sd, sizeof sd, "%.2f", var > 0 ? sqrt(var) : 0.0);
      snprintf(thrN, sizeof thrN, "%.4f", mean * 0.00980665);
      if (r.nT > 0) {
        double pw = r.power / r.nT;
        snprintf(rpm, sizeof rpm, "%.0f", r.rpm / r.nT);
        snprintf(v, sizeof v, "%.2f", r.volts / r.nT);
        snprintf(a, sizeof a, "%.2f", r.amps / r.nT);
        snprintf(p, sizeof p, "%.2f", pw);
        if (pw >= 1.0 && mean >= 1.0) snprintf(eff, sizeof eff, "%.3f", mean / pw);
        snprintf(tmax, sizeof tmax, "%d", r.maxTemp);
      }
    }
    snprintf(line, sizeof line, "%d,%.1f,%.1f,%.1f,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
             i + 1, r.pct, r.startS, r.durS, r.n, thr, sd, thrN, rpm, v, a, p, eff, tmax);
    f.print(line);
  }
  f.close();
}

void writeInfo(const char *name, const char *reason) {
  char path[64];
  snprintf(path, sizeof path, "/%s_info.txt", name);
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) return;
  f.printf("Thrust stand auto test: %s\n", name);
  if (reason[0]) f.printf("Result: stopped early (%s)\n", reason);
  else f.printf("Result: complete\n");
  f.printf("Steps completed: %d of %d\n", (int)nResults, buildSteps(nullptr));
  f.printf("Settings: %g %% steps up to %g %%, %g s per step, first %g s of each step not averaged\n",
           STEP_PCT, MAX_THROTTLE_PCT, STEP_HOLD_S, settleS());
  float zeroBefore = (nResults > 0 && results[0].pct == 0 && results[0].n > 0)
                     ? (float)(results[0].sum / results[0].n) : NAN;
  float after = zeroAfter;
  if (!isnan(zeroBefore)) f.printf("Zero before (0 %% step): %.2f g\n", zeroBefore);
  if (!isnan(after)) f.printf("Zero after (motor off at the end): %.2f g\n", after);
  if (!isnan(zeroBefore) && !isnan(after)) f.printf("Zero drift: %+.2f g\n", after - zeroBefore);
  f.printf("Calibration factor: %.4f%s\n", calFactor, calibrated ? "" : " (NOT calibrated)");
  f.printf("Readings recorded: %u, dropped: %u\n", (unsigned)recRows, (unsigned)rowsDropped);
  f.printf("Firmware: %s\n", FW_VERSION);
  f.close();
}

// Writes the readings of a running test to its files, and finishes them when it ends
void serviceRecorder() {
  uint32_t testId, endedId;
  char name[40], reason[80];
  portENTER_CRITICAL(&stateMux);
  testId = live.testId;
  endedId = live.endedId;
  memcpy(name, live.runName, sizeof name);
  memcpy(reason, live.endReason, sizeof reason);
  portEXIT_CRITICAL(&stateMux);

  if (testId != recId) {                   // a new test has started
    recId = testId;
    recRows = 0;
    recFailed = false;
    strlcpy(recName, name, sizeof recName);
    char path[64];
    snprintf(path, sizeof path, "/%s_raw.csv", recName);
    if (fsOk) rawFile = LittleFS.open(path, FILE_WRITE);
    recOpen = fsOk && rawFile;
    if (recOpen) rawFile.print(RAW_HEADER);
    else requestAbort("Could not create the data file (storage full?) - test stopped.");
  }

  Row r;
  for (int i = 0; i < 20 && xQueueReceive(rowQueue, &r, 0) == pdTRUE; i++) {
    if (!recOpen || recFailed) continue;
    if (writeRow(r)) {
      if (++recRows % 50 == 0) rawFile.flush();
    } else {
      recFailed = true;
      requestAbort("Storage full - test stopped. Delete some old runs.");
    }
  }

  if (recOpen && endedId == recId && uxQueueMessagesWaiting(rowQueue) == 0) {
    rawFile.close();
    recOpen = false;
    writeSummary(recName);
    writeInfo(recName, reason);
    savedCount++;
    strlcpy(lastRun, recName, sizeof lastRun);
    if (reason[0]) snprintf(lastResult, sizeof lastResult, "stopped early (%s)", reason);
    else strlcpy(lastResult, "complete", sizeof lastResult);
    char note[120];
    if (reason[0]) snprintf(note, sizeof note, "Test stopped. Data up to that point saved as %s.", recName);
    else snprintf(note, sizeof note, "Auto test complete. Saved as %s.", recName);
    if (!reason[0]) postNote(note);        // a stop keeps its own message on screen
    Serial.println(note);
  }
}

// ============================================================================ web server (loop task)

void touchLink() {
  lastLinkMs = millis();
}

Live snapshot() {
  Live s;
  portENTER_CRITICAL(&stateMux);
  s = live;
  portEXIT_CRITICAL(&stateMux);
  return s;
}

void queueCommand(Commands &k) {           // merge a request into cmd
  portENTER_CRITICAL(&stateMux);
  if (k.estop) cmd.estop = true;
  if (k.resetEstop) cmd.resetEstop = true;
  if (k.tare) cmd.tare = true;
  if (k.manual) {
    cmd.manual = true;
    cmd.manualPct = k.manualPct;
  }
  if (k.start) {
    cmd.start = true;
    memcpy(cmd.runName, k.runName, sizeof cmd.runName);
  }
  portEXIT_CRITICAL(&stateMux);
}

void reply(int code, const char *error) {  // error = nullptr means OK
  char b[200];
  if (error) snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", error);
  else strlcpy(b, "{\"ok\":true}", sizeof b);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", b);
}

const char *num(char *buf, size_t n, float v, int decimals) {
  if (isnan(v)) return "null";
  snprintf(buf, n, "%.*f", decimals, v);
  return buf;
}

void jsonEscape(char *out, size_t n, const char *in) {
  size_t j = 0;
  for (; *in && j + 2 < n; in++) {
    char ch = *in;
    if (ch == '"' || ch == '\\') {
      out[j++] = '\\';
      out[j++] = ch;
    } else if ((unsigned char)ch >= 32) {
      out[j++] = ch;
    }
  }
  out[j] = 0;
}

// Keeps letters, digits, - and _ (the rest becomes _), at most n-1 characters
void cleanName(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (; *in && j + 1 < n; in++) {
    char ch = *in;
    out[j++] = (isalnum((unsigned char)ch) || ch == '-' || ch == '_') ? ch : '_';
  }
  out[j] = 0;
}

bool validFileName(const String &s) {
  if (s.length() == 0 || s.length() > 60 || s[0] == '.') return false;
  for (unsigned i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (!isalnum((unsigned char)ch) && ch != '-' && ch != '_' && ch != '.') return false;
  }
  return true;
}

void handleRoot() {
  touchLink();
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleStatus() {
  touchLink();
  Live s = snapshot();
  static const char *MODES[] = {"idle", "manual", "test"};
  static const char *PHASES[] = {"", "prep", "tare", "step", "ramp", "zero"};
  char th[16], rp[16], vo[16], am[16], tp[8], msg[250], last[210], json[1400];
  jsonEscape(msg, sizeof msg, s.msg);
  jsonEscape(last, sizeof last, lastResult);
  if (s.telemOk) snprintf(tp, sizeof tp, "%d", s.tempC);
  else strlcpy(tp, "null", sizeof tp);
  int planned = buildSteps(nullptr);
  snprintf(json, sizeof json,
    "{\"thrust\":%s,\"pct\":%.1f,\"rpm\":%s,\"volts\":%s,\"amps\":%s,\"temp\":%s,"
    "\"telem\":%s,\"loadOk\":%s,\"tared\":%s,\"cal\":%s,\"estop\":%s,\"button\":%s,"
    "\"mode\":\"%s\",\"phase\":\"%s\",\"step\":%d,\"steps\":%d,"
    "\"phaseLeft\":%.1f,\"testLeft\":%.1f,\"testTotal\":%.1f,\"msg\":\"%s\","
    "\"saving\":%s,\"saved\":%u,\"last\":\"%s\",\"lastResult\":\"%s\",\"fs\":%s,"
    "\"max\":%g,\"stepPct\":%g,\"hold\":%g,\"linkTimeout\":%u}",
    num(th, sizeof th, s.thrust, 1), s.pct, num(rp, sizeof rp, s.rpm, 0),
    num(vo, sizeof vo, s.volts, 2), num(am, sizeof am, s.amps, 2), tp,
    s.telemOk ? "true" : "false", s.loadOk ? "true" : "false", s.tared ? "true" : "false",
    calibrated ? "true" : "false", s.estop ? "true" : "false", s.buttonDown ? "true" : "false",
    MODES[s.mode], PHASES[s.phase], s.step, s.mode == MODE_TEST ? s.steps : planned,
    s.phaseLeftS, s.testLeftS, testSeconds(planned), msg,
    recOpen ? "true" : "false", (unsigned)savedCount, lastRun, last, fsOk ? "true" : "false",
    MAX_THROTTLE_PCT, STEP_PCT, STEP_HOLD_S, (unsigned)LINK_TIMEOUT_MS);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void handleEstop() {
  touchLink();
  Commands k = {};
  k.estop = true;
  queueCommand(k);
  reply(200, nullptr);
}

void handleReset() {
  touchLink();
  Commands k = {};
  k.resetEstop = true;
  queueCommand(k);
  reply(200, nullptr);
}

void handleManual() {
  touchLink();
  String a = server.arg("pct");
  bool numeric = a.length() > 0 && a.length() < 8;
  for (unsigned i = 0; numeric && i < a.length(); i++) numeric = isdigit((unsigned char)a[i]) || a[i] == '.';
  float p = numeric ? a.toFloat() : -1;
  if (!numeric || p < 0 || p > MAX_THROTTLE_PCT) {
    char e[80];
    snprintf(e, sizeof e, "Throttle must be 0-%g %%.", MAX_THROTTLE_PCT);
    return reply(400, e);
  }
  Live s = snapshot();
  if (s.estop && p > 0) return reply(409, "E-stop is on - tap Reset first.");
  if (s.mode == MODE_TEST) return reply(409, "Auto test running - use EMERGENCY STOP to stop it.");
  Commands k = {};
  k.manual = true;
  k.manualPct = p;
  queueCommand(k);
  reply(200, nullptr);
}

void handleStart() {
  touchLink();
  Live s = snapshot();
  if (!fsOk) return reply(409, "File storage is not working - can't record.");
  if (recOpen) return reply(409, "Still saving the last run - try again in a moment.");
  if (s.estop) return reply(409, "E-stop is on - tap Reset first.");
  if (s.mode == MODE_TEST) return reply(409, "A test is already running.");
  if (!s.loadOk || !s.tared) return reply(409, "Load cell not responding - check the wiring.");
  if (!calibrated) return reply(409, "Load cell not calibrated - upload load_cell_calibrate first.");
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  size_t needed = (size_t)(testSeconds(buildSteps(nullptr)) * 10 * 80) + 16384;
  if (freeBytes < needed) return reply(409, "Not enough storage left - delete some old runs.");

  // File name: date and time from the phone, plus the label
  char base[33], name[40], path[64];
  cleanName(server.arg("name").c_str(), base, sizeof base);
  if (!base[0]) snprintf(base, sizeof base, "run%lu", (unsigned long)(millis() / 1000));
  strlcpy(name, base, sizeof name);
  for (int k = 2; k < 100; k++) {          // don't overwrite an earlier run
    snprintf(path, sizeof path, "/%s_raw.csv", name);
    if (!LittleFS.exists(path)) break;
    snprintf(name, sizeof name, "%s_%d", base, k);
  }
  Commands k = {};
  k.start = true;
  strlcpy(k.runName, name, sizeof k.runName);
  queueCommand(k);
  reply(200, nullptr);
}

void handleTare() {
  touchLink();
  Live s = snapshot();
  if (s.mode != MODE_IDLE) return reply(409, "Turn the motor off before taring.");
  if (!s.loadOk) return reply(409, "Load cell not responding.");
  Commands k = {};
  k.tare = true;
  queueCommand(k);
  reply(200, nullptr);
}

void handleFiles() {
  touchLink();
  String j = "{\"files\":[";
  bool first = true;
  if (fsOk) {
    File root = LittleFS.open("/");
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (f.isDirectory()) continue;
      char n[70];
      jsonEscape(n, sizeof n, f.name());
      if (!first) j += ",";
      first = false;
      j += "{\"n\":\"";
      j += n;
      j += "\",\"s\":";
      j += String((unsigned)f.size());
      j += "}";
    }
  }
  j += "],\"used\":";
  j += String((unsigned)(fsOk ? LittleFS.usedBytes() : 0));
  j += ",\"total\":";
  j += String((unsigned)(fsOk ? LittleFS.totalBytes() : 0));
  j += ",\"recording\":\"";
  j += recOpen ? recName : "";
  j += "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleDownload() {
  touchLink();
  String n = server.arg("f");
  if (!fsOk || !validFileName(n)) {
    server.send(400, "text/plain", "Bad file name");
    return;
  }
  if (recOpen && n.startsWith(String(recName) + "_")) {
    server.send(409, "text/plain", "That run is still being recorded.");
    return;
  }
  String path = String("/") + n;
  if (!LittleFS.exists(path)) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  File f = LittleFS.open(path, FILE_READ);
  server.sendHeader("Content-Disposition", String("attachment; filename=\"") + n + "\"");
  server.streamFile(f, n.endsWith(".csv") ? "text/csv" : "text/plain");
  f.close();
}

void handleDelete() {
  touchLink();
  String run = server.arg("run");
  if (!fsOk || !validFileName(run)) return reply(400, "Bad run name.");
  if (recOpen && run == recName) return reply(409, "That run is still being recorded.");
  const char *suffixes[] = {"_raw.csv", "_summary.csv", "_info.txt"};
  int removed = 0;
  for (const char *sfx : suffixes) {
    String path = String("/") + run + sfx;
    if (LittleFS.exists(path) && LittleFS.remove(path)) removed++;
  }
  if (!removed) return reply(404, "Run not found.");
  reply(200, nullptr);
}

// ============================================================================ setup / loop

bool loadCalibration(float &factor) {
  if (!prefs.begin("loadcell", true)) return false;   // read-only; fails if never saved
  bool found = prefs.isKey("cal");
  factor = prefs.getFloat("cal", 0.0f);
  prefs.end();
  return found && factor != 0.0f && !isnan(factor);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Thrust stand: web control panel ===");

  calibrated = loadCalibration(calFactor);
  if (!calibrated) calFactor = 1.0f;
  Serial.printf("Calibration factor: %.4f%s\n", calFactor, calibrated ? "" : "  (NOT calibrated)");

  pinMode(HX711_SCK, OUTPUT);
  digitalWrite(HX711_SCK, LOW);
  pinMode(HX711_DOUT, INPUT_PULLUP);
  if (STOP_BUTTON_PIN >= 0) pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  if (!rmtInit(ESC_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_HZ)) {
    Serial.println("ERROR: could not start the DShot output on D25. Stopped.");
    while (true) delay(1000);
  }
  Serial2.begin(115200, SERIAL_8N1, TELEM_RX_PIN, TELEM_TX_PIN);
  rowQueue = xQueueCreate(ROW_QUEUE_LEN, sizeof(Row));

  // From here on the ESC gets zero-throttle frames every millisecond
  xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 3, nullptr, 1);

  Serial.println("Starting file storage (the first start after an upload can take ~30 s)...");
  fsOk = LittleFS.begin(true);
  if (fsOk) Serial.printf("Storage: %u KB free\n", (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
  else Serial.println("ERROR: file storage failed. Check Tools > Partition Scheme (needs spiffs).");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_NAME, WIFI_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/estop", HTTP_ANY, handleEstop);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/manual", HTTP_POST, handleManual);
  server.on("/api/start", HTTP_POST, handleStart);
  server.on("/api/tare", HTTP_POST, handleTare);
  server.on("/api/files", HTTP_GET, handleFiles);
  server.on("/api/delete", HTTP_POST, handleDelete);
  server.on("/download", HTTP_GET, handleDownload);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();

  Serial.printf("Wi-Fi network: %s   password: %s\n", WIFI_NAME, WIFI_PASSWORD);
  Serial.print("Control panel: http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  server.handleClient();
  serviceRecorder();

  // echo the status line to the USB serial port too
  uint32_t id;
  char msg[120];
  portENTER_CRITICAL(&stateMux);
  id = live.msgId;
  memcpy(msg, live.msg, sizeof msg);
  portEXIT_CRITICAL(&stateMux);
  if (id != printedMsgId) {
    printedMsgId = id;
    Serial.println(msg);
  }
}
