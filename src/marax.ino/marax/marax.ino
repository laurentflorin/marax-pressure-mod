
#include <Arduino.h>
#include "wiring_private.h"
#include "EasyNextionLibrary.h"
#include <WiFiNINA.h>
#include <PubSubClient.h>
#include <dimmable_light.h>
#include <SD.h>

// === Required Libraries ===
// - EasyNextionLibrary     (Arduino Library Manager)
// - WiFiNINA               (Arduino Library Manager)
// - PubSubClient           (Arduino Library Manager)
// - dimmable_light         (Arduino Library Manager)
// - SD                     (Arduino built-in)
// - ArduinoBLE             (Arduino Library Manager)
// - AcaiaArduinoBLE        (manual install: https://github.com/baettigp/Acaia_Felicita_ArduinoBLE)
// Requires: baettigp/Acaia_Felicita_ArduinoBLE library
// https://github.com/baettigp/Acaia_Felicita_ArduinoBLE
#include <AcaiaArduinoBLE.h>

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Wifi
// nunununununununununununununununununununununununununununununun
// Define MQTT / WIFI
#define wifi_ssid ""
#define wifi_password ""

WiFiClient wifiClient;
bool wifiConnected = false;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Timers
// nunununununununununununununununununununununununununununununun
long serialMaraxUpdateMillis = millis();
long updateMqttTimer = millis();
long readSettigsRefreshTimer = millis();
unsigned long pageRefreshTimer = millis();
unsigned long refresh_timer = millis();
unsigned long activeBrewingStart = millis();

bool POWER_ON = false;

// Serial for Nex
Uart nexSerial(&sercom0, 5, 6, SERCOM_RX_PAD_1, UART_TX_PAD_0);
// EasyNex myNex(nexSerial);
EasyNex myNex(nexSerial);

