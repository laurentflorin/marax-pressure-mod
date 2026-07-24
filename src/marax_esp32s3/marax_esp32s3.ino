#include <Arduino.h>
#include <EasyNextionLibrary.h>

// DISABLE WiFi to prevent radio conflicts with BLE
// ESP32-S3 has only ONE 2.4GHz radio shared between WiFi and BLE
// Disabling WiFi gives BLE full control for reliable scale connection
// #define DISABLE_WIFI_MQTT

// ─────────────────────────────────────────────────────────────────────────
// OTA (Over-The-Air firmware updates over WiFi)
// Comment out ENABLE_OTA to build without OTA.
//
// WARNING: OTA requires WiFi. The ESP32-S3 shares ONE 2.4GHz radio between
// WiFi and BLE, so bringing WiFi up for OTA can reduce BLE scale reliability
// (this is the very reason DISABLE_WIFI_MQTT exists). The OTA code here is
// non-blocking and never stalls normal operation. Set wifi_ssid /
// wifi_password (below) before using OTA.
// Flashing: Arduino IDE → Tools → Port → network port "MaraXController at <ip>".
// ─────────────────────────────────────────────────────────────────────────
#define ENABLE_OTA

#include <WiFi.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <driver/uart.h>
#include <NimBLEDevice.h>
#include "FelicitaScale_NimBLE.h"

#ifdef ENABLE_OTA
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#endif

// === Required Libraries ===
// - EasyNextionLibrary     (Arduino Library Manager)
// - WiFi                   (built-in with ESP32 Arduino core)
// - PubSubClient           (Arduino Library Manager)
// - SD                     (Arduino built-in)
// - NimBLE-Arduino         (Arduino Library Manager) - REQUIRED for ESP32-S3 BLE
// NOTE: ArduinoBLE and AcaiaArduinoBLE are NOT compatible with ESP32-S3!
//       Using NimBLE-based implementation instead (FelicitaScale_NimBLE.h)
//
// Arduino IDE board: "ESP32S3 Dev Module"
//   Board package:   esp32 by Espressif Systems (Boards Manager)
//   Flash size:      16MB (128Mb)
//   PSRAM:           OPI PSRAM
//   USB Mode:        Hardware CDC and JTAG

// ═══════════════════════════════════════════════════════════════════════════
// Board: ESP32-S3-DEV-KIT-N16R8-M (Waveshare)
//
// The Mara X GiCar board outputs INVERTED UART (idle LOW, like RS-232).
// On ESP32-S3 this is fixed in SOFTWARE via uart_set_line_inverse().
// No hardware inverter or transistor required!
//
// IMPORTANT: Physical wiring - ESP32 RX connects to MaraX TX, and vice versa:
//   - ESP32 GPIO16 (RX) ← physically wire to → MaraX TX pin
//   - ESP32 GPIO17 (TX) ← physically wire to → MaraX RX pin
//
// Pin Assignments:
//   GPIO16  ← MaraX TX   (UART1 RX, inverted in software)
//   GPIO17  → MaraX RX   (UART1 TX)
//   GPIO18  ← Nextion TX (UART2 RX)
//   GPIO8   → Nextion RX (UART2 TX)
//   GPIO3   ← Pressure sensor output (ADC1_CH2)
//              IMPORTANT: Must use ADC1 (GPIO1–10). ADC2 (GPIO11–20)
//              cannot be used for analog reads when WiFi is active!
//   GPIO4   ← AC mains zero-cross detection (sync for dimmable_light)
//   GPIO5   → Pump TRIAC gate (via optocoupler)
//   GPIO6   ← Brew lever switch input
//   GPIO7   → GiCar brew relay output
//   GPIO10  → SD card CS
//   GPIO11  → SD card MOSI (SPI2 / FSPI)
//   GPIO12  → SD card SCK
//   GPIO13  ← SD card MISO
//
// Machine Data Format: CSV
//   Example frame: +1.10,023,,0,022,0000,0,0
//   Field 0: Version/Mode (+1.10)
//   Field 1: Brew temperature (023 = 23°C)
//   Field 2: (empty)
//   Field 3: Mode indicator
//   Field 4: Steam temperature (022 = 22°C)
//   Field 5: Fast heat countdown (0000)
//   Field 6: Heating element status (0 = off, 1 = on)
//   Field 7: Unknown
// ═══════════════════════════════════════════════════════════════════════════

#define MARAX_RX_PIN         16
#define MARAX_TX_PIN         17
#define NEXTION_RX_PIN       18
#define NEXTION_TX_PIN        8
#define PRESSURE_SENSOR_PIN   3   // ADC1_CH2 — safe to use with WiFi
#define AC_ZERO_CROSS_PIN     4
#define PUMP_PIN              5
#define BREW_SWITCH_PIN       6
#define BREW_RELAY_PIN        7
#define SD_CS_PIN            10
#define SD_MOSI_PIN          11
#define SD_SCK_PIN           12
#define SD_MISO_PIN          13

// Brew input/relay electrical behavior:
// - BREW_SWITCH_ACTIVE_LOW=1 means GPIO reads LOW when lever is ON (default).
// - BREW_RELAY_ACTIVE_HIGH=1 means GPIO HIGH energizes relay (default).
// If your relay board is low-level-trigger, set BREW_RELAY_ACTIVE_HIGH to 0.
#define BREW_SWITCH_ACTIVE_LOW 1
#define BREW_RELAY_ACTIVE_HIGH 1
#define BREW_SWITCH_DEBOUNCE_MS 35

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Wifi and Bluetooth
// nunununununununununununununununununununununununununununununun
#define wifi_ssid ""
#define wifi_password ""
// Scale MAC address - empty string enables auto-discovery of any Felicita scale
#define SCALE_MAC_ADDRESS ""

WiFiClient wifiClient;
bool wifiConnected = false;
bool scaleConnected = false;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Timers
// nunununununununununununununununununununununununununununununun
long serialMaraxUpdateMillis = millis();
long updateMqttTimer = millis();
long readSettigsRefreshTimer = millis();
unsigned long pageRefreshTimer = millis();
unsigned long refresh_timer = millis();
unsigned long activeBrewingStart = millis();
unsigned long lastWifiReconnectAttemptMs = 0;
unsigned long lastMqttReconnectAttemptMs = 0;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000;  // Try every 10 seconds
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;   // Try every 5 seconds

bool POWER_ON = false;

// Nextion display on UART2 (Serial2)
// Note: we initialize Serial2 directly in setup() with explicit pins.
// myNex.begin() is intentionally NOT called — doing so would re-init Serial2
// with default pins, overriding our custom NEXTION_RX/TX pin assignment.
EasyNex myNex(Serial2);

// Forward declaration required because PubSubClient constructor references it
void callbackfun(char *topic, byte *payload, unsigned int length);
void showProfileSelection();
void populateProfileList();
void handleProfileSelection(int rowIndex);
bool selectProfile(int profileIndex);
void handleNextionProfileTouchEvents();
void loadPersistedProfileSelection();
void persistSelectedProfile();
void loadPersistedTargetWeight();
void persistTargetWeight();
void updateTargetWeightUi(bool force);
void updateProfileSelectionHighlight();
bool loadProfile(const char *filename);
void loadBeta();
void loadObservations();
void updateProfileModeText();
void scanProfiles();
bool readRawBrewSwitchOn();
bool readDebouncedBrewSwitchOn();
void writeBrewRelay(bool brewOn);
void resumeIdleBoilerFill();

// nunununununununununununununununununununununununununununununun
// nunununununununununununu MQTT Settings
// nunununununununununununununununununununununununununununununun
#define mqtt_server ""
#define mqtt_user ""
#define mqtt_password ""

#define brewtemp_topic           "marax/sensor/brewtemp"
#define steamtemp_topic          "marax/sensor/steamtemp"
#define steamtargettemp_topic    "marax/sensor/steamtargettemp"
#define fastheat_topic           "marax/sensor/fastheat_timer"
#define heatingElement_topic     "marax/sensor/heatingelement"
#define debug_topic              "marax/sensor/debug"
#define shots_topic              "marax/sensor/shots"
#define power_topic              "marax/sensor/power_state"

PubSubClient mqttClient(mqtt_server, 1883, callbackfun, wifiClient);

// nunununununununununununununununununununununununununununununun
// nunununununununununununu CONSTS
// nunununununununununununununununununununununununununununununun
#define REFRESH_SCREEN_EVERY 150 // Screen refresh interval (ms)
#define PUMP_RANGE 255;

const int REFRESH_TIME = 100;

bool brewActive = false;
bool brewTimerActive = false; // active if brewing or descaling

// nunununununununununununununununununununununununununununununun
// nunununununununununununu MaraxSerialStuff
// nunununununununununununununununununununununununununununununun
const byte numCharsSerialMarax = 32;
char receivedCharsFromMarax[numCharsSerialMarax];
static byte ndx = 0;
char endMarker = '\r'; // Mara X terminates frames with CR (\r), not LF (\n)
char rc;

// Serial Marax Sensors
int brewTemp = 0;
int steamTemp = 0;
int steamTargetTemp = 0;
int maraxMode = 0;
int fastHeatingCountDown = 0;
bool heatingElementOn = false;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Pins
// nunununununununununununununununununununununununununununununun
int brewSwitchRelayPin = BREW_RELAY_PIN;
int brewSwitchPin = BREW_SWITCH_PIN;

float i = 0;
float V, P, B;
uint32_t number = 0;
int inPin = 2;
int pinOutput = 0;

int shotCount = 0;

float voltage;
float bar;
uint32_t barGraphValue;
int analog = 0;
float pressure;

bool pressureProfilingEnabled = false;
bool remoteProfilingEnabled = false;

bool cleaningModeActive = 0;
bool cleaningRunActive = 0;
int cleaningShots = 0;
int cleaningShotsWater = 0;

int brewSwitchAnalogValue = 0;
bool brewSwitchStableOn = false;
bool brewSwitchLastRawOn = false;
bool brewSwitchDebounceInitialized = false;
unsigned long brewSwitchLastChangeMs = 0;

uint32_t currentPageId;
int lastPageId;
bool displayIsInSleep = true;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Pressure Profile
// nunununununununununununununununununununununununununununununun
int t1p = 0, t1t = 0, t2p = 0, t2t = 0, t3p = 0, t3t = 0, t4p = 0, t4t = 0;
int t1pWave = 0, t2pWave = 0, t3pWave = 0, t4pWave = 0;

