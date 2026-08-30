/*
  ============================================================
  ADPM — SENSOR-SELECT FIELD FIRMWARE v5.0
  Animal Disease Prediction Meter (Meter side only)
  ESP32 | 4 Sensors | Offline | Calibration Saved

  CHANGE FROM v4.0:
  The meter no longer stores an animal database or runs the
  disease-prediction engine on-device. Instead:

    1. User selects WHICH SENSOR to read from a menu
       (Temperature / pH / TDS / Respiration) using UP/DOWN.
    2. Pressing SELECT starts a reading cycle for just that
       sensor and stores the result.
    3. User can repeat for any/all of the 4 sensors, in any
       order, any number of times (each new reading overwrites
       the previous one for that sensor).
    4. A "View Summary" screen shows all captured readings
       together, and also prints them to Serial as CSV/JSON so
       they can be copied straight into the website.
    5. The actual species selection + disease prediction now
       happens entirely on the companion website
       (animal-disease-prediction-meter.html), which mirrors
       the same 11-rule engine in JavaScript.

  Hardware pins (unchanged from v4.0):
    DS18B20 temperature probe         -> GPIO 4
    pH Sensor PH-4502C analog out     -> GPIO 34
    TDS Sensor V1.0 analog out        -> GPIO 35
    Respiration Piezo Belt analog out -> GPIO 32
    SSD1306 OLED I2C                  -> SDA GPIO21, SCL GPIO22
    Buttons                           -> UP 14, DOWN 27, SELECT 26, BACK 25
    Passive/active buzzer             -> GPIO 33

  Button use:
    UP/DOWN    : move selection in menus
    SELECT     : start reading / confirm / open item
    BACK       : go back one screen
    Long BACK  : sleep from the sensor menu

  Arduino libraries required:
    OneWire by Paul Stoffregen
    DallasTemperature by Miles Burton
    Adafruit GFX Library
    Adafruit SSD1306

  IMPORTANT: This is a screening/monitoring device. It is not a
  final veterinary diagnostic instrument. Use output as early
  warning only, and confirm with a veterinarian.
  ============================================================
*/

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <esp_sleep.h>

// ---------------- Pins ----------------
#define ONE_WIRE_BUS 4
#define PH_PIN       34
#define TDS_PIN      35
#define RESP_PIN     32
#define BUZZER_PIN   33

#define BTN_UP       14
#define BTN_DOWN     27
#define BTN_SELECT   26
#define BTN_BACK     25

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- Sensors ----------------
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
Preferences prefs;

// ---------------- Default calibration ----------------
#define PH_VREF  3.3f
#define TDS_VREF 3.3f
#define RESP_WINDOW_MS 15000UL
#define IDLE_SLEEP_MS  300000UL   // 5 minutes idle sleep

float phOffset = 0.0f;
float tdsFactor = 0.50f;
int respThreshold = 2000;

// ---------------- Sensor menu ----------------
const char* SENSOR_NAMES[4] = { "Temperature", "pH Level", "TDS (Water)", "Respiration" };
#define SENSOR_MENU_ITEMS 5   // 4 sensors + "View Summary"
#define VIEW_SUMMARY_INDEX 4

// ---------------- State ----------------
// appState: 0 sensor menu, 1 reading in progress, 2 summary,
//           5 tools, 6 calibrate, 7 raw test (blocking function)
int appState = 0;
int menuIndex = 0;
int menuOffset = 0;
int toolIndex = 0;
int calIndex = 0;

// Captured readings + "have we read this yet" flags
float lastTemp = 0.0f;  bool haveTemp = false;
float lastPH   = 0.0f;  bool havePH   = false;
int   lastTDS  = 0;     bool haveTDS  = false;
int   lastResp = 0;     bool haveResp = false;

unsigned long lastBtnTime = 0;
unsigned long lastActivity = 0;
#define DEBOUNCE_MS 180

// ============================================================
// Button helpers
// ============================================================
bool isPressed(int pin) { return digitalRead(pin) == LOW; }

bool btnPressed(int pin) {
  if (millis() - lastBtnTime < DEBOUNCE_MS) return false;
  if (isPressed(pin)) {
    lastBtnTime = millis();
    lastActivity = millis();
    return true;
  }
  return false;
}

