#include <Arduino.h>
#include <EasyNextionLibrary.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SD.h>
#include <SPI.h>
#include <driver/uart.h>
#include <AcaiaArduinoBLE.h>

// === Required Libraries ===
// - EasyNextionLibrary     (Arduino Library Manager)
// - WiFi                   (built-in with ESP32 Arduino core)
// - PubSubClient           (Arduino Library Manager)
// - SD                     (Arduino built-in)
// - ArduinoBLE             (Arduino Library Manager)
// - AcaiaArduinoBLE        (manual install: https://github.com/baettigp/Acaia_Felicita_ArduinoBLE)
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
//   GPIO1   ← Pressure sensor output (ADC1_CH0)
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
#define PRESSURE_SENSOR_PIN   1   // ADC1_CH0 — safe to use with WiFi
#define AC_ZERO_CROSS_PIN     4
#define PUMP_PIN              5
#define BREW_SWITCH_PIN       6
#define BREW_RELAY_PIN        7
#define SD_CS_PIN            10
#define SD_MOSI_PIN          11
#define SD_SCK_PIN           12
#define SD_MISO_PIN          13

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
#define remoteProfileEnabled_topic    "marax/sensor/pressureProfilingEnabled"
#define pressureProfileEnabled_topic  "marax/sensor/remoteProfileEnabled"

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

float remoteProfileArray[50];

bool cleaningModeActive = 0;
bool cleaningRunActive = 0;
int cleaningShots = 0;
int cleaningShotsWater = 0;

int brewSwitchAnalogValue = 0;

uint32_t currentPageId;
int lastPageId;
bool displayIsInSleep = true;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Pressure Profile
// nunununununununununununununununununununununununununununununun
int t1p = 0, t1t = 0, t2p = 0, t2t = 0, t3p = 0, t3t = 0, t4p = 0, t4t = 0;
int t1pWave = 0, t2pWave = 0, t3pWave = 0, t4pWave = 0;

// SD / Profile globals
#define MAX_PROFILES 10
#define PROFILE_NAME_MAX_LEN 32
#define MAX_PATH_LEN 64
#define PROFILE_FIELD_COUNT 10
#define PROFILE_CSV_BUFFER_LEN 128
#define WEIGHT_STABLE_WINDOW_MS 5000  // how long weight must be stable before committing observation
#define WEIGHT_STABLE_THRESHOLD 0.2f  // max increase (g) over stability window
#define OBSERVATION_MAX_WAIT_MS 30000 // max time to wait for stability after brew stop
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
float targetWeight = 36.0f;
bool sdReady = false;

AcaiaArduinoBLE scale(false);
bool nected = false;
float currentWeight = 0.0f;
float prevWeight = 0.0f;
unsigned long lastWeightTime = 0;
float flowRate = 0.0f;
int lastScaleConnectedUi = -1; // -1 forces first UI update
unsigned long lastScaleReconnectAttemptMs = 0;
const unsigned long SCALE_RECONNECT_INTERVAL_MS = 5000;

#define OLS_WINDOW 30
const float DEFAULT_OLS_BETA[5] = {3.5f, 1.2f, 0.1f, 0.1f, 0.1f};
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
volatile uint8_t pumpBrightness = 255;  // 0-255, 255 = full power (no dimming)
hw_timer_t *acTimer = NULL;
portMUX_TYPE acTimerMux = portMUX_INITIALIZER_UNLOCKED;