// SD / Profile globals
#define MAX_PROFILES 64
#define PROFILE_NAME_MAX_LEN 32
#define MAX_PATH_LEN 64
#define PROFILE_FIELD_COUNT 10
#define PROFILE_CSV_BUFFER_LEN 128
#define PROFILE_ROWS_PER_PAGE 10
#define PROFILE_TOUCH_COMPONENT_ID_MIN 5
#define PROFILE_TOUCH_COMPONENT_ID_MAX 14
// Fallback page ID for `profile1` (matches page order in MaraxDisplayFile.HMI).
#define PROFILE_PAGE_ID_FALLBACK 4
#define WEIGHT_STABLE_WINDOW_MS 2000  // how long weight must be stable before committing observation
#define WEIGHT_STABLE_THRESHOLD 0.5f  // max increase (g) over stability window
#define OBSERVATION_MAX_WAIT_MS 5000 // max time to wait for stability after brew stop
#define MIN_VALID_FINAL_WEIGHT 5.0f
#define MAX_VALID_EXTRA_WEIGHT 15.0f
#define OLS_SINGULARITY_THRESHOLD 1e-10
#define OLS_MIN_OBSERVATIONS 5
char activeProfileName[PROFILE_NAME_MAX_LEN] = "default";
char activeProfileFileStem[PROFILE_NAME_MAX_LEN] = "default";
char profileNames[MAX_PROFILES][PROFILE_NAME_MAX_LEN];
int profileCount = 0;
String profileListStr = "";
int selectedProfileIndex = 0;
int profileListPageStart = 0;
int profileSelectionPageId = PROFILE_PAGE_ID_FALLBACK;
bool profilePageNeedsPopulate = true;
unsigned long lastProfileScanMs = 0;
const unsigned long PROFILE_SCAN_INTERVAL_MS = 30000;
float targetWeight = 36.0f;
bool sdReady = false;
char persistedProfileFile[PROFILE_NAME_MAX_LEN] = "";
Preferences preferences;

FelicitaScale_NimBLE scale(false);  // true = enable debug to see BLE scan results
float currentWeight = 0.0f;
float prevWeight = 0.0f;
unsigned long lastWeightTime = 0;
float flowRate = 0.0f;
int lastScaleConnectedUi = -1; // -1 forces first UI update
unsigned long lastScaleReconnectAttemptMs = 0;
const unsigned long SCALE_RECONNECT_INTERVAL_MS = 30000;  // 30 seconds between scan attempts

#define OLS_WINDOW 30
#define GLOBAL_BETA_PATH "/models/global_beta.csv"
#define GLOBAL_DATA_PATH "/models/global_data.csv"
const float DEFAULT_OLS_BETA[5] = {0.0f, 0.6f, 0.0f, 0.0f, 0.0f};
float olsBeta[5] = {DEFAULT_OLS_BETA[0], DEFAULT_OLS_BETA[1], DEFAULT_OLS_BETA[2], DEFAULT_OLS_BETA[3], DEFAULT_OLS_BETA[4]};
float olsX[OLS_WINDOW][2];
float olsY[OLS_WINDOW];
int olsCount = 0;
int olsWriteIndex = 0;

bool pendingObservation = false;
unsigned long pendingObsTime = 0;
float pendingFlowAtStop = 0.0f;
float pendingPressAtStop = 0.0f;
float pendingWeightAtStop = 0.0f;
float pendingStabilityCheckWeight = 0.0f;
unsigned long pendingStabilityCheckTime = 0;
int pendingBrewTimeS = 0;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu LowPassFilterStuff
// nunununununununununununununununununununununununununununununun
const float alpha = 0.97; // Low Pass Filter alpha (0 - 1)
float filteredVal = 2048.0; // midpoint of 12-bit ADC (0–4095)
float sensorVal;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu AC Dimming (Zero-Cross Phase Control)
// nunununununununununununununununununununununununununununununun
//
// NOTE: No dedicated AC dimmer library is used here because the available
// libraries (RBDdimmer, dimmable-light-arduino) conflict with ESP32-S3's
// hardware timer API changes in Arduino core v3.x.  The hand-rolled
// zero-cross + one-shot timer approach below is equivalent and confirmed
// to compile on ESP32-S3 core 3.x.
//
// How it works:
//   1. zeroCrossISR fires on every AC zero-crossing (100 times/sec at 50 Hz).
//   2. It starts a one-shot hardware timer whose period is calculated from
//      pumpBrightness: high brightness → short delay → TRIAC fires early in
//      the half-cycle → more power delivered.
//   3. triacTriggerISR fires when the timer expires and pulses the TRIAC gate.
//
// Brightness mapping (pumpBrightness 0–255):
//   0   → pump OFF  (ISR returns immediately, no TRIAC trigger)
//   255 → FULL power (TRIAC triggered immediately at zero-cross)
//   1–254 → delay = map(brightness, 0, 255, 10000µs, 100µs)
//            i.e. brightness=128 → ~5050µs delay → ~50% power
//
// Idle pressure threshold above which the boiler is considered full.
// It switches dynamically: 5.0 bar when steam is hot (>80°C), otherwise 0.5 bar.
float boilerFullPressureBar = 0.5f;

// Dimmer debug counters (global so brewDetect can seed them at brew start)
unsigned long lastDimmerDebugMs = 0;
uint32_t lastZeroCrossCountDebug = 0;
// Latch: once pressure cuts the pump in idle, stay off until next brew ends
bool boilerFullLatch = false;
// Latch: once the predicted weight reaches target during a profiled brew,
// keep the pump off until the brew ends (lever released)
bool targetWeightReached = false;
volatile uint8_t  pumpBrightness = 255;  // 0-255, 255 = full power (no dimming)
volatile uint32_t zeroCrossCount = 0;    // Incremented in ISR, read in debug
hw_timer_t *acTimer = NULL;
portMUX_TYPE acTimerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR zeroCrossISR() {
  zeroCrossCount++;  // Count every half-cycle (debug: expect 100/s at 50 Hz)

  // Zero-cross detected — start timer for TRIAC trigger delay
  if (pumpBrightness == 0) {
    return;  // Off, don't trigger
  }
  
  if (pumpBrightness == 255) {
    // Full power - trigger immediately with a pulse
    digitalWrite(PUMP_PIN, HIGH);
    delayMicroseconds(100);  // 100µs gate pulse — matches triacTriggerISR
    digitalWrite(PUMP_PIN, LOW);
    return;
  }
  
  // Calculate delay: 0 = ~10ms (full), 255 = ~0μs (off for AC dimming logic)
  // Switzerland: 50Hz AC mains → half-cycle = 10000μs
  uint32_t delayUs = map(pumpBrightness, 0, 255, 10000, 100);
  
  portENTER_CRITICAL_ISR(&acTimerMux);
  timerRestart(acTimer);
  timerAlarm(acTimer, delayUs, false, 0);  // One-shot timer
  portEXIT_CRITICAL_ISR(&acTimerMux);
}

void IRAM_ATTR triacTriggerISR() {
  digitalWrite(PUMP_PIN, HIGH);  // Trigger TRIAC
  delayMicroseconds(100);        // 100µs pulse — minimum for reliable MOC3021/BT136 triggering
  digitalWrite(PUMP_PIN, LOW);
}

void setPumpBrightness(uint8_t brightness) {
  portENTER_CRITICAL(&acTimerMux);
  uint8_t prev = pumpBrightness;
  pumpBrightness = brightness;
  portEXIT_CRITICAL(&acTimerMux);

  // Log every change so we can trace what is sent to the dimmer
  if (brightness != prev) {
    uint32_t delayUs = 0;
    if (brightness > 0 && brightness < 255)
      delayUs = map(brightness, 0, 255, 10000, 100);

    Serial.print("[DIMMER] ");
    Serial.print(prev); Serial.print(" → "); Serial.print(brightness);
    Serial.print("/255  (");
    Serial.print(brightness * 100 / 255);
    Serial.print("%)");
    if (brightness == 0)        Serial.print("  → PUMP OFF (no TRIAC trigger)");
    else if (brightness == 255) Serial.print("  → FULL POWER (immediate trigger)");
    else { Serial.print("  → TRIAC delay "); Serial.print(delayUs); Serial.print("µs"); }
    Serial.println();
  }
}

void resumeIdleBoilerFill()
{
  boilerFullLatch = false;
  setPumpBrightness(255);
}

void primePressureFilter()
{
  sensorVal = (float)analogRead(PRESSURE_SENSOR_PIN);
  filteredVal = sensorVal;
  voltage = (filteredVal / 4096.0f) * 3.3f;
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu OTA (Over-The-Air Updates)
// nunununununununununununununununununununununununununununununun
#ifdef ENABLE_OTA
// OTA needs WiFi. In this build WiFi may otherwise be disabled for BLE, so the
// helpers below bring WiFi up themselves (non-blocking) and start the OTA
// service lazily once an IP is obtained. Normal operation never blocks on OTA.
bool otaInitialized = false;
unsigned long lastOtaWifiAttemptMs = 0;
const unsigned long OTA_WIFI_RETRY_INTERVAL_MS = 10000;  // background reconnect cadence

void setupOTA()
{
#ifdef DISABLE_WIFI_MQTT
  // WiFi is not started anywhere else in this build — OTA must bring it up.
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("MaraXController");
  WiFi.begin(wifi_ssid, wifi_password);
  lastOtaWifiAttemptMs = millis();
#endif

  ArduinoOTA.setHostname("MaraXController");
  // Optional: require a password to push updates (recommended on shared networks).
  // ArduinoOTA.setPassword("admin");

  ArduinoOTA
    .onStart([]() {
      // Safety: kill the pump before the device reboots into new firmware.
      setPumpBrightness(0);
      String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
      Serial.println("[OTA] Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\n[OTA] End");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      static unsigned long lastOtaProgressMs = 0;
      if (total > 0 && millis() - lastOtaProgressMs > 500) {
        Serial.printf("[OTA] Progress: %u%%\n", (progress / (total / 100)));
        lastOtaProgressMs = millis();
      }
    })
    .onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR)         Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR)     Serial.println("End Failed");
    });

  Serial.println("[OTA] Configured — waiting for WiFi to start the service");
}