void SERCOM0_Handler()
{
  nexSerial.IrqHandler();
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu MQTT Settings
// nunununununununununununununununununununununununununununununun
#define mqtt_server ""
#define mqtt_user ""
#define mqtt_password ""

#define brewtemp_topic "marax/sensor/brewtemp"
#define steamtemp_topic "marax/sensor/steamtemp"
#define steamtargettemp_topic "marax/sensor/steamtargettemp"
#define fastheat_topic "marax/sensor/fastheat_timer"
#define heatingElement_topic "marax/sensor/heatingelement"
#define debug_topic "marax/sensor/debug"
#define shots_topic "marax/sensor/shots"
#define power_topic "marax/sensor/power_state"
#define remoteProfileEnabled_topic "marax/sensor/pressureProfilingEnabled"
#define pressureProfileEnabled_topic "marax/sensor/remoteProfileEnabled"

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
char endMarker = '\n';
char rc;
int noSerialCount = 0;

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
int brewSwitchRelayPin = 10; // Relay to tell MaraxCard that a brew is active
int brewSwitchPin = 7;       // moved from 11 — D11 now used by SD card MOSI

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
int analogPressurePin = A1;

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
#define SD_CS_PIN 4
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
#define OLS_MIN_OBSERVATIONS 3
char activeProfileName[PROFILE_NAME_MAX_LEN] = "default";
char activeProfileFileStem[PROFILE_NAME_MAX_LEN] = "default";
char profileNames[MAX_PROFILES][PROFILE_NAME_MAX_LEN];
int profileCount = 0;
String profileListStr = "";
int selectedProfileIndex = 0;
float targetWeight = 36.0f;
bool sdReady = false;

AcaiaArduinoBLE scale(false);
bool scaleConnected = false;
float currentWeight = 0.0f;
float prevWeight = 0.0f;
unsigned long lastWeightTime = 0;
float flowRate = 0.0f;

#define OLS_WINDOW 10
const float DEFAULT_OLS_BETA[3] = {3.5f, 1.2f, 0.1f};
float olsBeta[3] = {DEFAULT_OLS_BETA[0], DEFAULT_OLS_BETA[1], DEFAULT_OLS_BETA[2]}; // Initial OLS coefficients: β0 intercept, β1 flow-rate coefficient, β2 pressure coefficient.
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
const float alpha = 0.97; // Low Pass Filter alpha (0 - 1 )
float filteredVal = 512.0;
float sensorVal;

DimmableLight pump(3);

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Setup
// nunununununununununununununununununununununununununununununun
void setup()
{
  // Set Marax Brew Switch Relay to off on startup
  pinMode(brewSwitchRelayPin, INPUT_PULLUP);
  delay(20);

  // AC DimmerStuff
  DimmableLight::setSyncPin(2);
  DimmableLight::begin();
  // Set pump 100% to allow mcu to still control
  pump.setBrightness(255);

  pinMode(brewSwitchPin, INPUT_PULLUP);
  // Marax Brew Switch Relay output
  pinMode(brewSwitchRelayPin, OUTPUT);

  // start serial port at 960                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                0 bps:
  // Reassign pins 5 and 6 to SERCOM alt
  pinPeripheral(5, PIO_SERCOM_ALT);
  pinPeripheral(6, PIO_SERCOM_ALT);

  delay(200);
  Serial1.begin(9600);
  myNex.begin(115200);
  delay(2000);
  Serial1.write(0x11);

  // Serial Marax
  memset(receivedCharsFromMarax, 0, numCharsSerialMarax);
  // check for the presence of the shield:
  if (WiFi.status() == WL_NO_SHIELD)
  {
    // Serial.println("Debug: NO WIFI");
    // Serial.println("WiFi shield not present");
    while (true)
      ;
  }
  String fv = WiFi.firmwareVersion();
  if (fv != "1.1.0")
  {
    // Serial.println("Please upgrade the firmware");
  }
  WiFi.setHostname("MaraXController");

  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED)
  {
    // Serial.println("Debug: NO d WIFI");
    delay(500);
    // Serial.print(".");
    WiFi.begin(wifi_ssid, wifi_password);
  }
  delay(2000);

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

  scaleConnected = scale.init();
}

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Main Loop
// nunununununununununununununununununununununununununununununun
void loop()
{
  updateScale();
  checkPendingObservation();

  if (!mqttClient.connected())
  {
    reconnect();
  }
  mqttClient.loop();
  myNex.NextionListen();

  getMaschineInput();
  updateDisplay();
  updateMqtt();
  brewDetect();

  liveData();
  pressureProfile();

  refresh_timer = millis();
}
void reconnect()
{
  // Loop until we're reconnected
  while (!mqttClient.connected())
  {
    // Attempt to connect
    if (mqttClient.connect("MaraXMod", mqtt_user, mqtt_password))
    {
      mqttClient.subscribe("marax/remoteProfile");
    }
    else
    {
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}
void callbackfun(char *topic, byte *payload, unsigned int length)
{

  String topicFromCallback = topic;
  if (topicFromCallback == "marax/remoteProfile")
  {
    int pos = 0;
    int payloadSize = sizeof(payload);
    char chars[payloadSize];
    memcpy(chars, payload, payloadSize);

    char temp[5];
    int charTempIndex = 0;
    int index = 0;
    for (int i = 0; i < strlen(chars); i++)
    {
      if (chars[i] != ',')
      {
        temp[charTempIndex] = chars[i];
        charTempIndex++;
      }
      else if (chars[i] == ',')
      {
        remoteProfileArray[index] = strtod(temp, NULL);
        charTempIndex = 0;
        index++;
      }
    }
    index = 0;
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
char *toCharArray(String str)
{
  return &str[0];
}

float predictedFinalWeight(float pressure)
{
  return currentWeight + olsBeta[0] + olsBeta[1] * flowRate + olsBeta[2] * pressure;
}

void fitOLS()
{
  if (olsCount < OLS_MIN_OBSERVATIONS)
  {
    return;
  }

  double XtX[3][3] = {};
  double Xty[3] = {};

  for (int i = 0; i < olsCount; i++)
  {
    double row[3] = {1.0, (double)olsX[i][0], (double)olsX[i][1]};
    for (int r = 0; r < 3; r++)
    {
      Xty[r] += row[r] * olsY[i];
      for (int c = 0; c < 3; c++)
      {
        XtX[r][c] += row[r] * row[c];
      }
    }
  }

  double A[3][4];
  for (int r = 0; r < 3; r++)
  {
    for (int c = 0; c < 3; c++)
    {
      A[r][c] = XtX[r][c];
    }
    A[r][3] = Xty[r];
  }

  for (int col = 0; col < 3; col++)
  {
    int pivot = col;
    for (int row = col + 1; row < 3; row++)
    {
      if (fabs(A[row][col]) > fabs(A[pivot][col]))
      {
        pivot = row;
      }
    }

    for (int c = 0; c <= 3; c++)
    {
      double tmp = A[col][c];
      A[col][c] = A[pivot][c];
      A[pivot][c] = tmp;
    }

    if (fabs(A[col][col]) < OLS_SINGULARITY_THRESHOLD)
    {
      return;
    }

    for (int row = 0; row < 3; row++)
    {
      if (row == col)
      {
        continue;
      }

      double factor = A[row][col] / A[col][col];
      for (int c = col; c <= 3; c++)
      {
        A[row][c] -= factor * A[col][c];
      }
    }
  }

  for (int i = 0; i < 3; i++)
  {
    olsBeta[i] = (float)(A[i][3] / A[i][i]);
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
  for (int i = 0; i < 3; i++)
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

  for (int i = 0; i < 3; i++)
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

  for (int i = 0; i < 3; i++)
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

// Gets "live" Info  during brew
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
  // Nano 33 IoT ADC:
  //  - 3.3V reference
  //  - analogRead() range 0..1023 in this codebase
  
  const float sensorMinV = 0.4f;
  const float sensorMaxV = 2.4f;
  const float sensorMaxBar = 12.0f;

  sensorVal = (float)analogRead(A1);
  filteredVal = (alpha * filteredVal) + ((1.0 - alpha) * sensorVal);
  voltage = (filteredVal / 1024.0f) * 3.3f;

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
    // Set Controller to sleep if Machine is off
    else if (!POWER_ON && !displayIsInSleep)
    {
      myNex.writeStr("page home");
      myNex.writeNum("sleep", 1);
      displayIsInSleep = true;
    }

    // Send global vars always
    myNex.writeNum("brewTemp", brewTemp);
    myNex.writeNum("steamTemp", steamTemp);
    myNex.writeNum("targetWeight", (int)targetWeight);

    currentPageId = myNex.readNumber("dp");

    // Save Settings im Page changes
    // Refresh Pages that only need one time refresh
    if (currentPageId != lastPageId)
    {
      lastPageId = currentPageId;
    }

    // We Changed to More Settings
    if (currentPageId == 4278190086 || currentPageId == 6)
    {

      myNex.writeNum("tarsteam.val", steamTargetTemp);
      delay(5);
      myNex.writeNum("fastheattimer.val", fastHeatingCountDown);
      delay(5);
      myNex.writeNum("heatingel.val", heatingElementOn);
      delay(5);
    }

    // Dont try to read setting when we are brewing to save time
    if (currentPageId != 4278190082 || currentPageId != 2)
    {
      readSettigs();
    }

    // Cleaing Mode Settings
    if (currentPageId == 4278190085 || currentPageId == 5)
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
  pump.setBrightness(pumpValue);
}

void pressureProfile()
{
  // Run pressure profiling whenever either local or remote pressure profiling is enabled.
  // The source of the profile values is selected in readSettigs():
  // - remoteProfilingEnabled => SD profile values from selectedProfile
  // - pressureProfilingEnabled => manual values from the display
  if (brewActive && pressureProfilingEnabled)
  {
    if (scaleConnected && targetWeight > 0.0f)
    {
      float predicted = predictedFinalWeight();
      if (predicted >= targetWeight)
      {
        pump.setBrightness(0);
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
      // AT this Point Brewing is done
      myNex.writeNum("n0.pco", 1535);
      myNex.writeNum("n1.pco", 1535);
      myNex.writeNum("setbar.val", 0);
      pump.setBrightness(0);
    }
  }
  // The actual pressure profile values are already selected in readSettigs():
  // - remoteProfilingEnabled => SD profile values from selectedProfile
  // - pressureProfilingEnabled => manual values from the display
  // When both flags are false, this branch is skipped entirely.
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
      brewTimer(true); // nextion timer start
      myNex.writeNum("pBrew.pic", 25);
      brewActive = true;
    }
  }
  else
  {
    brewActive = false;
    brewTimer(false);
    myNex.writeNum("pBrew.pic", 8);
    pump.setBrightness(255);

    // Reset cleaningRunActive
    if (cleaningRunActive)
    {
      myNex.writeNum("cleanTimer", 0);
    }
    cleaningRunActive = false;
  }
}

// Function to get the state of the brew switch button
bool brewState()
{
  // Brewswitch Pin is 1 in Off Postion and when on is 0
  // Only of the Machine is On and the Leaver is up we set brew to active
  // Power Led is connected to gnd 0 -> ON 1-> OFF
  if (digitalRead(brewSwitchPin) == LOW)
  {
    // Give the Data back to Marax MCU to let it know we are making coffee
    //  The Mcu will heat to keep temp and for better temps
    // Write to NC Relay
    digitalWrite(brewSwitchRelayPin, false);
    return true;
  }
  else
  {
    digitalWrite(brewSwitchRelayPin, true);
    return false;
  }
}

// C1.19,116,124,095,0560,0
void getMaschineInput()
{

  while (Serial1.available())
  {
    rc = Serial1.read();
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

      // Make sure we have valid data
      if (!receivedCharsFromMarax[25])
      {
        // Marax Brew Temp
        if (receivedCharsFromMarax[14] && receivedCharsFromMarax[15] && receivedCharsFromMarax[16])
        {
          String temp = String(receivedCharsFromMarax[14]) + String(receivedCharsFromMarax[15]) + String(receivedCharsFromMarax[16]);
          brewTemp = temp.toInt();
        }
        // Marax Steam Temp
        if (receivedCharsFromMarax[6] && receivedCharsFromMarax[7] && receivedCharsFromMarax[8])
        {
          String temp = String(receivedCharsFromMarax[6]) + String(receivedCharsFromMarax[7]) + String(receivedCharsFromMarax[8]);
          steamTemp = temp.toInt();
        }
        // Marax Target Temp
        if (receivedCharsFromMarax[10] && receivedCharsFromMarax[11] && receivedCharsFromMarax[12])
        {
          String temp = String(receivedCharsFromMarax[10]) + String(receivedCharsFromMarax[11]) + String(receivedCharsFromMarax[12]);
          steamTargetTemp = temp.toInt();
        }
        // Marax BoostTimer
        if (receivedCharsFromMarax[18] && receivedCharsFromMarax[19] && receivedCharsFromMarax[20] && receivedCharsFromMarax[21])
        {
          String temp = String(receivedCharsFromMarax[18]) + String(receivedCharsFromMarax[19]) + String(receivedCharsFromMarax[20]) + String(receivedCharsFromMarax[21]);
          fastHeatingCountDown = temp.toInt();
        }
        // Marax Heat Mode
        if (receivedCharsFromMarax[23])
        {
          String temp = String(receivedCharsFromMarax[23]);
          heatingElementOn = temp.toInt();
        }
      }
    }
  }
  if (receivedCharsFromMarax[0] != NULL)
  {
    POWER_ON = true;
    noSerialCount = 0;
  }
  else
  {
    noSerialCount++;
    if (noSerialCount == 3000)
    {
      POWER_ON = false;
      brewTemp = 0;
      steamTemp = 0;
      steamTargetTemp = 0;
      fastHeatingCountDown = 0;
      heatingElementOn = 0;

      //Reset all MQTT Values to 0
      mqttClient.publish(brewtemp_topic, toCharArray(String(brewTemp)), true);
      mqttClient.publish(steamtemp_topic, toCharArray(String(steamTemp)), true);
      mqttClient.publish(steamtargettemp_topic, toCharArray(String(steamTargetTemp)), true);
      mqttClient.publish(fastheat_topic, toCharArray(String(fastHeatingCountDown)), true);
      mqttClient.publish(heatingElement_topic, toCharArray(String(heatingElementOn)), true);
      mqttClient.publish(shots_topic, toCharArray(String(shotCount)), true);
      mqttClient.publish(power_topic, toCharArray(String(POWER_ON)), true);
    }
  }

  // Get Serial Update
  if (millis() - serialMaraxUpdateMillis > 5000)
  {
    serialMaraxUpdateMillis = millis();
    memset(receivedCharsFromMarax, 0, numCharsSerialMarax);
    Serial1.write(0x11);
  }
}

void brewTimer(bool start)
{ // small function for easier brew timer start/stop
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