void IRAM_ATTR zeroCrossISR() {
  // Zero-cross detected — start timer for TRIAC trigger delay
  if (pumpBrightness == 0) {
    return;  // Off, don't trigger
  }
  
  if (pumpBrightness == 255) {
    // Full power - trigger immediately with a pulse
    digitalWrite(PUMP_PIN, HIGH);
    delayMicroseconds(10);  // 10μs gate pulse
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
  delayMicroseconds(10);         // 10μs pulse
  digitalWrite(PUMP_PIN, LOW);
}

void setPumpBrightness(uint8_t brightness) {
  portENTER_CRITICAL(&acTimerMux);
  pumpBrightness = brightness;
  portEXIT_CRITICAL(&acTimerMux);
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Setup
// nunununununununununununununununununununununununununununununun
void setup()
{
  // USB Serial for debugging — open Serial Monitor at 9600
  Serial.begin(9600);
  delay(1000);
  Serial.println("[DEBUG] setup() start");

  // ESP32-S3 ADC is 12-bit (0–4095). Must set this before any analogRead().
  analogReadResolution(12);

  // Set Marax Brew Switch Relay to off on startup
  pinMode(BREW_RELAY_PIN, INPUT_PULLUP);
  delay(20);

  // AC Dimming: Zero-cross interrupt and TRIAC control
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  pinMode(AC_ZERO_CROSS_PIN, INPUT);
  
  // Timer for TRIAC trigger delay
  acTimer = timerBegin(1000000);  // 1MHz (1μs resolution)
  timerAttachInterrupt(acTimer, &triacTriggerISR);
  
  // Zero-cross interrupt
  attachInterrupt(digitalPinToInterrupt(AC_ZERO_CROSS_PIN), zeroCrossISR, RISING);
  
  setPumpBrightness(255);  // Full power - allow machine to control pump normally

  pinMode(BREW_SWITCH_PIN, INPUT_PULLUP);
  // Marax Brew Switch Relay output
  pinMode(BREW_RELAY_PIN, OUTPUT);

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
  // We initialize Serial2 directly instead of calling myNex.begin(),
  // because myNex.begin() would call Serial2.begin(baud) without pin
  // arguments and overwrite our custom pin assignment.
  Serial2.begin(9600, SERIAL_8N1, NEXTION_RX_PIN, NEXTION_TX_PIN);
  Serial.println("[DEBUG] Serial2 (Nextion) started at 9600");

  // Force a known value onto the display right now to verify display wiring
  myNex.writeStr("t0.txt", "BOOT");
  delay(2000);
  Serial1.write(0x11);
  Serial.println("[DEBUG] Sent 0x11 wakeup to machine");

  // Serial Marax
  memset(receivedCharsFromMarax, 0, numCharsSerialMarax);

  // WiFi — non-blocking, continue even if connection fails
  WiFi.setHostname("MaraXController");
  WiFi.begin(wifi_ssid, wifi_password);
  unsigned long wifiStartTime = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartTime < 10000))
  {
    delay(500);
  }
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  lastWifiReconnectAttemptMs = millis();

  // SD card init
  if (SD.begin(SD_CS_PIN))
  {
    sdReady = true;

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

    if (profileCount > 0 && loadProfile(profileNames[0]))
    {
      selectedProfileIndex = 0;
      loadBeta();
      loadObservations();
    }

    myNex.writeStr("profileList.txt", profileListStr.c_str());
    myNex.writeStr("actProftxt", activeProfileName);
    updateProfileSelectUi();
    updateProfileModeText();
  }

  // Initialize scale with auto-discovery (empty MAC = connect to any Felicita scale)
  // Skip initialization if MAC address is empty to avoid BLE stack issues
  if (strlen(SCALE_MAC_ADDRESS) > 0)
  {
    scaleConnected = scale.init(SCALE_MAC_ADDRESS);
    Serial.println(scaleConnected ? "[DEBUG] Scale connected" : "[DEBUG] Scale not found");
  }
  else
  {
    scaleConnected = false;
    Serial.println("[DEBUG] Scale init skipped - no MAC address configured");
  }
  updateScaleConnectionUi();
  lastScaleReconnectAttemptMs = millis();
  Serial.println("[DEBUG] setup() complete");
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Main Loop
// nunununununununununununununununununununununununununununununun
void loop()
{
  updateScale();
  tryReconnectScale();
  tryReconnectWifi();
  tryReconnectMqtt();
  checkPendingObservation();

  if (mqttClient.connected())
  {
    mqttClient.loop();
  }
  myNex.NextionListen();

  getMaschineInput();
  updateDisplay();
  updateMqtt();
  brewDetect();

  liveData();
  pressureProfile();

  refresh_timer = millis();
}

void callbackfun(char *topic, byte *payload, unsigned int length)
{
  String topicFromCallback = topic;
  if (topicFromCallback == "marax/remoteProfile")
  {
    int payloadSize = (int)length;
    char chars[payloadSize + 1]; // +1 for null terminator
    memcpy(chars, payload, payloadSize);
    chars[payloadSize] = '\0'; // null-terminate before any string operations

    char temp[8]; // large enough for float strings like "-12.50"
    int charTempIndex = 0;
    int index = 0;
    for (int i = 0; i <= payloadSize; i++) // include '\0' to flush last value
    {
      char c = chars[i];
      if (c != ',' && c != '\0')
      {
        if (charTempIndex < (int)sizeof(temp) - 1) // prevent temp overflow
        {
          temp[charTempIndex] = c;
          charTempIndex++;
        }
      }
      else if (charTempIndex > 0) // flush accumulated number on ',' or end of string
      {
        temp[charTempIndex] = '\0'; // null-terminate before strtod
        if (index < 50) // prevent remoteProfileArray out-of-bounds
        {
          remoteProfileArray[index] = strtod(temp, NULL);
        }
        charTempIndex = 0;
        index++;
      }
    }
  }
}

void updateMqtt()
{
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
}

const char *toCharArray(const String &str)
{
  return str.c_str();
}

float predictedFinalWeight(float flow, float pressure)
{
  return currentWeight + olsBeta[0] + olsBeta[1] * flow + olsBeta[2] * pressure + olsBeta[3] * flow * pressure + olsBeta[4] * pressure * pressure;
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
    double row[5] = {1.0, flow, pressure, flow * pressure, pressure * pressure};
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
    return false;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/profiles/%s", filename);
  File f = SD.open(path);
  if (!f)
  {
    return false;
  }

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
      t1pWave = map(t1p, 0, 10, 0, 164);
      break;
    case 2:
      t1t = atoi(token);
      break;
    case 3:
      t2p = atoi(token);
      t2pWave = map(t2p, 0, 10, 0, 164);
      break;
    case 4:
      t2t = atoi(token);
      break;
    case 5:
      t3p = atoi(token);
      t3pWave = map(t3p, 0, 10, 0, 164);
      break;
    case 6:
      t3t = atoi(token);
      break;
    case 7:
      t4p = atoi(token);
      t4pWave = map(t4p, 0, 10, 0, 164);
      break;
    case 8:
      t4t = atoi(token);
      break;
    case 9:
      targetWeight = atof(token);
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

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/models/%s_beta.csv", activeProfileFileStem);
  File f = SD.open(path);
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

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/models/%s_beta.csv", activeProfileFileStem);
  SD.remove(path);
  File f = SD.open(path, FILE_WRITE);
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

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/models/%s_data.csv", activeProfileFileStem);
  File f = SD.open(path);
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

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/models/%s_data.csv", activeProfileFileStem);
  bool exists = SD.exists(path);
  File f = SD.open(path, FILE_WRITE);
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
  int maxIndex = (profileCount > 0) ? profileCount - 1 : 0;
  int currentIndex = (selectedProfileIndex < 0) ? 0 : selectedProfileIndex;
  if (currentIndex >= profileCount)
    currentIndex = maxIndex;

  myNex.writeNum("profileMax", maxIndex);
  myNex.writeNum("selectedProfile", currentIndex);
}

void updateProfileModeText()
{
  if (pressureProfilingEnabled && remoteProfilingEnabled && sdReady)
  {
    myNex.writeStr("profileModeTxt", activeProfileName);
  }
  else if (pressureProfilingEnabled)
  {
    myNex.writeStr("profileModeTxt", "Manual");
  }
  else
  {
    myNex.writeStr("profileModeTxt", "None");
  }
}

void updateScaleConnectionUi()
{
  int connected = scaleConnected ? 1 : 0;
  if (connected != lastScaleConnectedUi)
  {
    myNex.writeNum("scaleConnected", connected);
    lastScaleConnectedUi = connected;
  }
}

void tryReconnectScale()
{
  if (scaleConnected)
  {
    return;
  }

  // Skip if no MAC address configured
  if (strlen(SCALE_MAC_ADDRESS) == 0)
  {
    return;
  }

  unsigned long now = millis();
  if (now - lastScaleReconnectAttemptMs < SCALE_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastScaleReconnectAttemptMs = now;
  // Auto-discover and connect to any available Felicita scale
  scaleConnected = scale.init(SCALE_MAC_ADDRESS);
  if (!scaleConnected)
  {
    flowRate = 0.0f;
  }
  updateScaleConnectionUi();
}

void tryReconnectWifi()
{
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
}

void tryReconnectMqtt()
{
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
    mqttClient.subscribe("marax/remoteProfile");
  }
}

void scanProfiles()
{
  profileCount = 0;
  profileListStr = "";

  if (!sdReady)
  {
    return;
  }

  File dir = SD.open("/profiles");
  if (!dir)
  {
    return;
  }

  while (profileCount < MAX_PROFILES)
  {
    File entry = dir.openNextFile();
    if (!entry)
    {
      break;
    }

    if (!entry.isDirectory())
    {
      String name = entry.name();
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
        profileCount++;
      }
    }
    entry.close();
  }
  dir.close();
}