bool longBackPressed() {
  static unsigned long t0 = 0;
  if (isPressed(BTN_BACK)) {
    if (t0 == 0) t0 = millis();
    if (millis() - t0 > 1800) { t0 = 0; return true; }
  } else t0 = 0;
  return false;
}

void saveSettings() {
  prefs.putFloat("phOffset", phOffset);
  prefs.putFloat("tdsFactor", tdsFactor);
  prefs.putInt("respThr", respThreshold);
}

void loadSettings() {
  phOffset = prefs.getFloat("phOffset", 0.0f);
  tdsFactor = prefs.getFloat("tdsFactor", 0.50f);
  respThreshold = prefs.getInt("respThr", 2000);
}

// ============================================================
// OLED helpers
// ============================================================
void oledClear() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
}

void showSplash() {
  oledClear();
  display.setCursor(8, 2);  display.println("ADPM Sensor Meter");
  display.setCursor(8, 15); display.println("Select sensor,");
  display.setCursor(8, 27); display.println("take reading,");
  display.setCursor(8, 39); display.println("enter on website");
  display.setCursor(8, 53); display.println("Press any button");
  display.display();
}

String fmtVal(int i) {
  switch (i) {
    case 0: return haveTemp ? (String(lastTemp, 1) + "C") : "--";
    case 1: return havePH   ? (String(lastPH, 2))         : "--";
    case 2: return haveTDS  ? (String(lastTDS) + "ppm")   : "--";
    case 3: return haveResp ? (String(lastResp) + "/min") : "--";
  }
  return "--";
}

void showSensorMenu() {
  oledClear();
  display.setCursor(0, 0); display.println("Select Sensor");
  display.setCursor(94, 0); display.print("B=Tls");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  for (int i = 0; i < 4; i++) {
    int idx = menuOffset + i;
    if (idx >= SENSOR_MENU_ITEMS) break;
    int y = 14 + i * 12;
    if (idx == menuIndex) {
      display.fillRect(0, y - 1, 128, 11, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, y);
    if (idx == VIEW_SUMMARY_INDEX) {
      display.print(">> View Summary");
    } else {
      display.print(SENSOR_NAMES[idx]);
      display.setCursor(90, y);
      display.print(fmtVal(idx));
    }
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56); display.println("SEL=Read HOLD BACK=Sleep");
  display.display();
}

void showReadingHeader(const char* sensorName) {
  oledClear();
  display.setCursor(0, 0); display.print("Reading: "); display.println(sensorName);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void showSummary() {
  oledClear();
  display.setCursor(0, 0); display.println("Captured Readings");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 13); display.print("Temp: "); display.println(fmtVal(0));
  display.setCursor(0, 24); display.print("pH  : "); display.println(fmtVal(1));
  display.setCursor(0, 35); display.print("TDS : "); display.println(fmtVal(2));
  display.setCursor(0, 46); display.print("Resp: "); display.println(fmtVal(3));
  display.setCursor(0, 56); display.println("Enter on website. BACK=Menu");
  display.display();
}

// Print all captured readings to Serial in a copy-paste friendly
// CSV line plus a JSON object, so they can be typed/pasted into
// the website's manual-entry fields.
void printSummarySerial() {
  Serial.println(F("---- ADPM READING SUMMARY ----"));
  Serial.print(F("CSV: "));
  Serial.print(haveTemp ? String(lastTemp, 1) : "NA"); Serial.print(",");
  Serial.print(havePH   ? String(lastPH, 2)   : "NA"); Serial.print(",");
  Serial.print(haveTDS  ? String(lastTDS)     : "NA"); Serial.print(",");
  Serial.println(haveResp ? String(lastResp) : "NA");

  Serial.print(F("JSON: {\"temp\":"));
  Serial.print(haveTemp ? String(lastTemp, 1) : "null"); Serial.print(F(",\"ph\":"));
  Serial.print(havePH   ? String(lastPH, 2)   : "null"); Serial.print(F(",\"tds\":"));
  Serial.print(haveTDS  ? String(lastTDS)     : "null"); Serial.print(F(",\"resp\":"));
  Serial.print(haveResp ? String(lastResp)    : "null"); Serial.println(F("}"));
  Serial.println(F("-------------------------------"));
}

// ============================================================
// Sensor read functions (unchanged sensing logic from v4.0)
// ============================================================
float readTemperature() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C || t < -50 || t > 120) return -999.0f;
  return t;
}

