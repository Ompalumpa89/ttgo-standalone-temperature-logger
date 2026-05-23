/*
  TTGO Standalone Temperature Logger
  Powerbank Version: 1.1.0

  For portable / battery / powerbank field use.
  Display turns off automatically after 60 seconds.

  Portable offline temperature logger for 1-2 DS18B20 sensors.
  Logs to microSD card in CSV format suitable for Norwegian Excel:
  semicolon separator and comma decimal separator.

  CSV file on SD card:
  /templogg.csv

  CSV columns:
  sekunder_siden_start;dager_siden_start;sensor_1_C;sensor_2_C;sensor_antall;logg_OK

  Main features:
  - Works offline without WiFi or internet
  - Supports one or two DS18B20 sensors on the same OneWire bus
  - Logs to microSD card every 30 seconds
  - Auto screen-off after 60 seconds for powerbank use
  - Built-in buttons:
      Button 1 / GPIO0  = wake screen / toggle screen
      Button 2 / GPIO35 = short press: force log now
      Button 2 / GPIO35 = long press: pause/resume logging for safer SD removal

  Important:
  - Do not hold GPIO0 button while powering up or resetting the ESP32.
  - Do not remove the SD card while logging is active.
*/

#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>

// --------------------------------------------------
// PROJECT INFO
// --------------------------------------------------

#define PROJECT_NAME "TTGO Standalone Temperature Logger"
#define PROJECT_VERSION "1.1.0 Powerbank"

// --------------------------------------------------
// PIN SETUP
// --------------------------------------------------

#define ONE_WIRE_BUS 27

#define SD_CS    13
#define SD_SCK   25
#define SD_MOSI  32
#define SD_MISO  33

// TTGO T-Display common button pins
#define BUTTON_SCREEN 0
#define BUTTON_ACTION 35

// TTGO T-Display common backlight pin.
// TFT_eSPI often defines TFT_BL. If not, GPIO4 is common on TTGO T-Display.
#ifndef TFT_BL
  #define TFT_BL 4
#endif

#define BACKLIGHT_ON  HIGH
#define BACKLIGHT_OFF LOW

// --------------------------------------------------
// OBJECTS
// --------------------------------------------------

TFT_eSPI tft = TFT_eSPI();

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

SPIClass sdSPI(HSPI);

// --------------------------------------------------
// SETTINGS
// --------------------------------------------------

const unsigned long LOG_INTERVAL = 30000;          // 30 seconds
const unsigned long SCREEN_INTERVAL = 1000;        // 1 second when screen is on
const unsigned long RESCAN_INTERVAL = 60000;       // 60 seconds
const unsigned long DISPLAY_TIMEOUT = 60000;       // screen turns off after 60 seconds
const unsigned long BUTTON_DEBOUNCE = 60;          // ms
const unsigned long LONG_PRESS_TIME = 1500;        // ms

unsigned long lastLogTime = 0;
unsigned long lastScreenTime = 0;
unsigned long lastRescanTime = 0;
unsigned long lastUserActivityTime = 0;

bool sdOK = false;
bool headerCreated = false;
bool screenDrawn = false;
bool displayOn = true;
bool loggingEnabled = true;

int sensorCount = 0;

float temp1 = DEVICE_DISCONNECTED_C;
float temp2 = DEVICE_DISCONNECTED_C;

DeviceAddress sensorAddress1;
DeviceAddress sensorAddress2;

bool sensor1OK = false;
bool sensor2OK = false;

// Button state
unsigned long buttonScreenPressedAt = 0;
unsigned long buttonActionPressedAt = 0;

bool buttonScreenWasPressed = false;
bool buttonActionWasPressed = false;

// --------------------------------------------------
// HELPERS
// --------------------------------------------------

