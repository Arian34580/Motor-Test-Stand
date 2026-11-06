/*
  load_cell_calibrate.ino  -  run this ONCE to calibrate the thrust stand's load cell
  =============================================================================
  ESP32 + HX711. Walks you through a tare and a known-mass measurement, then saves
  the calibration factor in the ESP32's flash memory. The factor survives power-offs
  and uploading other sketches, and thrust_measure.ino reads it on every startup.
  Upload this sketch again only when you want to recalibrate.

  Wiring:  HX711 VCC -> 3V3   GND -> GND   DT -> D26   SCK -> D27
  Serial Monitor: 115200 baud. Type into the box at the top and press Enter.
  Library: HX711_ADC (Tools -> Manage Libraries)

  Keep Tools -> "Erase All Flash Before Sketch Upload" set to Disabled (the default),
  or uploading a sketch will wipe the saved calibration.
*/

#include <HX711_ADC.h>
#include <Preferences.h>

const int   HX711_DOUT = 26;
const int   HX711_SCK  = 27;
const float MIN_COUNTS = 500;     // the mass must move the raw reading at least this much

HX711_ADC LoadCell(HX711_DOUT, HX711_SCK);
Preferences prefs;

// ============================================================ typed input

String rxLine;
unsigned long rxLastMs = 0;

// True when a full line has been typed. Works with any Serial Monitor line-ending setting.
bool pollLine(String &out) {
  while (Serial.available()) {
    char c = Serial.read();
    rxLastMs = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      out = rxLine;
      rxLine = "";
      out.trim();
      return true;
    }
    if (rxLine.length() < 30) rxLine += c;
  }
  if (rxLine.length() > 0 && millis() - rxLastMs > 200) {   // "No line ending" setting
    out = rxLine;
    rxLine = "";
    out.trim();
    return true;
  }
  return false;
}

// Waits for a non-empty line, keeping the load cell readings flowing meanwhile.
String waitForInput() {
  String s;
  while (true) {
    LoadCell.update();
    if (pollLine(s) && s.length() > 0) return s;
    delay(1);
  }
}

// ============================================================ flash storage

bool loadCalibration(float &factor, float &mass) {
  if (!prefs.begin("loadcell", true)) return false;   // read-only; fails if never saved
  bool found = prefs.isKey("cal");
  factor = prefs.getFloat("cal", 0.0f);
  mass   = prefs.getFloat("mass", 0.0f);
  prefs.end();
  return found && factor != 0.0f && !isnan(factor);
}

bool saveCalibration(float factor, float mass) {
  if (!prefs.begin("loadcell", false)) return false;
  bool ok = prefs.putFloat("cal", factor) == sizeof(float);
  prefs.putFloat("mass", mass);
  prefs.end();
  return ok;
}

// ============================================================ calibration

void printReadingsFor(unsigned long ms) {
  unsigned long start = millis();
  unsigned long lastPrint = 0;
  while (millis() - start < ms) {
    LoadCell.update();
    if (millis() - lastPrint >= 500) {
      lastPrint = millis();
      Serial.print("  Reading: ");
      Serial.print(LoadCell.getData(), 1);
      Serial.println(" g");
    }
    delay(1);
  }
}