float readPHRawVoltage() {
  long sum = 0;
  for (int i = 0; i < 25; i++) { sum += analogRead(PH_PIN); delay(8); }
  return (sum / 25.0f) * (PH_VREF / 4095.0f);
}

float readPH() {
  float v = readPHRawVoltage();
  float ph = 7.0f + ((2.5f - v) / 0.18f) + phOffset;
  return constrain(ph, 0.0f, 14.0f);
}

int readTDS() {
  long sum = 0;
  for (int i = 0; i < 25; i++) { sum += analogRead(TDS_PIN); delay(8); }
  float v = (sum / 25.0f) * (TDS_VREF / 4095.0f);
  float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * tdsFactor;
  return (int)constrain(tds, 0.0f, 5000.0f);
}

int readRespirationLive() {
  int breathCount = 0;
  bool inPeak = false;
  unsigned long t0 = millis();
  while (millis() - t0 < RESP_WINDOW_MS) {
    int val = analogRead(RESP_PIN);
    unsigned long elapsed = millis() - t0;
    int remaining = (RESP_WINDOW_MS - elapsed) / 1000 + 1;

    if (val > respThreshold && !inPeak) { inPeak = true; breathCount++; }
    if (val < respThreshold - 120) inPeak = false;  // hysteresis prevents double count

    oledClear();
    display.setCursor(0, 0); display.println("Reading: Respiration");
    display.setCursor(0, 12); display.println("Belt on chest/ribs");
    display.setCursor(0, 26); display.print("Breaths: "); display.println(breathCount);
    display.setCursor(0, 38); display.print("ADC: "); display.print(val); display.print(" T:"); display.println(respThreshold);
    display.setCursor(0, 50); display.print("Left: "); display.print(remaining); display.println("s");
    int barW = (int)((elapsed * 128UL) / RESP_WINDOW_MS);
    display.fillRect(0, 61, barW, 3, SSD1306_WHITE);
    display.display();
    delay(45);
  }
  return (int)((float)breathCount * (60000.0f / RESP_WINDOW_MS));
}

// Runs the reading cycle for exactly one sensor (menuIndex 0-3)
void runSingleReading(int sensorIdx) {
  switch (sensorIdx) {
    case 0: { // Temperature
      showReadingHeader("Temperature");
      display.setCursor(0, 20); display.println("Hold probe in place..."); display.display();
      delay(400);
      lastTemp = readTemperature();
      haveTemp = true;
      break;
    }
    case 1: { // pH
      showReadingHeader("pH Level");
      display.setCursor(0, 20); display.println("Probe in saliva/water..."); display.display();
      delay(400);
      lastPH = readPH();
      havePH = true;
      break;
    }
    case 2: { // TDS
      showReadingHeader("TDS (Water)");
      display.setCursor(0, 20); display.println("Probe fully submerged..."); display.display();
      delay(400);
      lastTDS = readTDS();
      haveTDS = true;
      break;
    }
    case 3: { // Respiration (takes RESP_WINDOW_MS, has its own live screen)
      lastResp = readRespirationLive();
      haveResp = true;
      break;
    }
  }

  // Confirmation beep + brief result flash
  digitalWrite(BUZZER_PIN, HIGH); delay(120); digitalWrite(BUZZER_PIN, LOW);
  oledClear();
  display.setCursor(0, 0); display.println("Reading Captured");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  display.setCursor(0, 22); display.setTextSize(2);
  display.println(fmtVal(sensorIdx));
  display.setTextSize(1);
  display.setCursor(0, 56); display.println("Returning to menu...");
  display.display();
  delay(1200);
}