void printAddress(DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

bool validTemp(float value) {
  if (value == DEVICE_DISCONNECTED_C) return false;
  if (value < -100.0) return false;
  if (value > 125.0) return false;
  return true;
}

String decimalComma(float value, int decimals) {
  String text = String(value, decimals);
  text.replace(".", ",");
  return text;
}

String tempToText(float value) {
  if (!validTemp(value)) {
    return "--,-";
  }

  return decimalComma(value, 1);
}

uint16_t tempColor(float value) {
  if (!validTemp(value)) {
    return TFT_DARKGREY;
  }

  if (value < 0.0) {
    return TFT_BLUE;
  }

  if (value < 25.0) {
    return TFT_GREEN;
  }

  if (value < 35.0) {
    return TFT_YELLOW;
  }

  if (value < 45.0) {
    return TFT_ORANGE;
  }

  return TFT_RED;
}

uint16_t textColorForBackground(float value) {
  if (!validTemp(value)) {
    return TFT_WHITE;
  }

  if (value < 0.0) {
    return TFT_WHITE;
  }

  if (value < 45.0) {
    return TFT_BLACK;
  }

  return TFT_WHITE;
}

float readSensorWithRetry(DeviceAddress address) {
  float value = DEVICE_DISCONNECTED_C;

  for (int attempt = 0; attempt < 3; attempt++) {
    ds18b20.requestTemperaturesByAddress(address);
    delay(150);

    value = ds18b20.getTempC(address);

    if (validTemp(value)) {
      return value;
    }

    delay(100);
  }

  return DEVICE_DISCONNECTED_C;
}

// --------------------------------------------------
// POWER MANAGEMENT
// --------------------------------------------------

void disableUnusedRadios() {
  WiFi.mode(WIFI_OFF);
  btStop();

  // Lower CPU frequency to reduce power use.
  // 80 MHz is enough for this logger.
  setCpuFrequencyMhz(80);
}

void setDisplayPower(bool on) {
  displayOn = on;

  if (displayOn) {
    digitalWrite(TFT_BL, BACKLIGHT_ON);
    screenDrawn = false;
    lastUserActivityTime = millis();
    updateScreen();
    Serial.println("DISPLAY: ON");
  } else {
    digitalWrite(TFT_BL, BACKLIGHT_OFF);
    Serial.println("DISPLAY: OFF");
  }
}

void wakeDisplay() {
  if (!displayOn) {
    setDisplayPower(true);
  } else {
    lastUserActivityTime = millis();
  }
}

void toggleDisplay() {
  setDisplayPower(!displayOn);
}

void handleDisplayTimeout() {
  if (displayOn && (millis() - lastUserActivityTime >= DISPLAY_TIMEOUT)) {
    setDisplayPower(false);
  }
}

// --------------------------------------------------
// SENSORS
// --------------------------------------------------

void scanSensors() {
  sensor1OK = false;
  sensor2OK = false;

  ds18b20.begin();

  sensorCount = ds18b20.getDeviceCount();

  Serial.println();
  Serial.println("---------- SENSOR SCAN ----------");
  Serial.print("Sensors found: ");
  Serial.println(sensorCount);

  if (sensorCount >= 1) {
    if (ds18b20.getAddress(sensorAddress1, 0)) {
      sensor1OK = true;
      Serial.print("Sensor 1 address: ");
      printAddress(sensorAddress1);
      Serial.println();
    } else {
      Serial.println("Sensor 1 address not found");
    }
  }

  if (sensorCount >= 2) {
    if (ds18b20.getAddress(sensorAddress2, 1)) {
      sensor2OK = true;
      Serial.print("Sensor 2 address: ");
      printAddress(sensorAddress2);
      Serial.println();
    } else {
      Serial.println("Sensor 2 address not found");
    }
  }

  if (sensorCount > 2) {
    Serial.println("Warning: More than 2 sensors found. Only first 2 are used.");
  }

  Serial.println("---------------------------------");
}

void readTemperatures() {
  if (sensor1OK) {
    temp1 = readSensorWithRetry(sensorAddress1);
  } else {
    temp1 = DEVICE_DISCONNECTED_C;
  }

  if (sensor2OK) {
    temp2 = readSensorWithRetry(sensorAddress2);
  } else {
    temp2 = DEVICE_DISCONNECTED_C;
  }

  Serial.print("TEMP | Sensors: ");
  Serial.print(sensorCount);

  Serial.print(" | T1: ");
  if (validTemp(temp1)) {
    Serial.print(decimalComma(temp1, 2));
    Serial.print(" C");
  } else {
    Serial.print("ERROR");
  }

  Serial.print(" | T2: ");
  if (validTemp(temp2)) {
    Serial.print(decimalComma(temp2, 2));
    Serial.print(" C");
  } else {
    Serial.print("ERROR");
  }

  Serial.print(" | SD: ");
  Serial.print(sdOK ? "OK" : "FAIL");

  Serial.print(" | LOG: ");
  Serial.print(loggingEnabled ? "ON" : "PAUSED");

  Serial.print(" | DISPLAY: ");
  Serial.println(displayOn ? "ON" : "OFF");
}

// --------------------------------------------------
// SD CARD
// --------------------------------------------------

bool testSDWrite() {
  fs::File testFile = SD.open("/sd_test.tmp", FILE_WRITE);

  if (!testFile) {
    return false;
  }

  testFile.println("test");
  testFile.flush();
  testFile.close();

  if (!SD.exists("/sd_test.tmp")) {
    return false;
  }

  SD.remove("/sd_test.tmp");

  return true;
}

void initSDCard() {
  Serial.println();
  Serial.println("========== SD INIT ==========");
  Serial.println("Starting SD card...");
  Serial.print("CS: ");
  Serial.println(SD_CS);
  Serial.print("SCK: ");
  Serial.println(SD_SCK);
  Serial.print("MOSI: ");
  Serial.println(SD_MOSI);
  Serial.print("MISO: ");
  Serial.println(SD_MISO);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (SD.begin(SD_CS, sdSPI, 4000000)) {
    uint8_t cardType = SD.cardType();

    if (cardType == CARD_NONE) {
      sdOK = false;
      headerCreated = false;
      Serial.println("SD card: FAIL - no card detected");
      Serial.println("=============================");
      return;
    }

    if (!testSDWrite()) {
      sdOK = false;
      headerCreated = false;
      Serial.println("SD card: FAIL - write test failed");
      Serial.println("=============================");
      return;
    }

    sdOK = true;

    Serial.println("SD card: OK");

    Serial.print("Card type: ");

    if (cardType == CARD_MMC) {
      Serial.println("MMC");
    } else if (cardType == CARD_SD) {
      Serial.println("SDSC");
    } else if (cardType == CARD_SDHC) {
      Serial.println("SDHC");
    } else {
      Serial.println("Unknown");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    uint64_t totalBytes = SD.totalBytes() / (1024 * 1024);
    uint64_t usedBytes = SD.usedBytes() / (1024 * 1024);

    Serial.print("Card size: ");
    Serial.print(cardSize);
    Serial.println(" MB");

    Serial.print("Total space: ");
    Serial.print(totalBytes);
    Serial.println(" MB");

    Serial.print("Used space: ");
    Serial.print(usedBytes);
    Serial.println(" MB");

  } else {
    sdOK = false;
    headerCreated = false;
    Serial.println("SD card: FAIL");
    Serial.println("Check: VCC, GND, CS, SCK, MOSI, MISO and FAT32 format.");
  }

  Serial.println("=============================");
}

bool checkSDCardBeforeLog() {
  if (!SD.begin(SD_CS, sdSPI, 4000000)) {
    if (sdOK) {
      Serial.println("SD STATUS: FAIL - SD card not available");
    }

    sdOK = false;
    headerCreated = false;
    return false;
  }

  if (SD.cardType() == CARD_NONE) {
    if (sdOK) {
      Serial.println("SD STATUS: FAIL - no card detected");
    }

    sdOK = false;
    headerCreated = false;
    return false;
  }

  if (!sdOK) {
    Serial.println("SD STATUS: OK - SD card found again");
  }

  sdOK = true;
  return true;
}

void createLogFileHeader() {
  if (!sdOK) {
    Serial.println("Header not created: SD not OK");
    return;
  }

  Serial.println("Checking log file: /templogg.csv");

  if (!SD.exists("/templogg.csv")) {
    Serial.println("Log file does not exist. Creating new file.");

    fs::File file = SD.open("/templogg.csv", FILE_WRITE);

    if (file) {
      file.println("sekunder_siden_start;dager_siden_start;sensor_1_C;sensor_2_C;sensor_antall;logg_OK");
      file.flush();
      file.close();

      headerCreated = true;
      Serial.println("Header created OK");
    } else {
      headerCreated = false;
      sdOK = false;
      Serial.println("ERROR: Could not create /templogg.csv");
    }
  } else {
    headerCreated = true;
    Serial.println("Log file already exists.");
  }
}

void logTemperatures() {
  Serial.println();
  Serial.println("---------- LOGGING ----------");

  if (!loggingEnabled) {
    Serial.println("Logging paused. SD card can be removed after power-off.");
    Serial.println("-----------------------------");
    return;
  }

  if (!checkSDCardBeforeLog()) {
    Serial.println("Logging stopped: SD not OK");
    Serial.println("-----------------------------");
    return;
  }

  if (!headerCreated) {
    createLogFileHeader();
  }

  fs::File file = SD.open("/templogg.csv", FILE_APPEND);

  if (!file) {
    Serial.println("ERROR: Could not open /templogg.csv");
    sdOK = false;
    headerCreated = false;
    Serial.println("-----------------------------");
    return;
  }

  unsigned long secondsSinceStart = millis() / 1000;
  float daysSinceStart = secondsSinceStart / 86400.0;

  file.print(secondsSinceStart);
  file.print(";");

  file.print(decimalComma(daysSinceStart, 6));
  file.print(";");

  if (validTemp(temp1)) {
    file.print(decimalComma(temp1, 2));
  }

  file.print(";");

  if (validTemp(temp2)) {
    file.print(decimalComma(temp2, 2));
  }

  file.print(";");
  file.print(sensorCount);
  file.print(";");
  file.println("OK");

  file.flush();
  file.close();

  Serial.print("Logged: ");
  Serial.print(secondsSinceStart);
  Serial.print(" sec | ");
  Serial.print(decimalComma(daysSinceStart, 6));
  Serial.print(" days | T1: ");

  if (validTemp(temp1)) {
    Serial.print(decimalComma(temp1, 2));
    Serial.print(" C");
  } else {
    Serial.print("ERROR");
  }

  Serial.print(" | T2: ");

  if (validTemp(temp2)) {
    Serial.print(decimalComma(temp2, 2));
    Serial.print(" C");
  } else {
    Serial.print("ERROR");
  }

  Serial.println();
  Serial.println("Written to: /templogg.csv");
  Serial.println("-----------------------------");
}

// --------------------------------------------------
// BUTTONS
// --------------------------------------------------

bool readButtonPressed(int pin) {
  return digitalRead(pin) == LOW;
}

void handleButtons() {
  unsigned long now = millis();

  bool screenPressed = readButtonPressed(BUTTON_SCREEN);
  bool actionPressed = readButtonPressed(BUTTON_ACTION);

  // Button 1: GPIO0 - screen wake/toggle
  if (screenPressed && !buttonScreenWasPressed) {
    buttonScreenWasPressed = true;
    buttonScreenPressedAt = now;
  }

  if (!screenPressed && buttonScreenWasPressed) {
    unsigned long pressTime = now - buttonScreenPressedAt;
    buttonScreenWasPressed = false;

    if (pressTime > BUTTON_DEBOUNCE) {
      toggleDisplay();
      lastUserActivityTime = now;
    }
  }

  // Button 2: GPIO35 - short press force log, long press pause/resume logging
  if (actionPressed && !buttonActionWasPressed) {
    buttonActionWasPressed = true;
    buttonActionPressedAt = now;
  }

  if (!actionPressed && buttonActionWasPressed) {
    unsigned long pressTime = now - buttonActionPressedAt;
    buttonActionWasPressed = false;

    if (pressTime > BUTTON_DEBOUNCE) {
      wakeDisplay();

      if (pressTime >= LONG_PRESS_TIME) {
        loggingEnabled = !loggingEnabled;

        Serial.print("LOGGING: ");
        Serial.println(loggingEnabled ? "RESUMED" : "PAUSED");

        if (displayOn) {
          updateScreen();
        }
      } else {
        readTemperatures();
        logTemperatures();
        lastLogTime = now;

        if (displayOn) {
          updateScreen();
        }
      }
    }
  }
}

// --------------------------------------------------
// DISPLAY
// --------------------------------------------------

void drawStaticScreen() {
  if (!displayOn) return;

  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TL_DATUM);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TEMP LOGGER", 8, 6);

  tft.setTextSize(1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("v1.1.0 / Powerbank", 8, 28);

  tft.drawRoundRect(8, 45, 108, 82, 8, TFT_DARKGREY);
  tft.drawRoundRect(124, 45, 108, 82, 8, TFT_DARKGREY);

  screenDrawn = true;
}

void updateSensorBox(int x, int y, String title, float value, bool active) {
  if (!displayOn) return;

  uint16_t bgColor;
  uint16_t fgColor;

  if (!active || !validTemp(value)) {
    bgColor = TFT_DARKGREY;
    fgColor = TFT_WHITE;
  } else {
    bgColor = tempColor(value);
    fgColor = textColorForBackground(value);
  }

  tft.fillRoundRect(x, y, 108, 82, 8, bgColor);
  tft.drawRoundRect(x, y, 108, 82, 8, TFT_WHITE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
  tft.setTextColor(fgColor, bgColor);
  tft.drawString(title, x + 10, y + 9);

  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(3);
  tft.setTextColor(fgColor, bgColor);
  tft.drawString(tempToText(value), x + 54, y + 45);

  tft.setTextSize(1);
  tft.drawString("C", x + 54, y + 68);
}

void updateStatusArea() {
  if (!displayOn) return;

  tft.fillRect(0, 132, 240, 50, TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Sensors: " + String(sensorCount), 8, 138);

  if (sdOK) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("SD: OK", 8, 153);
  } else {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD: FAIL", 8, 153);
  }

  unsigned long secondsSinceStart = millis() / 1000;
  unsigned long nextLog = 0;

  if (millis() - lastLogTime < LOG_INTERVAL) {
    nextLog = (LOG_INTERVAL - (millis() - lastLogTime)) / 1000;
  }

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Uptime: " + String(secondsSinceStart) + " sec", 95, 138);

  if (loggingEnabled) {
    tft.drawString("Next log: " + String(nextLog) + " sec", 95, 153);
  } else {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Logging paused", 95, 153);
  }

  if (sensorCount == 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("No sensors detected", 8, 170);
  } else if (sensorCount == 1) {
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("One sensor active", 8, 170);
  } else if (!validTemp(temp1) || !validTemp(temp2)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Sensor read error", 8, 170);
  } else if (!sdOK) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("SD missing / write error", 8, 170);
  } else if (!loggingEnabled) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("Hold B to resume logging", 8, 170);
  } else {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("System running", 8, 170);
  }
}

void updateScreen() {
  if (!displayOn) return;

  if (!screenDrawn) {
    drawStaticScreen();
  }

  updateSensorBox(8, 45, "SENSOR 1", temp1, sensor1OK);
  updateSensorBox(124, 45, "SENSOR 2", temp2, sensor2OK);

  updateStatusArea();
}

// --------------------------------------------------
// POWER MANAGEMENT
// --------------------------------------------------

void disableUnusedRadios() {
  WiFi.mode(WIFI_OFF);
  btStop();

  // Lower CPU frequency to reduce power use.
  // 80 MHz is enough for this logger.
  setCpuFrequencyMhz(80);
}

void setDisplayPower(bool on) {
  displayOn = on;

  if (displayOn) {
    digitalWrite(TFT_BL, BACKLIGHT_ON);
    screenDrawn = false;
    lastUserActivityTime = millis();
    updateScreen();
    Serial.println("DISPLAY: ON");
  } else {
    digitalWrite(TFT_BL, BACKLIGHT_OFF);
    Serial.println("DISPLAY: OFF");
  }
}

void wakeDisplay() {
  if (!displayOn) {
    setDisplayPower(true);
  } else {
    lastUserActivityTime = millis();
  }
}

void toggleDisplay() {
  setDisplayPower(!displayOn);
}

void handleDisplayTimeout() {
  if (displayOn && (millis() - lastUserActivityTime >= DISPLAY_TIMEOUT)) {
    setDisplayPower(false);
  }
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(1000);

  disableUnusedRadios();

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, BACKLIGHT_ON);

  pinMode(BUTTON_SCREEN, INPUT_PULLUP);
  pinMode(BUTTON_ACTION, INPUT_PULLUP);

  Serial.println();
  Serial.println("=================================");
  Serial.println(PROJECT_NAME);
  Serial.print("Version: ");
  Serial.println(PROJECT_VERSION);
  Serial.println("TTGO / ESP32 + DS18B20 + SD");
  Serial.println("Serial Monitor: 115200 baud");
  Serial.println("CSV format: Norwegian Excel with semicolon and decimal comma");
  Serial.println("Power mode: WiFi off, Bluetooth off, CPU 80 MHz, screen auto-off");
  Serial.println("Button 1 GPIO0: toggle display");
  Serial.println("Button 2 GPIO35: short press = force log, long press = pause/resume logging");
  Serial.println("=================================");

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Starting...", 120, 67);

  pinMode(ONE_WIRE_BUS, INPUT_PULLUP);

  ds18b20.begin();

  ds18b20.setResolution(10);
  ds18b20.setWaitForConversion(true);
  ds18b20.setCheckForConversion(true);

  scanSensors();

  initSDCard();
  createLogFileHeader();

  readTemperatures();
  logTemperatures();

  lastLogTime = millis();
  lastScreenTime = millis();
  lastRescanTime = millis();
  lastUserActivityTime = millis();

  drawStaticScreen();
  updateScreen();
}

// --------------------------------------------------
// LOOP
// --------------------------------------------------

void loop() {
  unsigned long now = millis();

  handleButtons();
  handleDisplayTimeout();

  if (now - lastRescanTime >= RESCAN_INTERVAL) {
    lastRescanTime = now;
    scanSensors();
  }

  if (displayOn && now - lastScreenTime >= SCREEN_INTERVAL) {
    lastScreenTime = now;
    readTemperatures();
    updateScreen();
  }

  if (now - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = now;
    readTemperatures();
    logTemperatures();

    if (displayOn) {
      updateScreen();
    }
  }
}