void updateScale()
{
  if (!scaleConnected)
  {
    return;
  }

  if (!scale.isConnected())
  {
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
    pendingObservation = false;
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

  pendingObservation = false;

  float finalWeight = currentWeight;
  float extraWeight = finalWeight - pendingWeightAtStop;

  if (extraWeight < 0.0f || extraWeight > MAX_VALID_EXTRA_WEIGHT)
  {
    return;
  }
  if (finalWeight < MIN_VALID_FINAL_WEIGHT)
  {
    return;
  }

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
  saveBrewLog(finalWeight, pendingBrewTimeS, pendingFlowAtStop, pendingPressAtStop, extraWeight);
}

// Gets "live" Info during brew
void liveData()
{
  if (brewActive)
  {
    // Write Brew Temp
    myNex.writeNum("brewTemp.val", brewTemp);
    // Write BrewTimer
    myNex.writeNum("brewTime", (int)((millis() - activeBrewingStart) / 1000));
    // Write Live Pressure as normal number
    float pressure = getPressure();
    int pressureInt = pressure * 100;
    myNex.writeNum("barvar.val", pressureInt);
    // Map the pressure to wave pixels in graph
    barGraphValue = map(pressureInt, 0, 1000, 0, 164);
    myNex.writeNum("barwave.val", barGraphValue);

    if (scaleConnected)
    {
      myNex.writeNum("weightVar.val", (int)(currentWeight * 10));
      myNex.writeNum("flowVar.val", (int)(flowRate * 100));
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
    int previousProfileCount = profileCount;
    if (sdReady)
    {
      scanProfiles();
      if (previousProfileCount != profileCount)
      {
        updateProfileSelectUi();
      }
    }

    getPressure();
    pressureProfilingEnabled = myNex.readNumber("pPEnabled");
    remoteProfilingEnabled = myNex.readNumber("remoteEnabled");

    int idx = myNex.readNumber("selectedProfile");
    if (idx < 0)
      idx = 0;
    if (idx >= profileCount)
      idx = profileCount - 1;

    bool usePresetProfile = pressureProfilingEnabled && remoteProfilingEnabled && sdReady && idx >= 0 && idx < profileCount;
    if (usePresetProfile && idx != selectedProfileIndex)
    {
      selectedProfileIndex = idx;
      if (loadProfile(profileNames[selectedProfileIndex]))
      {
        loadBeta();
        loadObservations();
        myNex.writeStr("actProftxt", activeProfileName);
      }
    }
    else if (!usePresetProfile && pressureProfilingEnabled)
    {
      int temp = myNex.readNumber("t1p");
      if (temp != t1p)
      {
        t1p = temp;
        t1pWave = map(temp, 0, 10, 0, 164);
      }
      temp = myNex.readNumber("t2p");
      if (temp != t2p)
      {
        t2p = temp;
        t2pWave = map(temp, 0, 10, 0, 164);
      }
      temp = myNex.readNumber("t3p");
      if (temp != t3p)
      {
        t3p = temp;
        t3pWave = map(temp, 0, 10, 0, 164);
      }
      temp = myNex.readNumber("t4p");
      if (temp != t4p)
      {
        t4p = temp;
        t4pWave = map(temp, 0, 10, 0, 164);
      }

      t1t = myNex.readNumber("t1t");
      t2t = myNex.readNumber("t2t");
      t3t = myNex.readNumber("t3t");
      t4t = myNex.readNumber("t4t");

      // Manual profile mode also lets the user choose the target weight.
      int manualTargetWeight = myNex.readNumber("targetWeight");
      if (manualTargetWeight >= 0 && manualTargetWeight != (int)targetWeight)
      {
        targetWeight = (float)manualTargetWeight;
      }
    }

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
      myNex.writeNum("sleep", 0);
      displayIsInSleep = false;
    }
    else if (!POWER_ON && !displayIsInSleep)
    {
      myNex.writeStr("page home");
      myNex.writeNum("sleep", 1);
      displayIsInSleep = true;
    }

    static unsigned long lastDisplayDebugMs = 0;
    if (millis() - lastDisplayDebugMs > 5000) {
      Serial.print("[DEBUG] Display update — brewTemp:"); Serial.print(brewTemp);
      Serial.print(" steamTemp:"); Serial.print(steamTemp);
      Serial.print(" POWER_ON:"); Serial.println(POWER_ON);
      lastDisplayDebugMs = millis();
    }
    
    Serial.print("[NEXTION] Writing brewTemp="); Serial.println(brewTemp);
    myNex.writeNum("brewTemp.val", brewTemp);
    
    Serial.print("[NEXTION] Writing steamTemp="); Serial.println(steamTemp);
    myNex.writeNum("steamTemp.val", steamTemp);
    
    // Try reading back to verify communication
    int readBack1 = myNex.readNumber("brewTemp.val");
    Serial.print("[NEXTION] Read back brewTemp: "); Serial.println(readBack1);
    int readBack2 = myNex.readNumber("steamTemp.val");
    Serial.print("[NEXTION] Read back steamTemp: "); Serial.println(readBack2);
    myNex.writeNum("targetWeight", (int)targetWeight);
    updateScaleConnectionUi();

    currentPageId = myNex.readNumber("dp");

    if (currentPageId != lastPageId)
    {
      lastPageId = currentPageId;
    }

    if (currentPageId == 4278190086 || currentPageId == 6)
    {
      myNex.writeNum("tarsteam.val", steamTargetTemp);
      delay(5);
      myNex.writeNum("fastheattimer.val", fastHeatingCountDown);
      delay(5);
      myNex.writeNum("heatingel.val", heatingElementOn);
      delay(5);
    }

    if (currentPageId != 4278190082 && currentPageId != 2)
    {
      readSettigs();
    }

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

void setPressure(float targetValue)
{
  int pumpValue;
  float currentPressure = (getPressure() - 1.7f);
  float diff = targetValue - currentPressure;

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
    if (scaleConnected && targetWeight > 0.0f)
    {
      float predicted = predictedFinalWeight(flowRate, getPressure());
      if (predicted >= targetWeight)
      {
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
      myNex.writeNum("setbar.val", 0);
      setPumpBrightness(0);
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
      
      // If pressure profiling is disabled, set pump to full power immediately
      if (!pressureProfilingEnabled)
      {
        setPumpBrightness(255);
      }
    }
  }
  else
  {
    brewActive = false;
    brewTimer(false);
    myNex.writeNum("pBrew.pic", 8);
    setPumpBrightness(255);  // Return to full power for normal machine operation

    if (cleaningRunActive)
    {
      myNex.writeNum("cleanTimer", 0);
    }
    cleaningRunActive = false;
  }
}

bool brewState()
{
  if (digitalRead(BREW_SWITCH_PIN) == LOW)
  {
    digitalWrite(BREW_RELAY_PIN, false);
    return true;
  }
  else
  {
    digitalWrite(BREW_RELAY_PIN, true);
    return false;
  }
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

    if (!frameReceived) {
      Serial.print("[RAW] 0x");
      if ((uint8_t)rc < 0x10) Serial.print("0");
      Serial.print((uint8_t)rc, HEX);
      Serial.print(" (");
      if (rc >= 0x20 && rc < 0x7F) Serial.print(rc); else Serial.print('?');
      Serial.println(")");
    }
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
      Serial.print("[DEBUG] Machine frame: ");
      Serial.println(receivedCharsFromMarax);

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
      if (fieldCount > 1 && fields[1][0] != '\0')
      {
        brewTemp = atoi(fields[1]);
      }
      if (fieldCount > 2 && fields[2][0] != '\0')
      {
        steamTargetTemp = atoi(fields[2]);
      }
      if (fieldCount > 3 && fields[3][0] != '\0')
      {
        steamTemp = atoi(fields[3]);
      }
      if (fieldCount > 4 && fields[4][0] != '\0')
      {
        fastHeatingCountDown = atoi(fields[4]);
      }
      if (fieldCount > 5 && fields[5][0] != '\0')
      {
        heatingElementOn = atoi(fields[5]);
      }
      
      Serial.print("[DEBUG] Parsed - brewTemp:"); Serial.print(brewTemp);
      Serial.print(" steamTemp:"); Serial.print(steamTemp);
      Serial.print(" steamTarget:"); Serial.print(steamTargetTemp);
      Serial.print(" fastHeat:"); Serial.print(fastHeatingCountDown);
      Serial.print(" heating:"); Serial.println(heatingElementOn);
    }
  }
  
  // Check for machine power state based on fresh data and timeout
  if (newFrameThisIteration)
  {
    if (!POWER_ON)
    {
      Serial.println("[DEBUG] POWER_ON = true (machine data received)");
    }
    POWER_ON = true;
  }
  else if (POWER_ON && lastFrameReceivedMs > 0 && (millis() - lastFrameReceivedMs > 15000))
  {
    // No frame for 15 seconds — machine is off
    Serial.println("[DEBUG] POWER_ON = false (no serial for 15 seconds)");
    POWER_ON = false;
    brewTemp = 0;
    steamTemp = 0;
    steamTargetTemp = 0;
    fastHeatingCountDown = 0;
    heatingElementOn = 0;

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
  }

  if (millis() - serialMaraxUpdateMillis > 5000)
  {
    serialMaraxUpdateMillis = millis();
    memset(receivedCharsFromMarax, 0, numCharsSerialMarax);
    ndx = 0;
    
    int availBefore = Serial1.available();
    Serial.print("[DEBUG] Sending 0x11 poll to machine (Serial1.available before: ");
    Serial.print(availBefore);
    Serial.println(")");
    Serial1.write(0x11);
    
    // Check if bytes appear shortly after sending poll
    delay(100);
    int availAfter = Serial1.available();
    if (availAfter > 0)
    {
      Serial.print("[DEBUG] Got response! Serial1.available after 100ms: ");
      Serial.println(availAfter);
    }
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
      unsigned long now = millis();

      if (scaleConnected && lastWeightTime > 0)
      {
        pendingObservation = true;
        pendingObsTime = now;
        pendingFlowAtStop = flowAtStop;
        pendingPressAtStop = pressAtStop;
        pendingWeightAtStop = weightAtStop;
        pendingStabilityCheckWeight = weightAtStop;
        pendingStabilityCheckTime = now;
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