void handleOTA()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    if (!otaInitialized)
    {
      ArduinoOTA.begin();
      otaInitialized = true;
      Serial.print("[OTA] Ready — hostname MaraXController, IP: ");
      Serial.println(WiFi.localIP());
    }
    ArduinoOTA.handle();
  }
  else
  {
    otaInitialized = false;  // re-announce the service after a reconnect
#ifdef DISABLE_WIFI_MQTT
    // Only OTA manages WiFi in this build, so retry the connection here.
    // (When MQTT is enabled, tryReconnectWifi() owns reconnection instead.)
    unsigned long now = millis();
    if (now - lastOtaWifiAttemptMs >= OTA_WIFI_RETRY_INTERVAL_MS)
    {
      lastOtaWifiAttemptMs = now;
      WiFi.begin(wifi_ssid, wifi_password);
    }
#endif
  }
}
#endif  // ENABLE_OTA

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Setup
// nunununununununununununununununununununununununununununununun
void setup()
{
  // Force the GiCar brew input inactive as soon as the sketch starts.
  // Preloading the output level before switching to OUTPUT avoids a boot
  // pulse while the pin changes modes.
  writeBrewRelay(false);
  pinMode(BREW_RELAY_PIN, OUTPUT);
  writeBrewRelay(false);

  // USB Serial for debugging — open Serial Monitor at 9600
  Serial.begin(9600);
  delay(1000);
  Serial.println("[DEBUG] setup() start");

  // ESP32-S3 ADC is 12-bit (0–4095). Must set this before any analogRead().
  analogReadResolution(12);
  primePressureFilter();

  // AC Dimming: Zero-cross interrupt and TRIAC control
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  pinMode(AC_ZERO_CROSS_PIN, INPUT);
  
  // Timer for TRIAC trigger delay
  acTimer = timerBegin(1000000);  // 1MHz (1μs resolution)
  timerAttachInterrupt(acTimer, &triacTriggerISR);
  
  // Zero-cross interrupt — try CHANGE if RISING gives ~75 ZC/s instead of ~100
  attachInterrupt(digitalPinToInterrupt(AC_ZERO_CROSS_PIN), zeroCrossISR, CHANGE);
  
  // Match the post-brew refill behavior on cold boot: give the GiCar full
  // pump authority until idle pressure says the boiler is full.
  resumeIdleBoilerFill();

  pinMode(BREW_SWITCH_PIN, INPUT_PULLUP);

  // Prime lever debounce with current hardware level so first brew transition
  // is edge-driven and not sensitive to switch contact bounce at startup.
  brewSwitchStableOn = readRawBrewSwitchOn();
  brewSwitchLastRawOn = brewSwitchStableOn;
  brewSwitchDebounceInitialized = true;
  brewSwitchLastChangeMs = millis();
  Serial.print("[BREW] Switch polarity: ");
  Serial.println(BREW_SWITCH_ACTIVE_LOW ? "LOW=ON" : "HIGH=ON");
  Serial.print("[BREW] Relay polarity: ");
  Serial.println(BREW_RELAY_ACTIVE_HIGH ? "HIGH=ON" : "LOW=ON");

  // Initialize SPI bus with custom pins for the SD card
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  delay(200);

  // ═══════════════════════════════════════════════════════════════════════════
  // MaraX UART: 9600 baud, software RX inversion.
  // uart_set_line_inverse() tells the ESP32-S3 hardware UART to treat the
  // idle-LOW (inverted) signal from the GiCar board as normal UART.
  // Must be called AFTER Serial1.begin().
  // ═══════════════════════════════════════════════════════════════════════════
  Serial1.begin(9600, SERIAL_8N1, MARAX_RX_PIN, MARAX_TX_PIN);
  Serial.print("[DEBUG] Serial1 init - RX:GPIO"); Serial.print(MARAX_RX_PIN);
  Serial.print(" TX:GPIO"); Serial.println(MARAX_TX_PIN);
  
  uart_set_line_inverse(UART_NUM_1, UART_SIGNAL_RXD_INV);
  Serial.println("[DEBUG] UART RX inversion enabled");
  
  // Test if UART can read anything
  delay(100);
  int availBytes = Serial1.available();
  Serial.print("[DEBUG] Initial Serial1.available(): "); Serial.println(availBytes);

  // Nextion display on UART2 with explicit pin assignment.
  // The HMI file shipped in this repo is configured for 115200 baud, so
  // UART2 must use the same rate here or the display will ignore writes.
  // We initialize Serial2 directly instead of calling myNex.begin(),
  // because myNex.begin() would call Serial2.begin(baud) without pin
  // arguments and overwrite our custom pin assignment.
  Serial2.begin(115200, SERIAL_8N1, NEXTION_RX_PIN, NEXTION_TX_PIN);
  Serial.println("[DEBUG] Serial2 (Nextion) started at 115200");

  // Force a known value onto the display right now to verify display wiring
  Serial.println("[DEBUG] Testing Nextion display - writing BOOT to t0.txt");
  myNex.writeStr("t0.txt", "BOOT");
  delay(100);
  
  // Check if display responds by reading connect message
  while (Serial2.available())
  {
    Serial.print("[DEBUG] Nextion response: 0x");
    Serial.println(Serial2.read(), HEX);
  }
  
  delay(2000);
  Serial1.write(0x11);
  Serial.println("[DEBUG] Sent 0x11 wakeup to machine");

  preferences.begin("marax", false);
  loadPersistedProfileSelection();
  loadPersistedTargetWeight();

  // Serial Marax
  memset(receivedCharsFromMarax, 0, numCharsSerialMarax);

  // IMPORTANT: Initialize BLE scale BEFORE WiFi to avoid conflicts
  // BLE and WiFi share radio resources on ESP32, init BLE first
  Serial.println("[DEBUG] Initializing BLE hardware (NimBLE)...");
  
  if (strlen(SCALE_MAC_ADDRESS) > 0)
  {
    Serial.print("[DEBUG] Connecting to scale MAC: ");
    Serial.println(SCALE_MAC_ADDRESS);
  }
  else
  {
    Serial.println("[DEBUG] Auto-discovering any Felicita/Acaia scale...");
  }
  
  // Start scale discovery BEFORE WiFi so BLE gets the best radio conditions,
  // but keep the scan in the background so brew handling is never blocked.
  scale.beginConnect(SCALE_MAC_ADDRESS);
  scaleConnected = scale.isConnected();
  Serial.println(scaleConnected ? "[DEBUG] Scale connected" : "[DEBUG] Scale scan started in background");
  updateScaleConnectionUi();
  lastScaleReconnectAttemptMs = millis();

#ifndef DISABLE_WIFI_MQTT
  // WiFi — non-blocking, continue even if connection fails
  Serial.println("[DEBUG] Starting WiFi...");
  WiFi.setHostname("MaraXController");
  WiFi.begin(wifi_ssid, wifi_password);
  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartTime < 10000))
  {
    delay(500);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiConnected)
  {
    Serial.print("[DEBUG] WiFi connected - IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("[DEBUG] WiFi connection failed");
  }
  lastWifiReconnectAttemptMs = millis();
#else
  Serial.println("[DEBUG] WiFi/MQTT disabled for testing");
#endif

#ifdef ENABLE_OTA
  setupOTA();
#endif

  // SD card init
  Serial.print("[SD] Mounting SD card on CS=GPIO"); Serial.println(SD_CS_PIN);
  if (SD.begin(SD_CS_PIN))
  {
    sdReady = true;
    Serial.print("[SD] Mounted OK — card type: "); Serial.print(SD.cardType());
    Serial.print("  size: "); Serial.print(SD.totalBytes() / (1024 * 1024)); Serial.println(" MB");

    if (!SD.exists("/profiles"))
      SD.mkdir("/profiles");
    if (!SD.exists("/models"))
      SD.mkdir("/models");
    if (!SD.exists("/logs"))
      SD.mkdir("/logs");

    if (!SD.exists("/logs/brews.csv"))
    {
      File f = SD.open("/logs/brews.csv", FILE_WRITE);
      if (f)
      {
        f.println("timestamp_ms,profile,actual_weight,target_weight,brew_time_s,flow_rate_at_stop,pressure_at_stop,extra_weight");
        f.close();
      }
    }

    scanProfiles();
    lastProfileScanMs = millis();
    loadBeta();
    loadObservations();

    if (profileCount > 0)
    {
      int startupIndex = 0;
      if (persistedProfileFile[0] != '\0')
      {
        for (int i = 0; i < profileCount; i++)
        {
          if (strcmp(profileNames[i], persistedProfileFile) == 0)
          {
            startupIndex = i;
            break;
          }
        }
      }
      selectProfile(startupIndex);
    }

    pressureProfilingEnabled = true;
    remoteProfilingEnabled = true;
    myNex.writeNum("pPEnabled", 1);
    myNex.writeNum("remoteEnabled", 1);

    Serial.print("[DEBUG] Setting up profiles - count:"); Serial.println(profileCount);
    Serial.print("[DEBUG] Profile list string: "); Serial.println(profileListStr);
    Serial.print("[DEBUG] Active profile: "); Serial.println(activeProfileName);
    
    // NOTE: profileList.txt and actProftxt.txt components must exist in HMI
    myNex.writeStr("profileList.txt", profileListStr.c_str());
    Serial.println("[DEBUG] Sent profileList.txt");
    
    myNex.writeStr("actProfile.txt", activeProfileName);
    Serial.println("[DEBUG] Sent actProfiletxt.txt");
    updateTargetWeightUi(true);
    Serial.print("[DEBUG] Restored targetWeight: "); Serial.println((int)targetWeight);
    
    populateProfileList();
    updateProfileModeText();
  }
  else
  {
    Serial.println("[SD] FAILED to mount SD card — check wiring and card format (FAT32)");
    Serial.print("[SD]   CS=GPIO"); Serial.print(SD_CS_PIN);
    Serial.print("  SCK=GPIO"); Serial.print(SD_SCK_PIN);
    Serial.print("  MOSI=GPIO"); Serial.print(SD_MOSI_PIN);
    Serial.print("  MISO=GPIO"); Serial.println(SD_MISO_PIN);
  }

  Serial.println("[DEBUG] setup() complete");
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Main Loop
// nunununununununununununununununununununununununununununununun
void loop()
{
  updateScale();
#ifndef DISABLE_WIFI_MQTT
  tryReconnectWifi();
  tryReconnectMqtt();
  if (mqttClient.connected())
  {
    mqttClient.loop();
  }
#endif
#ifdef ENABLE_OTA
  handleOTA();  // Non-blocking: services OTA and (re)connects WiFi in background
#endif
  checkPendingObservation();

  handleNextionProfileTouchEvents();

  getMaschineInput();
  updateDisplay();
  updateMqtt();
  brewDetect();
  updateIdlePumpControl();  // Cut dimmer when boiler is full, restore when empty

  liveData();
  pressureProfile();
  tryReconnectScale();

  refresh_timer = millis();
}

void callbackfun(char *topic, byte *payload, unsigned int length)
{
  (void)topic;
  (void)payload;
  (void)length;
}

void updateMqtt()
{
#ifndef DISABLE_WIFI_MQTT
  if ((millis() - updateMqttTimer > 5000) && !brewActive && POWER_ON)
  {
    mqttClient.publish(debug_topic, toCharArray(String(receivedCharsFromMarax)), true);
    mqttClient.publish(brewtemp_topic, toCharArray(String(brewTemp)), true);
    mqttClient.publish(steamtemp_topic, toCharArray(String(steamTemp)), true);
    mqttClient.publish(steamtargettemp_topic, toCharArray(String(steamTargetTemp)), true);
    mqttClient.publish(fastheat_topic, toCharArray(String(fastHeatingCountDown)), true);
    mqttClient.publish(heatingElement_topic, toCharArray(String(heatingElementOn)), true);
    mqttClient.publish(shots_topic, toCharArray(String(shotCount)), true);
    mqttClient.publish(power_topic, toCharArray(String(POWER_ON)), true);

    updateMqttTimer = millis();
  }
#endif
}

const char *toCharArray(const String &str)
{
  return str.c_str();
}

void sendNextionCommand(const String &command)
{
  Serial2.print(command);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
  Serial2.write(0xFF);
}

String nextionEscapedText(const char *raw)
{
  String escaped = "";
  for (int i = 0; raw[i] != '\0'; i++)
  {
    char c = raw[i];
    if (c == '"')
    {
      escaped += '\'';
    }
    else
    {
      escaped += c;
    }
  }
  return escaped;
}

void loadPersistedProfileSelection()
{
  persistedProfileFile[0] = '\0';
  String savedFile = preferences.getString("selected_file", "");
  if (savedFile.length() > 0)
  {
    savedFile.toCharArray(persistedProfileFile, sizeof(persistedProfileFile));
    persistedProfileFile[sizeof(persistedProfileFile) - 1] = '\0';
  }
}

void persistSelectedProfile()
{
  if (selectedProfileIndex < 0 || selectedProfileIndex >= profileCount)
  {
    return;
  }
  preferences.putString("selected_file", profileNames[selectedProfileIndex]);
}

void loadPersistedTargetWeight()
{
  float savedTargetWeight = preferences.getFloat("target_weight", targetWeight);
  if (savedTargetWeight > 0.0f)
  {
    targetWeight = savedTargetWeight;
  }
}

void persistTargetWeight()
{
  if (targetWeight > 0.0f)
  {
    preferences.putFloat("target_weight", targetWeight);
  }
}

void updateTargetWeightUi(bool force)
{
  static int lastSentTargetWeight = -1;
  static bool lastWasProfilePage = false;
  int value = (int)targetWeight;

  bool onProfilePage =
      currentPageId == (uint32_t)profileSelectionPageId ||
      currentPageId == (uint32_t)(0xFF000000 | profileSelectionPageId);

  if (!force && value == lastSentTargetWeight && onProfilePage == lastWasProfilePage)
  {
    return;
  }

  myNex.writeNum("targetWeight", value);

  if (onProfilePage)
  {
    sendNextionCommand("ntargetW.val=" + String(value));
  }

  lastSentTargetWeight = value;
  lastWasProfilePage = onProfilePage;
}

bool isProfileSelectionPage(uint32_t pageId)
{
  return pageId == (uint32_t)profileSelectionPageId ||
         pageId == (uint32_t)(0xFF000000 | profileSelectionPageId);
}

void syncTargetWeightFromDisplay()
{
  static int lastPersistedTargetWeight = -1;
  int manualTargetWeight = myNex.readNumber("targetWeight");

  // Guard against invalid readNumber() responses and out-of-range values.
  if (manualTargetWeight < 1 || manualTargetWeight > 150)
  {
    return;
  }

  if (manualTargetWeight != (int)targetWeight)
  {
    Serial.print("[DEBUG] Target weight changed: "); Serial.print(targetWeight);
    Serial.print("g -> "); Serial.print(manualTargetWeight); Serial.println("g");
    targetWeight = (float)manualTargetWeight;
  }

  updateTargetWeightUi(false);

  if (manualTargetWeight != lastPersistedTargetWeight)
  {
    persistTargetWeight();
    lastPersistedTargetWeight = manualTargetWeight;
  }
}

void updateProfileSelectionHighlight()
{
  for (int row = 0; row < PROFILE_ROWS_PER_PAGE; row++)
  {
    int profileIndex = profileListPageStart + row;
    String component = "profile1.t" + String(row);
    if (profileIndex >= 0 && profileIndex < profileCount)
    {
      sendNextionCommand(component + ".style=border");
      sendNextionCommand(component + ".borderw=" + String((profileIndex == selectedProfileIndex) ? 3 : 2));
      sendNextionCommand(component + ".borderc=" + String((profileIndex == selectedProfileIndex) ? 2016 : 65535));
    }
  }
}

void populateProfileList()
{
  for (int row = 0; row < PROFILE_ROWS_PER_PAGE; row++)
  {
    int profileIndex = profileListPageStart + row;
    String component = "profile1.t" + String(row);

    if (profileIndex >= 0 && profileIndex < profileCount)
    {
      String stem = profileNames[profileIndex];
      if (stem.endsWith(".csv"))
      {
        stem.remove(stem.length() - 4);
      }

      sendNextionCommand(component + ".txt=\"" + nextionEscapedText(stem.c_str()) + "\"");
      sendNextionCommand(component + ".vis=1");
      sendNextionCommand(component + ".pco=65535");
      sendNextionCommand(component + ".bco=8518");
    }
    else
    {
      sendNextionCommand(component + ".vis=0");
    }
  }

  updateProfileSelectionHighlight();
}

bool selectProfile(int profileIndex)
{
  if (profileIndex < 0 || profileIndex >= profileCount)
  {
    return false;
  }

  if (!loadProfile(profileNames[profileIndex]))
  {
    return false;
  }

  selectedProfileIndex = profileIndex;
  myNex.writeStr("actProfile.txt", activeProfileName);
  updateProfileModeText();
  persistSelectedProfile();
  updateProfileSelectionHighlight();
  return true;
}

void handleProfileSelection(int rowIndex)
{
  int profileIndex = profileListPageStart + rowIndex;
  if (!selectProfile(profileIndex))
  {
    return;
  }

  sendNextionCommand("page home");
}

void showProfileSelection()
{
  if (sdReady)
  {
    scanProfiles();
    lastProfileScanMs = millis();
  }

  profileListPageStart = 0;

  sendNextionCommand("page profile1");

  delay(50);   // test with 50-100 ms first

  populateProfileList();

  profilePageNeedsPopulate = false;
}
void handleNextionProfileTouchEvents()
{
  static uint8_t handledPressComponentId = 0xFF;

  while (Serial2.available() >= 1)
  {
    if (Serial2.peek() != 0x65)
    {
      // Leave non-touch bytes for EasyNextionLibrary readNumber()/responses.
      return;
    }

    if (Serial2.available() < 7)
    {
      return;
    }

    uint8_t packet[7];
    size_t bytesRead = Serial2.readBytes(packet, sizeof(packet));
    if (bytesRead != sizeof(packet))
    {
      return;
    }

    if (packet[4] != 0xFF || packet[5] != 0xFF || packet[6] != 0xFF)
    {
      continue;
    }

    uint8_t pageId = packet[1];
    uint8_t componentId = packet[2];
    uint8_t eventType = packet[3];

    bool isProfileRowTouch =
        componentId >= PROFILE_TOUCH_COMPONENT_ID_MIN &&
        componentId <= PROFILE_TOUCH_COMPONENT_ID_MAX;

    bool isProfilePageEvent =
        pageId == (uint8_t)profileSelectionPageId ||
        pageId == (uint8_t)PROFILE_PAGE_ID_FALLBACK;

    if (!isProfileRowTouch || !isProfilePageEvent)
    {
      continue;
    }

    int rowIndex = componentId - PROFILE_TOUCH_COMPONENT_ID_MIN;

    // Some HMI objects are configured to send press events, others release.
    // Handle both edges while suppressing duplicate handling on release.
    if (eventType == 1)
    {
      handledPressComponentId = componentId;
      profileSelectionPageId = pageId;
      handleProfileSelection(rowIndex);
    }
    else if (eventType == 0)
    {
      if (handledPressComponentId == componentId)
      {
        handledPressComponentId = 0xFF;
        continue;
      }

      handledPressComponentId = 0xFF;
      profileSelectionPageId = pageId;
      handleProfileSelection(rowIndex);
    }
  }
}

// Optional EasyNextion trigger hook for opening the profile page.
// Can be bound in HMI with: printh 23 02 54 32
void trigger50()
{
  //showProfileSelection();
}

float predictedFinalWeight(float flow, float pressure)
{
  return currentWeight + olsBeta[0] + olsBeta[1] * flow + olsBeta[2] * pressure + olsBeta[3] * flow * pressure + olsBeta[4] * flow * flow;
}

void fitOLS()
{
  if (olsCount < OLS_MIN_OBSERVATIONS)
  {
    return;
  }

  double XtX[5][5] = {};
  double Xty[5] = {};

  for (int i = 0; i < olsCount; i++)
  {
    double flow = (double)olsX[i][0];
    double pressure = (double)olsX[i][1];
    double row[5] = {1.0, flow, pressure, flow * pressure, flow * flow};
    for (int r = 0; r < 5; r++)
    {
      Xty[r] += row[r] * olsY[i];
      for (int c = 0; c < 5; c++)
      {
        XtX[r][c] += row[r] * row[c];
      }
    }
  }

  double A[5][6];
  for (int r = 0; r < 5; r++)
  {
    for (int c = 0; c < 5; c++)
    {
      A[r][c] = XtX[r][c];
    }
    A[r][5] = Xty[r];
  }

  for (int col = 0; col < 5; col++)
  {
    int pivot = col;
    for (int row = col + 1; row < 5; row++)
    {
      if (fabs(A[row][col]) > fabs(A[pivot][col]))
      {
        pivot = row;
      }
    }

    for (int c = 0; c <= 5; c++)
    {
      double tmp = A[col][c];
      A[col][c] = A[pivot][c];
      A[pivot][c] = tmp;
    }

    if (fabs(A[col][col]) < OLS_SINGULARITY_THRESHOLD)
    {
      return;
    }

    for (int row = 0; row < 5; row++)
    {
      if (row == col)
      {
        continue;
      }

      double factor = A[row][col] / A[col][col];
      for (int c = col; c <= 5; c++)
      {
        A[row][c] -= factor * A[col][c];
      }
    }
  }

  for (int i = 0; i < 5; i++)
  {
    olsBeta[i] = (float)(A[i][5] / A[i][i]);
  }
}

bool loadProfile(const char *filename)
{
  if (!sdReady)
  {
    Serial.println("[SD] loadProfile: sdReady=false");
    return false;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/profiles/%s", filename);
  Serial.print("[SD] loadProfile: opening '"); Serial.print(path); Serial.println("'");
  File f = SD.open(path);
  if (!f)
  {
    Serial.print("[SD] loadProfile: FAILED to open '"); Serial.print(path); Serial.println("'");
    return false;
  }
  Serial.print("[SD] loadProfile: opened OK — size="); Serial.println(f.size());

  String line;
  f.readStringUntil('\n');
  line = f.readStringUntil('\n');
  f.close();
  line.trim();

  strncpy(activeProfileFileStem, filename, sizeof(activeProfileFileStem) - 1);
  activeProfileFileStem[sizeof(activeProfileFileStem) - 1] = '\0';
  char *ext = strrchr(activeProfileFileStem, '.');
  if (ext != NULL)
  {
    *ext = '\0';
  }

  strncpy(activeProfileName, activeProfileFileStem, sizeof(activeProfileName) - 1);
  activeProfileName[sizeof(activeProfileName) - 1] = '\0';

  int idx = 0;
  char buf[PROFILE_CSV_BUFFER_LEN];
  line.toCharArray(buf, sizeof(buf));
  char *token = strtok(buf, ",");
  while (token != NULL)
  {
    switch (idx)
    {
    case 0:
      strncpy(activeProfileName, token, sizeof(activeProfileName) - 1);
      activeProfileName[sizeof(activeProfileName) - 1] = '\0';
      break;
    case 1:
      t1p = atoi(token);
      t1pWave = map(t1p, 0, 10, 0, 180);
      break;
    case 2:
      t1t = atoi(token);
      break;
    case 3:
      t2p = atoi(token);
      t2pWave = map(t2p, 0, 10, 0, 180);
      break;
    case 4:
      t2t = atoi(token);
      break;
    case 5:
      t3p = atoi(token);
      t3pWave = map(t3p, 0, 10, 0, 180);
      break;
    case 6:
      t3t = atoi(token);
      break;
    case 7:
      t4p = atoi(token);
      t4pWave = map(t4p, 0, 10, 0, 180);
      break;
    case 8:
      t4t = atoi(token);
      break;
    case 9:
      break;
    }
    idx++;
    token = strtok(NULL, ",");
  }

  return idx >= PROFILE_FIELD_COUNT;
}

void loadBeta()
{
  for (int i = 0; i < 5; i++)
  {
    olsBeta[i] = DEFAULT_OLS_BETA[i];
  }

  if (!sdReady)
  {
    return;
  }

  File f = SD.open(GLOBAL_BETA_PATH);
  if (!f)
  {
    return;
  }

  for (int i = 0; i < 5; i++)
  {
    if (f.available())
    {
      olsBeta[i] = f.readStringUntil('\n').toFloat();
    }
  }
  f.close();
}

void saveBeta()
{
  if (!sdReady)
  {
    return;
  }

  SD.remove(GLOBAL_BETA_PATH);
  File f = SD.open(GLOBAL_BETA_PATH, FILE_WRITE);
  if (!f)
  {
    return;
  }

  for (int i = 0; i < 5; i++)
  {
    f.println(olsBeta[i], 6);
  }
  f.close();
}

void loadObservations()
{
  olsCount = 0;
  olsWriteIndex = 0;

  if (!sdReady)
  {
    return;
  }

  File f = SD.open(GLOBAL_DATA_PATH);
  if (!f)
  {
    return;
  }

  f.readStringUntil('\n');

  // Single-pass ring buffer: reads CSV sequentially, maintaining only the last
  // OLS_WINDOW rows in circular buffers (rbX0, rbX1, rbY). rbHead wraps around
  // via modulo arithmetic; rbCount tracks total rows stored (capped at OLS_WINDOW).
  float rbX0[OLS_WINDOW], rbX1[OLS_WINDOW], rbY[OLS_WINDOW];
  int rbHead = 0;
  int rbCount = 0;

  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
    {
      continue;
    }

    char buf[PROFILE_CSV_BUFFER_LEN];
    line.toCharArray(buf, sizeof(buf));
    char *tok = strtok(buf, ",");
    int col = 0;
    float vals[3];
    while (tok != NULL && col < 3)
    {
      vals[col] = atof(tok);
      tok = strtok(NULL, ",");
      col++;
    }

    if (col == 3)
    {
      rbX0[rbHead] = vals[0];
      rbX1[rbHead] = vals[1];
      rbY[rbHead] = vals[2];
      rbHead = (rbHead + 1) % OLS_WINDOW;
      if (rbCount < OLS_WINDOW)
      {
        rbCount++;
      }
    }
  }

  f.close();

  // Copy ring buffer into OLS arrays in chronological order
  // If buffer not yet full, oldest entry is at index 0; otherwise it is at rbHead.
  int start = (rbCount < OLS_WINDOW) ? 0 : rbHead;
  for (int i = 0; i < rbCount; i++)
  {
    int idx = (start + i) % OLS_WINDOW;
    olsX[i][0] = rbX0[idx];
    olsX[i][1] = rbX1[idx];
    olsY[i] = rbY[idx];
  }

  olsCount = rbCount;
  olsWriteIndex = olsCount % OLS_WINDOW;
  fitOLS();
}

void saveObservation(float flow, float pres, float extra)
{
  if (!sdReady)
  {
    return;
  }

  bool exists = SD.exists(GLOBAL_DATA_PATH);
  File f = SD.open(GLOBAL_DATA_PATH, FILE_WRITE);
  if (!f)
  {
    return;
  }

  if (!exists)
  {
    f.println("flow_rate_at_stop,pressure_at_stop,extra_weight");
  }
  f.print(flow, 4);
  f.print(",");
  f.print(pres, 4);
  f.print(",");
  f.println(extra, 4);
  f.close();
}

void startPendingObservation(float flowAtStop, float pressAtStop)
{
  if (!scaleConnected || lastWeightTime == 0)
  {
    pendingObservation = false;
    return;
  }

  unsigned long now = millis();
  pendingObservation = true;
  pendingObsTime = now;
  pendingFlowAtStop = flowAtStop;
  pendingPressAtStop = pressAtStop;
  pendingWeightAtStop = currentWeight;
  pendingStabilityCheckWeight = currentWeight;
  pendingStabilityCheckTime = now;
  pendingBrewTimeS = (int)((now - activeBrewingStart) / 1000);
}

void finalizePendingObservation(float finalWeight, bool allowModelUpdate)
{
  pendingObservation = false;

  float extraWeight = finalWeight - pendingWeightAtStop;

  bool validForModel = true;
  if (extraWeight < 0.0f || extraWeight > MAX_VALID_EXTRA_WEIGHT)
  {
    validForModel = false;
  }
  if (finalWeight < MIN_VALID_FINAL_WEIGHT)
  {
    validForModel = false;
  }

  if (allowModelUpdate && validForModel)
  {
    int slot = olsWriteIndex;
    olsX[slot][0] = pendingFlowAtStop;
    olsX[slot][1] = pendingPressAtStop;
    olsY[slot] = extraWeight;

    if (olsCount < OLS_WINDOW)
    {
      olsCount++;
    }
    olsWriteIndex = (olsWriteIndex + 1) % OLS_WINDOW;

    fitOLS();
    saveBeta();
    saveObservation(pendingFlowAtStop, pendingPressAtStop, extraWeight);
  }

  // Always save the brew log once an observation window was started.
  saveBrewLog(finalWeight, pendingBrewTimeS, pendingFlowAtStop, pendingPressAtStop, extraWeight);
}

void saveBrewLog(float finalWeight, int brewTimeS, float flow, float pres, float extra)
{
  if (!sdReady)
  {
    return;
  }

  File f = SD.open("/logs/brews.csv", FILE_WRITE);
  if (!f)
  {
    return;
  }

  f.print(millis());
  f.print(",");
  f.print(activeProfileName);
  f.print(",");
  f.print(finalWeight, 2);
  f.print(",");
  f.print(targetWeight, 2);
  f.print(",");
  f.print(brewTimeS);
  f.print(",");
  f.print(flow, 4);
  f.print(",");
  f.print(pres, 4);
  f.print(",");
  f.println(extra, 4);
  f.close();
}

void updateProfileSelectUi()
{
  populateProfileList();
}

void updateProfileModeText()
{
  static char lastProfileMode[PROFILE_NAME_MAX_LEN] = "";
  const char* newMode;

  if (pressureProfilingEnabled && remoteProfilingEnabled && sdReady)
  {
    newMode = activeProfileName;
    myNex.writeStr("profileModetxt.txt", activeProfileName);  // local var on brew page (for Preinitialize)
    myNex.writeStr("profileMode.txt", activeProfileName);    // direct write if currently on brew page
  }
  else if (pressureProfilingEnabled)
  {
    newMode = "Manual";
    myNex.writeStr("profileModetxt.txt", "Manual");
    myNex.writeStr("profileMode.txt", "Manual");
  }
  else
  {
    newMode = "None";
    myNex.writeStr("profileModetxt.txt", "None");
    myNex.writeStr("profileMode.txt", "None");
  }

  // Only log if mode actually changed
  if (strcmp(newMode, lastProfileMode) != 0)
  {
    Serial.print("[DEBUG] Profile mode changed: ");
    Serial.print(lastProfileMode);
    Serial.print(" -> ");
    Serial.println(newMode);
    strncpy(lastProfileMode, newMode, sizeof(lastProfileMode) - 1);
    lastProfileMode[sizeof(lastProfileMode) - 1] = '\0';
  }
}

void updateScaleConnectionUi()
{
  int connected = scaleConnected ? 1 : 0;
  
  if (connected != lastScaleConnectedUi)
  {
    Serial.print("[DEBUG] Scale connection changed: "); 
    Serial.print(lastScaleConnectedUi);
    Serial.print(" -> ");
    Serial.println(connected);

    // Update global variable (used by home page timer to refresh scalecon.val)
    myNex.writeNum("scaleConnected", connected);
    // Directly update p3.pic on home page (pic 20=connected, 21=disconnected)
    // home.p3.pic works as a cross-page write; silently ignored if home page not loaded
    myNex.writeNum("home.p3.pic", connected ? 20 : 21);

    lastScaleConnectedUi = connected;
  }
}

void tryReconnectScale()
{
  if (brewActive || readRawBrewSwitchOn())
  {
    if (scale.isConnectionAttemptInProgress())
    {
      scale.cancelPendingConnect();
      lastScaleReconnectAttemptMs = 0;
    }
    return;
  }

  if (scaleConnected)
  {
    return;
  }

  if (scale.isConnectionAttemptInProgress())
  {
    return;
  }

  unsigned long now = millis();
  if (now - lastScaleReconnectAttemptMs < SCALE_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastScaleReconnectAttemptMs = now;
  
  // With WiFi disabled, BLE has full control of the radio
  Serial.println("[DEBUG] Starting background BLE scale scan...");
  
  // Reinitialize and reconnect to scale (auto-discovers if MAC is empty)
  scale.beginConnect(SCALE_MAC_ADDRESS);
  scaleConnected = scale.isConnected();
  if (!scaleConnected)
  {
    flowRate = 0.0f;
  }
  
  updateScaleConnectionUi();
}

void tryReconnectWifi()
{
#ifndef DISABLE_WIFI_MQTT
  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    return;
  }

  wifiConnected = false;

  unsigned long now = millis();
  if (now - lastWifiReconnectAttemptMs < WIFI_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastWifiReconnectAttemptMs = now;
  WiFi.begin(wifi_ssid, wifi_password);
#endif
}

void tryReconnectMqtt()
{
#ifndef DISABLE_WIFI_MQTT
  if (mqttClient.connected())
  {
    return;
  }

  if (!wifiConnected || WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  unsigned long now = millis();
  if (now - lastMqttReconnectAttemptMs < MQTT_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastMqttReconnectAttemptMs = now;

  if (mqttClient.connect("MaraXMod", mqtt_user, mqtt_password))
  {
  }
#endif
}

void scanProfiles()
{
  profileCount = 0;
  profileListStr = "";

  if (!sdReady)
  {
    Serial.println("[SD] scanProfiles: sdReady=false — skipping");
    return;
  }

  Serial.println("[SD] scanProfiles: opening /profiles");
  File dir = SD.open("/profiles");
  if (!dir)
  {
    Serial.println("[SD] scanProfiles: FAILED to open /profiles directory");
    return;
  }
  Serial.println("[SD] scanProfiles: directory opened OK");

  int skipped = 0;
  while (profileCount < MAX_PROFILES)
  {
    File entry = dir.openNextFile();
    if (!entry)
    {
      break;
    }

    String rawName = entry.name();
    bool isDir = entry.isDirectory();
    Serial.print("[SD] scanProfiles: entry='" ); Serial.print(rawName);
    Serial.print("' isDir="); Serial.println(isDir);

    if (!isDir)
    {
      String name = rawName;
      int slashIndex = name.lastIndexOf('/');
      if (slashIndex >= 0)
      {
        name = name.substring(slashIndex + 1);
      }

      if (name.endsWith(".csv"))
      {
        name.toCharArray(profileNames[profileCount], sizeof(profileNames[profileCount]));
        profileNames[profileCount][sizeof(profileNames[profileCount]) - 1] = '\0';
        if (profileListStr.length() > 0)
        {
          profileListStr += ",";
        }
        profileListStr += name.substring(0, name.length() - 4);
        Serial.print("[SD] scanProfiles: added profile '"); Serial.print(profileNames[profileCount]); Serial.println("'");
        profileCount++;
      }
      else
      {
        Serial.print("[SD] scanProfiles: skipped (not .csv): "); Serial.println(name);
        skipped++;
      }
    }
    entry.close();
  }
  dir.close();
  Serial.print("[SD] scanProfiles: done — found "); Serial.print(profileCount);
  Serial.print(" profiles, skipped "); Serial.println(skipped);
}

void updateScale()
{
  if (!scaleConnected && (brewActive || readRawBrewSwitchOn()))
  {
    if (scale.isConnectionAttemptInProgress())
    {
      scale.cancelPendingConnect();
      lastScaleReconnectAttemptMs = 0;
    }
    return;
  }

  if (!scaleConnected && scale.pollConnect())
  {
    scaleConnected = true;
    updateScaleConnectionUi();
  }

  if (!scaleConnected)
  {
    return;
  }

  if (!scale.isConnected())
  {
    scale.disconnect();
    scaleConnected = false;
    flowRate = 0.0f;
    updateScaleConnectionUi();
    return;
  }

  if (scale.heartbeatRequired())
  {
    scale.heartbeat();
  }

  if (scale.newWeightAvailable())
  {
    float newWeight = scale.getWeight();
    unsigned long now = millis();
    unsigned long dt = now - lastWeightTime;

    prevWeight = currentWeight;
    currentWeight = newWeight;

    if (dt > 0 && lastWeightTime > 0)
    {
      flowRate = (currentWeight - prevWeight) / (dt / 1000.0f);
    }

    lastWeightTime = now;
  }
}

void checkPendingObservation()
{
  if (!pendingObservation)
  {
    return;
  }
  unsigned long now = millis();
  if (now - pendingObsTime >= OBSERVATION_MAX_WAIT_MS)
  {
    // Timeout reached with unstable weight: keep the brew record using the
    // current reading as finalWeight and include it in OLS training.
    finalizePendingObservation(currentWeight, true);
    return;
  }

  if (now - pendingStabilityCheckTime < WEIGHT_STABLE_WINDOW_MS)
  {
    return;
  }

  if (currentWeight - pendingStabilityCheckWeight >= WEIGHT_STABLE_THRESHOLD)
  {
    pendingStabilityCheckWeight = currentWeight;
    pendingStabilityCheckTime = now;
    return;
  }

  // Stable within the configured window: accept this as final and train model.
  finalizePendingObservation(currentWeight, true);
}

// Gets "live" Info during brew
void liveData()
{
  if (brewActive)
  {
    // Write Brew Temp
    myNex.writeNum("brewTemp", brewTemp);
    // Write BrewTimer
    myNex.writeNum("brewTime", (int)((millis() - activeBrewingStart) / 1000));
    // Write Live Pressure as normal number
    float pressure = getPressure();
    // brewBar is an XFloat with 3 decimal places: it displays value/1000.
    myNex.writeNum("barvar.val", (int)(pressure * 100));
    // Map the pressure to wave pixels in graph (0–10 bar → 0–180 px)
    barGraphValue = map((int)(pressure * 100), 0, 1000, 0, 180);
    myNex.writeNum("barwave.val", barGraphValue);

    if (scaleConnected)
    {
      myNex.writeNum("weightVar.val", (int)(currentWeight * 10));
      myNex.writeNum("flowVar.val", (int)(flowRate * 100));
    }

    // Periodic dimmer diagnostics (every 1 second during brew)
    // lastDimmerDebugMs and lastZeroCrossCountDebug are globals seeded in
    // brewDetect() at brew start, so the very first reading is always accurate.
    unsigned long nowMs = millis();
    if (nowMs - lastDimmerDebugMs >= 1000)
    {
      uint32_t snapshot = zeroCrossCount;  // single read, ISR-safe for uint32
      uint32_t crossingsPerSec = snapshot - lastZeroCrossCountDebug;
      lastZeroCrossCountDebug = snapshot;
      unsigned long windowMs = nowMs - lastDimmerDebugMs;  // actual elapsed time
      lastDimmerDebugMs = nowMs;

      uint8_t brt = pumpBrightness;  // snapshot volatile
      uint32_t delayUs = 0;
      if (brt > 0 && brt < 255)
        delayUs = map(brt, 0, 255, 10000, 100);

      // Normalise: scale to per-second if the window was stretched by Nextion I/O
      uint32_t zcPerSec = (windowMs > 0) ? (crossingsPerSec * 1000UL / windowMs) : crossingsPerSec;

      Serial.print("[DIMMER] brightness="); Serial.print(brt);
      Serial.print("/255 ("); Serial.print(brt * 100 / 255); Serial.print("%)");
      if (brt == 0)        Serial.print("  PUMP OFF");
      else if (brt == 255) Serial.print("  FULL POWER");
      else { Serial.print("  delay="); Serial.print(delayUs); Serial.print("µs"); }
      Serial.print("  ZC/s="); Serial.print(zcPerSec);
      Serial.print(" (window="); Serial.print(windowMs); Serial.print("ms)");
      if (zcPerSec < 80 || zcPerSec > 120)
        Serial.print("  *** ABNORMAL (expect ~100 at 50 Hz) ***");
      Serial.println();
    }
  }
}

float getPressure()
{
  // Sensor:
  //  - Supply: 5V
  //  - Output: 0.4V to 2.4V
  //  - Range: 0 to 1.2 MPa ~= 0 to 12 bar
  //
  // ESP32-S3 ADC:
  //  - 3.3V reference
  //  - analogReadResolution(12) → range 0..4095
  //  - NOTE: ESP32 ADC has non-linearity near 0V and 3.3V rails.
  //    The sensor output range (0.4V–2.4V) sits in the linear region, so
  //    this is not a practical concern here.

  const float sensorMinV = 0.4f;
  const float sensorMaxV = 2.4f;
  const float sensorMaxBar = 12.0f;

  sensorVal = (float)analogRead(PRESSURE_SENSOR_PIN);
  filteredVal = (alpha * filteredVal) + ((1.0 - alpha) * sensorVal);
  voltage = (filteredVal / 4096.0f) * 3.3f; // 12-bit: divide by 4096

  float Pressure = (voltage - sensorMinV) * sensorMaxBar / (sensorMaxV - sensorMinV);

  if (Pressure < 0.0f) return 0.0f;
  if (Pressure > sensorMaxBar) return sensorMaxBar;
  return Pressure;
}

void readSettigs()
{
  if ((millis() - readSettigsRefreshTimer > 4000) && !brewActive && !pendingObservation)
  {
    if (sdReady && (millis() - lastProfileScanMs >= PROFILE_SCAN_INTERVAL_MS))
    {
      int previousProfileCount = profileCount;
      scanProfiles();
      lastProfileScanMs = millis();
      if (previousProfileCount != profileCount)
      {
        Serial.print("[DEBUG] Profile count changed: "); Serial.print(previousProfileCount);
        Serial.print(" -> "); Serial.println(profileCount);
        populateProfileList();
      }
    }

    getPressure();
    
    // Read Nextion variables (only log if values change)
    static int lastPPEnabled = -1;
    static int lastRemoteEnabled = -1;
    
    // readNumber() returns -1 on timeout/error. Guard: only accept 0 or 1.
    int rawPP = myNex.readNumber("pPEnabled");
    if (rawPP >= 0) pressureProfilingEnabled = (rawPP != 0);
    int rawRemote = myNex.readNumber("remoteEnabled");
    if (rawRemote >= 0) remoteProfilingEnabled = (rawRemote != 0);
    
    // Only log if settings actually changed
    if (pressureProfilingEnabled != lastPPEnabled || 
        remoteProfilingEnabled != lastRemoteEnabled)
    {
      Serial.print("[DEBUG] Settings changed - pPEnabled:"); Serial.print(pressureProfilingEnabled);
      Serial.print(" remoteEnabled:"); Serial.println(remoteProfilingEnabled);
      
      lastPPEnabled = pressureProfilingEnabled;
      lastRemoteEnabled = remoteProfilingEnabled;
    }
    
    // Manual mode (pressure profiling ON, remote OFF): read manual pressure settings
    if (!remoteProfilingEnabled && pressureProfilingEnabled)
    {
      int temp = myNex.readNumber("t1p");
      if (temp != t1p)
      {
        t1p = temp;
        t1pWave = map(temp, 0, 10, 0, 180);
      }
      temp = myNex.readNumber("t2p");
      if (temp != t2p)
      {
        t2p = temp;
        t2pWave = map(temp, 0, 10, 0, 180);
      }
      temp = myNex.readNumber("t3p");
      if (temp != t3p)
      {
        t3p = temp;
        t3pWave = map(temp, 0, 10, 0, 180);
      }
      temp = myNex.readNumber("t4p");
      if (temp != t4p)
      {
        t4p = temp;
        t4pWave = map(temp, 0, 10, 0, 180);
      }

      t1t = myNex.readNumber("t1t");
      t2t = myNex.readNumber("t2t");
      t3t = myNex.readNumber("t3t");
      t4t = myNex.readNumber("t4t");
    }
    
    // targetWeight is authoritative from display input when available.
    syncTargetWeightFromDisplay();

    updateProfileModeText();
    readSettigsRefreshTimer = millis();
  }
}

void updateDisplay()
{
  if ((millis() > pageRefreshTimer) && !brewActive)
  {
    if (POWER_ON && displayIsInSleep)
    {
      Serial.println("[DEBUG] Display waking up");
      myNex.writeNum("sleep", 0);
      displayIsInSleep = false;
    }
    else if (!POWER_ON && !displayIsInSleep)
    {
      Serial.println("[DEBUG] Display going to sleep");
      myNex.writeStr("page home");
      myNex.writeNum("sleep", 1);
      displayIsInSleep = true;
    }

    // Throttled debug output - only every 10 seconds
    static unsigned long lastDisplayDebugMs = 0;
    if (millis() - lastDisplayDebugMs > 10000) {
      Serial.print("[DEBUG] Display status — brewTemp:"); Serial.print(brewTemp);
      Serial.print(" steamTemp:"); Serial.print(steamTemp);
      Serial.print(" POWER_ON:"); Serial.print(POWER_ON);
      Serial.print(" page:"); Serial.println(currentPageId);
      lastDisplayDebugMs = millis();
    }
    
    // Send updated values to display (no debug spam)
    myNex.writeNum("brewTemp", brewTemp);
    myNex.writeNum("steamTemp", steamTemp);
    myNex.writeStr("actProfile.txt", activeProfileName);
    updateProfileModeText();
    
    updateScaleConnectionUi();

    currentPageId = myNex.readNumber("dp");

    if (isProfileSelectionPage(currentPageId))
    {
      // Keep MCU targetWeight synchronized from profile page selector.
      syncTargetWeightFromDisplay();
      updateTargetWeightUi(false);
    }

    if (currentPageId != lastPageId)
    {
      if (isProfileSelectionPage(currentPageId))
      {
        profilePageNeedsPopulate = true;
      }
      lastPageId = currentPageId;
    }

    if (profilePageNeedsPopulate &&
        isProfileSelectionPage(currentPageId))
    {
      populateProfileList();
      profilePageNeedsPopulate = false;
    }

    // Always write all values - page checks removed for simplicity
    myNex.writeNum("tarsteam.val", steamTargetTemp);
    myNex.writeNum("fastheattimer.val", fastHeatingCountDown);
    myNex.writeNum("heatingel.val", heatingElementOn);

    // Read settings (except during active brew)
    if (currentPageId != 4278190082 && currentPageId != 2)
    {
      readSettigs();
    }

    // Check if we're on cleaning page
    if (currentPageId == 4278190086 || currentPageId == 6)
    {
      cleaningModeActive = true;
    }
    else
    {
      cleaningShots = 0;
      cleaningShotsWater = 0;
      cleaningModeActive = false;
    }

    pageRefreshTimer = millis() + REFRESH_SCREEN_EVERY;
  }
}

// Controls the dimmer during idle (not brewing).
// When pressure is detected the boiler is full — cut dimmer so pump stops.
// When pressure drops back to zero the boiler needs water — restore dimmer
// to 100% so GiCar can run the pump through it unimpeded.
void updateIdlePumpControl()
{
  if (brewActive) return;  // During brew, pressureProfile() owns the dimmer

  // Do not latch boiler-full before the MaraX reports that it is actually on.
  // During boot, steamTemp is still 0 and the pressure filter may not yet
  // reflect the real sensor value.
  if (!POWER_ON)
  {
    if (pumpBrightness != 255) setPumpBrightness(255);
    return;
  }

  // During cleaning/backflush the GiCar drives the pump on each lever press.
  // Keep the dimmer fully open so backflush pressure can build — the idle
  // "boiler full" cut-off below must not fight it.
  if (cleaningModeActive || cleaningRunActive)
  {
    if (pumpBrightness != 255) setPumpBrightness(255);
    return;
  }

  boilerFullPressureBar = (steamTemp > 80) ? 5.0f : 0.5f;
  float p = getPressure();
  if (p > boilerFullPressureBar)
  {
    // Boiler has pressure — latch off and cut power
    boilerFullLatch = true;
    if (pumpBrightness != 0) setPumpBrightness(0);
  }
  else if (!boilerFullLatch)
  {
    // No pressure and not latched — allow GiCar to run the pump
    if (pumpBrightness != 255) setPumpBrightness(255);
  }
  // If latched and pressure is gone: stay at 0 until brew ends
}

void setPressure(float targetValue)
{
  int pumpValue;
  float currentPressure = (getPressure() - 2.2f);

  if (targetValue == 0 || currentPressure > targetValue)
  {
    pumpValue = 0;
  }
  else
  {
    float diff = targetValue - currentPressure;
    pumpValue = 255 / (1.f + exp(2.f - diff / 0.9f));
  }
  setPumpBrightness(pumpValue);
}

void pressureProfile()
{
  if (brewActive && pressureProfilingEnabled)
  {
    // Safety: if all profile segments have zero duration, the profile is not
    // configured. Skip profiling so the pump is not silently killed.
    int totalProfileTime = t1t + t2t + t3t + t4t;
    if (totalProfileTime == 0)
    {
      setPumpBrightness(255);  // No profile configured → full power
      return;
    }

    // Brew-by-weight auto-stop is only valid when a scale is connected and
    // we have received at least one weight sample in this session.
    bool brewByWeightActive =
        scaleConnected &&
        targetWeight > 0.0f &&
        lastWeightTime > 0;

    // Target-weight latch: once the predicted final weight reaches target,
    // keep the pump off for the rest of this brew. Without this latch the
    // flow/pressure collapse after the pump stops drops the prediction back
    // under target, and the profile segments below would switch it on again.
    if (targetWeightReached)
    {
      setPumpBrightness(0);
      return;
    }

    if (brewByWeightActive)
    {
      float pressureNow = getPressure();
      float predicted = predictedFinalWeight(flowRate, pressureNow);
      if (predicted >= targetWeight)
      {
        // Start stability window from the exact moment target stop is triggered.
        if (!pendingObservation)
        {
          startPendingObservation(flowRate, pressureNow);
        }
        targetWeightReached = true;
        setPumpBrightness(0);
        myNex.writeNum("n0.pco", 1535);
        myNex.writeNum("n1.pco", 1535);
        myNex.writeNum("setbar.val", 0);
        return;
      }
    }

    int brewSecs = (int)((millis() - activeBrewingStart) / 1000);

    if (brewSecs <= t1t)
    {
      myNex.writeNum("setbar.val", t1pWave);
      setPressure(t1p);
    }
    else if (brewSecs <= (t2t + t1t))
    {
      myNex.writeNum("setbar.val", t2pWave);
      setPressure(t2p);
    }
    else if (brewSecs <= (t3t + t2t + t1t))
    {
      myNex.writeNum("setbar.val", t3pWave);
      setPressure(t3p);
    }
    else if (brewSecs <= (t4t + t3t + t2t + t1t))
    {
      myNex.writeNum("setbar.val", t4pWave);
      setPressure(t4p);

      if ((t4t + t3t + t2t + t1t) - brewSecs <= 3)
      {
        myNex.writeNum("n0.pco", 64864);
        myNex.writeNum("n1.pco", 64864);
      }
    }
    else if (brewSecs > (t4t + t3t + t2t + t1t))
    {
      myNex.writeNum("n0.pco", 1535);
      myNex.writeNum("n1.pco", 1535);

      if (brewByWeightActive)
      {
        // Existing behavior: with a connected scale, profile completion can
        // still terminate pump flow automatically.
        myNex.writeNum("setbar.val", 0);
        setPumpBrightness(0);
      }
      else
      {
        // No scale: continue at the final profile pressure and let the user
        // end the shot manually with the lever.
        int holdPressure = t4p;
        int holdWave = t4pWave;

        if (t4t <= 0)
        {
          if (t3t > 0)
          {
            holdPressure = t3p;
            holdWave = t3pWave;
          }
          else if (t2t > 0)
          {
            holdPressure = t2p;
            holdWave = t2pWave;
          }
          else
          {
            holdPressure = t1p;
            holdWave = t1pWave;
          }
        }

        myNex.writeNum("setbar.val", holdWave);
        setPressure(holdPressure);
      }
    }
  }
}

void brewDetect()
{
  if (brewState())
  {
    if (cleaningModeActive && !cleaningRunActive)
    {
      myNex.writeNum("cleanTimer", 1);
      cleaningRunActive = true;
      cleaningShots++;
      if (cleaningShots < 6)
      {
        myNex.writeNum("n0.val", cleaningShots);
        if (cleaningShots == 5)
        {
          myNex.writeNum("n0.pco", 2047);
        }
      }
      else
      {
        myNex.writeNum("n0.pco", 65535);
        cleaningShotsWater++;
        myNex.writeNum("n0.val", cleaningShotsWater);
        if (cleaningShotsWater == 5)
        {
          myNex.writeNum("n0.pco", 2047);
        }
      }
    }

    if (!brewActive && !cleaningModeActive)
    {
      myNex.writeStr("page brew");
      delay(10);
      brewTimer(true);
      myNex.writeNum("pBrew.pic", 25);
      brewActive = true;
      targetWeightReached = false;  // Fresh brew — clear the weight cut-off latch

      // Seed dimmer debug counters so the first ZC/s reading is not
      // inflated by zero-crossings accumulated since boot.
      lastZeroCrossCountDebug = zeroCrossCount;
      lastDimmerDebugMs = millis();

      // Enable TRIAC at full power; pressureProfile() adjusts immediately if profiling is enabled
      setPumpBrightness(255);
    }
  }
  else
  {
    bool wasBrewing = brewActive;
    brewActive = false;
    brewTimer(false);
    myNex.writeNum("pBrew.pic", 8);

    // Only hand the dimmer back to idle control on the brew→idle edge.
    // Doing this every idle loop fought updateIdlePumpControl() (255↔0
    // chatter) and reset boilerFullLatch before it could ever hold.
    if (wasBrewing)
    {
      targetWeightReached = false;  // Clear weight cut-off latch for next brew
      resumeIdleBoilerFill();       // Unlock idle control and hand full pump control back to GiCar
    }

    if (cleaningRunActive)
    {
      myNex.writeNum("cleanTimer", 0);
    }
    cleaningRunActive = false;
  }
}

bool brewState()
{
  bool brewOn = readDebouncedBrewSwitchOn();
  writeBrewRelay(brewOn);  // Signal GiCar: brew switch state
  return brewOn;
}

bool readRawBrewSwitchOn()
{
#if BREW_SWITCH_ACTIVE_LOW
  return digitalRead(BREW_SWITCH_PIN) == LOW;
#else
  return digitalRead(BREW_SWITCH_PIN) == HIGH;
#endif
}

bool readDebouncedBrewSwitchOn()
{
  bool rawOn = readRawBrewSwitchOn();
  unsigned long now = millis();

  if (!brewSwitchDebounceInitialized)
  {
    brewSwitchDebounceInitialized = true;
    brewSwitchLastRawOn = rawOn;
    brewSwitchStableOn = rawOn;
    brewSwitchLastChangeMs = now;
    return brewSwitchStableOn;
  }

  if (rawOn != brewSwitchLastRawOn)
  {
    brewSwitchLastRawOn = rawOn;
    brewSwitchLastChangeMs = now;
  }

  if ((now - brewSwitchLastChangeMs) >= BREW_SWITCH_DEBOUNCE_MS &&
      brewSwitchStableOn != rawOn)
  {
    brewSwitchStableOn = rawOn;
    Serial.print("[BREW] Lever ");
    Serial.println(brewSwitchStableOn ? "ON" : "OFF");
  }

  return brewSwitchStableOn;
}

void writeBrewRelay(bool brewOn)
{
#if BREW_RELAY_ACTIVE_HIGH
  digitalWrite(BREW_RELAY_PIN, brewOn ? HIGH : LOW);
#else
  digitalWrite(BREW_RELAY_PIN, brewOn ? LOW : HIGH);
#endif
}

// Mara X Machine Input Parser
// Frame format (CSV): +1.10,023,,0,022,0000,0,0
// Terminated with CR (\r)
void getMaschineInput()
{
  static bool frameReceived = false;
  static unsigned long lastMachineDebugMs = 0;
  static unsigned long lastFrameReceivedMs = 0;
  bool anyBytesReceived = false;
  bool newFrameThisIteration = false;
  
  while (Serial1.available())
  {
    anyBytesReceived = true;
    rc = Serial1.read();

    // Uncomment for raw byte debugging (disabled by default - confirmed working)
    // if (!frameReceived) {
    //   Serial.print("[RAW] 0x");
    //   if ((uint8_t)rc < 0x10) Serial.print("0");
    //   Serial.print((uint8_t)rc, HEX);
    //   Serial.print(" (");
    //   if (rc >= 0x20 && rc < 0x7F) Serial.print(rc); else Serial.print('?');
    //   Serial.println(")");
    // }
    
    if (rc != endMarker)
    {
      receivedCharsFromMarax[ndx] = rc;
      ndx++;
      if (ndx >= numCharsSerialMarax)
      {
        ndx = numCharsSerialMarax - 1;
      }
    }
    else
    {
      receivedCharsFromMarax[ndx] = '\0';
      ndx = 0;
      frameReceived = true;
      newFrameThisIteration = true;
      lastFrameReceivedMs = millis();
      
      // Only log frame if it contains new/changed data (not every poll)
      static int lastBrewTemp = -1;
      static int lastSteamTemp = -1;
      bool tempChanged = false;

      // Parse CSV format: +1.10,034,138,022,0000,1,0
      // Manual split to handle empty fields correctly (strtok skips them)
      // Field 0: Version (+1.10)
      // Field 1: Brew temp (034 = 34°C)
      // Field 2: Steam target temp (138°C, or empty)
      // Field 3: Steam temp (022 = 22°C)
      // Field 4: Fast heat countdown (0000)
      // Field 5: Heating element (1 = on)
      // Field 6: Unknown
      
      char parseBuffer[numCharsSerialMarax];
      strncpy(parseBuffer, receivedCharsFromMarax, sizeof(parseBuffer));
      
      char* fields[8];
      int fieldCount = 0;
      char* start = parseBuffer;
      
      // Split by commas, preserving empty fields
      for (int i = 0; parseBuffer[i] != '\0' && fieldCount < 8; i++)
      {
        if (parseBuffer[i] == ',')
        {
          parseBuffer[i] = '\0';
          fields[fieldCount++] = start;
          start = &parseBuffer[i + 1];
        }
      }
      if (fieldCount < 8)
      {
        fields[fieldCount++] = start;  // Last field
      }
      
      // Parse fields
      // Frame: +1.10,103,128,077,0000,1,0
      // field[3]=brew temp  field[2]=steam target  field[1]=steam temp  field[4]=countdown  field[5]=heating
      if (fieldCount > 3 && fields[3][0] != '\0')
      {
        int newBrewTemp = atoi(fields[3]);
        if (newBrewTemp != lastBrewTemp)
        {
          tempChanged = true;
          lastBrewTemp = newBrewTemp;
        }
        brewTemp = newBrewTemp;
      }
      if (fieldCount > 2 && fields[2][0] != '\0')
      {
        steamTargetTemp = atoi(fields[2]);
      }
      if (fieldCount > 3 && fields[1][0] != '\0')
      {
        int newSteamTemp = atoi(fields[1]);
        if (newSteamTemp != lastSteamTemp)
        {
          tempChanged = true;
          lastSteamTemp = newSteamTemp;
        }
        steamTemp = newSteamTemp;
      }
      if (fieldCount > 4 && fields[4][0] != '\0')
      {
        fastHeatingCountDown = atoi(fields[4]);
      }
      if (fieldCount > 5 && fields[5][0] != '\0')
      {
        heatingElementOn = atoi(fields[5]);
      }
      
      // Only log when temps actually change
      if (tempChanged)
      {
        Serial.print("[DEBUG] Machine frame: ");
        Serial.println(receivedCharsFromMarax);
        Serial.print("[DEBUG] Temps - brew:"); Serial.print(brewTemp);
        Serial.print("°C steam:"); Serial.print(steamTemp);
        Serial.print("°C target:"); Serial.print(steamTargetTemp);
        Serial.print("°C heating:"); Serial.println(heatingElementOn ? "ON" : "OFF");
      }
    }
  }
  
  // Check for machine power state based on fresh data and timeout
  if (newFrameThisIteration)
  {
    if (!POWER_ON)
    {
      primePressureFilter();
      resumeIdleBoilerFill();
      Serial.println("[DEBUG] POWER_ON = true (machine data received)");
    }
    POWER_ON = true;
  }
  else if (POWER_ON && lastFrameReceivedMs > 0 && (millis() - lastFrameReceivedMs > 15000))
  {
    Serial.println("[DEBUG] POWER_ON = false (no serial for 15 seconds)");
    POWER_ON = false;
    brewTemp = 0;
    steamTemp = 0;
    steamTargetTemp = 0;
    fastHeatingCountDown = 0;
    heatingElementOn = 0;

#ifndef DISABLE_WIFI_MQTT
    if (mqttClient.connected())
    {
      mqttClient.publish(brewtemp_topic, toCharArray(String(brewTemp)), true);
      mqttClient.publish(steamtemp_topic, toCharArray(String(steamTemp)), true);
      mqttClient.publish(steamtargettemp_topic, toCharArray(String(steamTargetTemp)), true);
      mqttClient.publish(fastheat_topic, toCharArray(String(fastHeatingCountDown)), true);
      mqttClient.publish(heatingElement_topic, toCharArray(String(heatingElementOn)), true);
      mqttClient.publish(shots_topic, toCharArray(String(shotCount)), true);
      mqttClient.publish(power_topic, toCharArray(String(POWER_ON)), true);
    }
#endif
  }

  if (millis() - serialMaraxUpdateMillis > 5000)
  {
    serialMaraxUpdateMillis = millis();
    memset(receivedCharsFromMarax, 0, numCharsSerialMarax);
    ndx = 0;
    
    // Reduced logging - only every 30 seconds (machine communication confirmed working)
    static unsigned long lastPollDebugMs = 0;
    if (millis() - lastPollDebugMs > 30000)
    {
      Serial.println("[DEBUG] Polling machine...");
      lastPollDebugMs = millis();
    }
    
    Serial1.write(0x11);
  }
}

void brewTimer(bool start)
{
  if (!brewTimerActive && start)
  {
    myNex.writeNum("activeBrewTime", 0);
    myNex.writeNum("timerState", 1);
    activeBrewingStart = millis();
    brewTimerActive = true;
  }
  else if (!start)
  {
    if (brewTimerActive)
    {
      myNex.writeNum("timerState", 0);
      int brewSecs = (int)((millis() - activeBrewingStart) / 1000);
      if (brewSecs > 20)
      {
        shotCount++;
      }

      float weightAtStop = currentWeight;
      float flowAtStop = flowRate;
      float pressAtStop = getPressure();

      // If target-weight stop already started pending observation, keep its
      // start time so the stability window reflects pump-off timing.
      if (pendingObservation)
      {
        pendingBrewTimeS = brewSecs;
      }
      else if (scaleConnected && lastWeightTime > 0)
      {
        startPendingObservation(flowAtStop, pressAtStop);
        pendingWeightAtStop = weightAtStop;
        pendingBrewTimeS = brewSecs;
      }
      else
      {
        pendingObservation = false;
      }
    }
    brewTimerActive = false;
  }
}