// ============================================================
// Tools and calibration (unchanged from v4.0)
// ============================================================
void showTools() {
  const char* tools[] = { "Calibrate", "Raw Sensor Test", "Reset Calib", "Clear Readings", "Sleep" };
  oledClear();
  display.setCursor(0, 0); display.println("Tools");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  for (int i = 0; i < 5; i++) {
    int y = 12 + i * 10;
    if (i == toolIndex) { display.fillRect(0, y - 1, 128, 9, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.println(tools[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56); display.println("SEL=Open BACK=Menu");
  display.display();
}

void showCalMenu() {
  const char* items[] = { "pH7 Auto Cal", "Resp Auto Cal", "Resp Thr +/-", "TDS Factor +/-" };
  oledClear();
  display.setCursor(0, 0); display.println("Calibration");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  for (int i = 0; i < 4; i++) {
    int y = 14 + i * 12;
    if (i == calIndex) { display.fillRect(0, y - 1, 128, 11, SSD1306_WHITE); display.setTextColor(SSD1306_BLACK); }
    else display.setTextColor(SSD1306_WHITE);
    display.setCursor(2, y); display.println(items[i]);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56); display.println("SEL=Run BACK=Tools");
  display.display();
}

void calibratePH7() {
  oledClear(); display.setCursor(0, 0); display.println("Put pH probe in"); display.setCursor(0, 12); display.println("pH 7.00 solution"); display.setCursor(0, 30); display.println("Press SELECT"); display.display();
  while (!btnPressed(BTN_SELECT)) { if (btnPressed(BTN_BACK)) return; delay(20); }
  float v = readPHRawVoltage();
  float uncal = 7.0f + ((2.5f - v) / 0.18f);
  phOffset = 7.0f - uncal;
  saveSettings();
  oledClear(); display.setCursor(0, 0); display.println("pH Cal Saved"); display.setCursor(0, 16); display.print("Voltage: "); display.println(v, 3); display.setCursor(0, 30); display.print("Offset: "); display.println(phOffset, 2); display.display(); delay(2500);
}

void calibrateRespAuto() {
  oledClear(); display.setCursor(0, 0); display.println("Resp Auto Cal"); display.setCursor(0, 12); display.println("Wear belt normally"); display.setCursor(0, 24); display.println("Breathe for 10 sec"); display.display(); delay(1500);
  int mn = 4095, mx = 0; long sum = 0; int n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 10000UL) {
    int v = analogRead(RESP_PIN); mn = min(mn, v); mx = max(mx, v); sum += v; n++;
    oledClear(); display.setCursor(0, 0); display.println("Auto Cal Running"); display.setCursor(0, 16); display.print("Min:"); display.print(mn); display.print(" Max:"); display.println(mx); display.setCursor(0, 30); display.print("Now:"); display.println(v); display.display();
    delay(40);
  }
  int avg = n ? sum / n : 0;
  respThreshold = constrain(avg + (mx - avg) * 55 / 100, 100, 3900);
  saveSettings();
  oledClear(); display.setCursor(0, 0); display.println("Resp Cal Saved"); display.setCursor(0, 16); display.print("Min/Max "); display.print(mn); display.print("/"); display.println(mx); display.setCursor(0, 30); display.print("Threshold "); display.println(respThreshold); display.display(); delay(2500);
}

void adjustValueScreen(bool respMode) {
  while (true) {
    oledClear();
    display.setCursor(0, 0); display.println(respMode ? "Adjust Resp Thr" : "Adjust TDS Factor");
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
    display.setCursor(0, 18);
    if (respMode) { display.print("Threshold: "); display.println(respThreshold); display.setCursor(0, 32); display.print("Live ADC: "); display.println(analogRead(RESP_PIN)); }
    else { display.print("TDS factor: "); display.println(tdsFactor, 2); display.setCursor(0, 32); display.print("Live TDS: "); display.println(readTDS()); }
    display.setCursor(0, 56); display.println("UP/DN change SEL save");
    display.display();
    if (btnPressed(BTN_UP)) { if (respMode) respThreshold += 50; else tdsFactor += 0.01f; }
    if (btnPressed(BTN_DOWN)) { if (respMode) respThreshold -= 50; else tdsFactor -= 0.01f; }
    respThreshold = constrain(respThreshold, 100, 3900);
    tdsFactor = constrain(tdsFactor, 0.10f, 1.20f);
    if (btnPressed(BTN_SELECT)) { saveSettings(); break; }
    if (btnPressed(BTN_BACK)) break;
    delay(80);
  }
}

void rawSensorTest() {
  while (true) {
    int phADC = analogRead(PH_PIN);
    int tdsADC = analogRead(TDS_PIN);
    int respADC = analogRead(RESP_PIN);
    oledClear();
    display.setCursor(0, 0); display.println("Raw Sensor Test");
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
    display.setCursor(0, 13); display.print("Temp: "); display.println(readTemperature(), 1);
    display.print("pH ADC : "); display.println(phADC);
    display.print("TDS ADC: "); display.println(tdsADC);
    display.print("RespADC: "); display.println(respADC);
    display.setCursor(0, 56); display.println("BACK=Exit");
    display.display();
    if (btnPressed(BTN_BACK)) break;
    delay(500);
  }
}

void resetCalibration() {
  phOffset = 0.0f; tdsFactor = 0.50f; respThreshold = 2000; saveSettings();
  oledClear(); display.setCursor(0, 0); display.println("Calibration Reset"); display.display(); delay(1500);
}

void clearReadings() {
  haveTemp = havePH = haveTDS = haveResp = false;
  oledClear(); display.setCursor(0, 0); display.println("Readings Cleared"); display.display(); delay(1200);
}

void goToSleep() {
  oledClear(); display.setCursor(0, 0); display.println("Sleeping...");
  display.setCursor(0, 16); display.println("Press SELECT to wake");
  display.display(); delay(800);
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  digitalWrite(BUZZER_PIN, LOW);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_SELECT, 0); // wake on LOW
  esp_deep_sleep_start();
}

// ============================================================
// Setup and loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found. Check SDA/SCL/VCC/GND.");
    while (true) { digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); delay(900); }
  }

  ds18b20.begin();
  prefs.begin("adpm", false);
  loadSettings();
  lastActivity = millis();

  showSplash();
  while (!isPressed(BTN_UP) && !isPressed(BTN_DOWN) && !isPressed(BTN_SELECT) && !isPressed(BTN_BACK)) delay(20);
  delay(250);
}