// Returns true once a calibration is saved, false if you chose to start over.
bool runCalibration() {
  // ---- Step 1: tare
  Serial.println();
  Serial.println("STEP 1 of 3: Take all added weight off the load cell.");
  Serial.println("  Leave the stand as it will be during tests (motor and prop mounted, motor stopped).");
  Serial.println("  Then type t and press Enter.");
  while (!waitForInput().equalsIgnoreCase("t")) {
    Serial.println("  Type t and press Enter when the load cell is unloaded.");
  }
  Serial.println("  Taring... don't touch the stand.");
  LoadCell.setCalFactor(1.0);      // work in raw counts until the new factor is known
  LoadCell.tare();
  Serial.println("  Tare done.");

  // ---- Step 2: known mass
  float mass = 0;
  float counts = 0;
  while (true) {
    Serial.println();
    Serial.println("STEP 2 of 3: Put a known mass on, pulling in the same direction thrust will push");
    Serial.println("  (for example hung from a string over a pulley). The heavier the better, ideally");
    Serial.println("  close to the thrust you will measure. Wait for it to stop swinging, then type");
    Serial.println("  its mass in grams (for example 500) and press Enter.");
    mass = waitForInput().toFloat();
    if (mass <= 0) {
      Serial.println("  That isn't a mass in grams. Try again.");
      continue;
    }
    Serial.println("  Measuring for about 2 s... keep it still.");
    LoadCell.refreshDataSet();     // a fresh average taken with the mass on
    counts = LoadCell.getData();   // raw counts above the tare (the factor is still 1.0)
    if (fabs(counts) < MIN_COUNTS) {
      Serial.print("  The reading barely changed (");
      Serial.print(counts, 0);
      Serial.println(" counts). Is the mass on the cell? Check the load cell's 4 wires.");
      continue;
    }
    break;
  }
  float factor = counts / mass;
  LoadCell.setCalFactor(factor);

  // ---- Step 3: check, then save
  Serial.println();
  Serial.print("STEP 3 of 3: New calibration factor: ");
  Serial.println(factor, 4);
  Serial.print("  These readings should all be close to ");
  Serial.print(mass, 1);
  Serial.println(" g:");
  printReadingsFor(5000);

  while (true) {
    Serial.println("  Type s to save this calibration, or r to start over.");
    String in = waitForInput();
    if (in.equalsIgnoreCase("r")) return false;
    if (!in.equalsIgnoreCase("s")) continue;

    float checkFactor = 0, checkMass = 0;
    if (saveCalibration(factor, mass) && loadCalibration(checkFactor, checkMass) && checkFactor == factor) {
      Serial.println("  Saved. thrust_measure will use this factor every time it starts.");
      return true;
    }
    Serial.println("  ERROR: saving to flash failed.");
  }
}

void printDoneHelp() {
  Serial.println();
  Serial.println("Done. Live readings follow, so you can check other known weights.");
  Serial.println("  t = re-tare for checking (doesn't change the saved factor)   r = recalibrate");
  Serial.println("When you're finished, upload thrust_measure.");
}

// ============================================================ setup / loop

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== Load cell calibration ===");

  float oldFactor = 0, oldMass = 0;
  if (loadCalibration(oldFactor, oldMass)) {
    Serial.print("Currently saved factor: ");
    Serial.print(oldFactor, 4);
    Serial.print(" (calibrated with ");
    Serial.print(oldMass, 1);
    Serial.println(" g). It is only replaced if you save a new one.");
  } else {
    Serial.println("No calibration saved yet.");
  }

  Serial.println("Starting the load cell...");
  LoadCell.begin();
  LoadCell.start(2000, true);      // settle 2 s, then tare
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: no response from the HX711.");
    Serial.println("Check wiring: VCC -> 3V3, GND -> GND, DT -> D26, SCK -> D27. Then press EN.");
    while (true) delay(1000);
  }
  LoadCell.setCalFactor(1.0);

  while (!runCalibration()) {}
  printDoneHelp();
}

void loop() {
  static unsigned long lastPrint = 0;

  LoadCell.update();
  if (LoadCell.getTareStatus()) Serial.println("Tare done.");

  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    Serial.print("Reading: ");
    Serial.print(LoadCell.getData(), 1);
    Serial.println(" g");
  }

  String in;
  if (pollLine(in)) {
    if (in.equalsIgnoreCase("r")) {
      while (!runCalibration()) {}
      printDoneHelp();
    } else if (in.equalsIgnoreCase("t")) {
      LoadCell.tareNoDelay();
      Serial.println("Taring...");
    }
  }
}