void loop() {
  if (longBackPressed() && appState == 0) goToSleep();
  if (millis() - lastActivity > IDLE_SLEEP_MS) goToSleep();

  switch (appState) {
    case 0: // Sensor select menu
      showSensorMenu();
      if (btnPressed(BTN_UP)) { if (menuIndex > 0) { menuIndex--; if (menuIndex < menuOffset) menuOffset = menuIndex; } }
      if (btnPressed(BTN_DOWN)) { if (menuIndex < SENSOR_MENU_ITEMS - 1) { menuIndex++; if (menuIndex >= menuOffset + 4) menuOffset = menuIndex - 3; } }
      if (btnPressed(BTN_SELECT)) {
        if (menuIndex == VIEW_SUMMARY_INDEX) { printSummarySerial(); appState = 2; }
        else appState = 1;
      }
      if (btnPressed(BTN_BACK)) appState = 5;
      break;

    case 1: // Reading in progress for the chosen sensor
      runSingleReading(menuIndex);
      appState = 0; // back to sensor menu, showing the freshly captured value
      break;

    case 2: // Summary of all captured readings
      showSummary();
      if (btnPressed(BTN_SELECT) || btnPressed(BTN_BACK)) appState = 0;
      break;

    case 5: // Tools
      showTools();
      if (btnPressed(BTN_UP)) { if (toolIndex > 0) toolIndex--; }
      if (btnPressed(BTN_DOWN)) { if (toolIndex < 4) toolIndex++; }
      if (btnPressed(BTN_BACK)) appState = 0;
      if (btnPressed(BTN_SELECT)) {
        if (toolIndex == 0) appState = 6;
        if (toolIndex == 1) rawSensorTest();
        if (toolIndex == 2) resetCalibration();
        if (toolIndex == 3) clearReadings();
        if (toolIndex == 4) goToSleep();
      }
      break;

    case 6: // Calibration menu
      showCalMenu();
      if (btnPressed(BTN_UP)) { if (calIndex > 0) calIndex--; }
      if (btnPressed(BTN_DOWN)) { if (calIndex < 3) calIndex++; }
      if (btnPressed(BTN_BACK)) appState = 5;
      if (btnPressed(BTN_SELECT)) {
        if (calIndex == 0) calibratePH7();
        if (calIndex == 1) calibrateRespAuto();
        if (calIndex == 2) adjustValueScreen(true);
        if (calIndex == 3) adjustValueScreen(false);
      }
      break;
  }
  delay(35);
}
