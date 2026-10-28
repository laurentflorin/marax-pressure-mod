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
// Machine Data Format: CSV (field order as parsed by getMaschineInput())
//   Example frame: +1.10,103,128,077,0000,1,0
//   Field 0: Version/Mode (+1.10)
//   Field 1: Steam temperature (103 = 103°C)
//   Field 2: Steam target temperature (128 = 128°C, may be empty)
//   Field 3: Brew temperature (077 = 77°C)
//   Field 4: Fast heat countdown (0000)
//   Field 5: Heating element status (0 = off, 1 = on)
//   Field 6: Unknown
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
// Set to true to log BLE scanning/connecting on the serial monitor. Useful when
// the scale will not (re)connect.
#define SCALE_BLE_DEBUG false

WiFiClient wifiClient;
bool wifiConnected = false;
bool scaleConnected = false;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Timers
// nunununununununununununununununununununununununununununununun
long serialMaraxUpdateMillis = millis();
long updateMqttTimer = millis();
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
void updateBrewHeader();
bool readRawBrewSwitchOn();
bool readDebouncedBrewSwitchOn();
void writeBrewRelay(bool brewOn);
void resumeIdleBoilerFill();
void processPendingRecipeCommand();
float profileTotalSeconds();
float profileTargetPressureAt(float second);
int pressureToWave(float bar);
void writeBrewTimerColour(uint32_t colour);
void writeSetBarValue(int wave);
void scanRecipes();
bool loadRecipe(const char *stem);
bool selectRecipe(const char *stem);
void clearActiveRecipe();
bool isRecipeActive();
bool recipeInCharge();
void setRecipeMode(bool enabled);
void loadHomeSlots();
void updateHomeSlotsUi();
void writeRecipePanel();
void handleHomeTileTap();
void handleRecipeModeTap();
void publishRecipeState();

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

// Last-will topic: retained "online" while the ESP32 is connected, flipped to
// "offline" by the broker the moment the connection drops. Every discovered
// entity references it, so Home Assistant greys them out instead of showing a
// stale retained reading when the controller is unplugged or loses WiFi.
#define availability_topic       "marax/status"

// Recipe editing. The recipe body is the CSV that lives on the SD card, in
// both directions. Carrying one format end to end means the firmware never has
// to parse JSON: it validates the bytes with the reader it already has and
// writes them straight to the card. Every command is answered on the result
// topic, and the library is published as one retained topic per recipe. Which
// recipe sits on which home-screen tile is its own topic, because a tile can
// only hold one.
#define recipe_list_topic        "marax/recipe/list"
#define recipe_active_topic      "marax/recipe/active"
#define recipe_home_topic        "marax/recipe/home"
#define recipe_result_topic      "marax/recipe/result"
#define recipe_select_topic      "marax/recipe/select"
#define recipe_home_set_topic    "marax/recipe/home/set"
#define recipe_save_prefix       "marax/recipe/save/"
#define recipe_delete_prefix     "marax/recipe/delete/"
#define recipe_body_prefix       "marax/recipe/body/"
// "Brew by recipe" on or off, as ON / OFF so Home Assistant's switch takes the
// payloads without templates.
#define recipe_mode_topic        "marax/recipe/mode"
#define recipe_mode_set_topic    "marax/recipe/mode/set"
// Published on the active topic, and offered by the select entity, when no
// recipe is in charge. The brackets cannot occur in a file name, so it can
// never collide with a real recipe.
#define RECIPE_NONE_OPTION       "(none)"

// ─────────────────────────────────────────────────────────────────────────
// Home Assistant MQTT discovery
//
// With this enabled the controller publishes retained config payloads under
// homeassistant/<component>/marax/<id>/config on every MQTT (re)connect, and
// Home Assistant creates the entities itself — no YAML needed on the HA side.
// Comment out to keep publishing plain values without announcing them.
// ─────────────────────────────────────────────────────────────────────────
#define ENABLE_HA_DISCOVERY

#define ha_discovery_prefix      "homeassistant"
#define ha_device_id             "marax"

// PubSubClient's default 256-byte buffer would silently drop every payload
// bigger than that (publish() just returns false). The largest traffic here is
// a full recipe body and the recipe-select discovery payload, which carries
// one option per recipe on the card.
#define MQTT_BUFFER_SIZE         2048
// Recipe bodies are bounded so an oversized retained payload cannot be
// accepted and then fail to fit in the buffer on the way back out.
#define RECIPE_BODY_MAX_LEN      1024

PubSubClient mqttClient(mqtt_server, 1883, callbackfun, wifiClient);

// nunununununununununununununununununununununununununununununun
// nunununununununununununu CONSTS
// nunununununununununununununununununununununununununununununun
#define REFRESH_SCREEN_EVERY 150 // Screen refresh interval (ms)
// Nextion writes during a brew are throttled to this interval so UART2 traffic
// cannot pace the loop that runs pressure control and the weight cut-off.
#define LIVE_DATA_REFRESH_MS 100

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

int shotCount = 0;

float voltage;
uint32_t barGraphValue;

bool pressureProfilingEnabled = false;
bool remoteProfilingEnabled = false;

bool cleaningModeActive = 0;
bool cleaningRunActive = 0;
int cleaningShots = 0;
int cleaningShotsWater = 0;

bool brewSwitchStableOn = false;
bool brewSwitchLastRawOn = false;
bool brewSwitchDebounceInitialized = false;
unsigned long brewSwitchLastChangeMs = 0;

uint32_t currentPageId = 0;
uint32_t lastPageId = 0;
bool displayIsInSleep = true;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Pressure curve
// nunununununununununununununununununununununununununununununun
// Every recipe carries a pressure curve: a polyline. Each point anchors a
// target pressure at an absolute time from the start of the shot, and `jump`
// says how the curve reaches that point:
//
//   ramp — interpolate linearly from the previous point
//   jump — hold the previous pressure until this point's time, then step to it
//
// Together they express everything from a classic four-step staircase (every
// point a jump) to a smooth declining curve (every point a ramp), including
// mixtures: hold 3 bar, jump straight to 8, hold, then ramp down to 6.
#define MAX_PROFILE_POINTS 16
#define PROFILE_MAX_BAR 12.0f
#define PROFILE_MAX_SECONDS 600.0f
// Two points is the minimum that describes a span of time; one point has no
// duration and would leave the pump with nothing to follow.
#define PROFILE_MIN_POINTS 2

struct ProfilePoint
{
  float seconds;  // absolute time from the start of the shot
  float bar;      // target pressure at that moment
  bool jump;      // true: step at `seconds`, false: ramp in from the point before
};

// The active recipe's curve — what the pump follows while brewing by recipe.
ProfilePoint profilePoints[MAX_PROFILE_POINTS];
int profilePointCount = 0;

// SD and display globals
#define RECIPE_NAME_MAX_LEN 32
#define MAX_PATH_LEN 64
#define RECIPE_LINE_MAX_LEN 128
// How long readNextionNumber() waits for a `get` reply before giving up. The
// display answers in well under a millisecond at 115200 baud, so this is
// generous; it only elapses when the display is busy, unplugged, or asked for
// something that does not exist. Keep it well below the display refresh
// interval so a dead read cannot stall the loop.
#define NEXTION_READ_TIMEOUT_MS 150
// How long the next read waits for the late answer to a `get` that already
// timed out, so it can be discarded instead of being taken for its own. The
// display normally answers in well under a millisecond, so anything that has
// not arrived by now is gone for good; keeping the window short bounds what a
// dead display costs the loop.
#define NEXTION_RESYNC_DRAIN_MS 40
// Page indexes in the HMI, as `dp` reports them. Deleting a page in the
// Nextion Editor renumbers every page after it, so check these against the
// editor's Page panel (the first page is 0) after changing the page list. The
// serial log prints the current page as "page:" every ten seconds.
#define HOME_PAGE_ID 1
#define BREW_PAGE_ID 2
#define CLEANING_PAGE_ID 4
// Brew page graph: the waveform `s0` has object id 1, is 300 x 180 px with
// dis=100, and has two channels — channel 0 carries the measured pressure,
// channel 1 the target. One sample per x pixel, value 0..180 spanning 0..10
// bar. BREW_WAVEFORM_WIDTH must match the waveform's width in the HMI.
#define BREW_WAVEFORM_ID 1
#define BREW_WAVEFORM_TARGET_CHANNEL 1
#define BREW_WAVEFORM_WIDTH 300
#define BREW_WAVEFORM_HEIGHT 180
// Handshake timeouts for `addt` (transparent data mode): the display answers
// 0xFE when it is ready for the raw bytes and 0xFD once it has taken them all.
#define NEXTION_ADDT_READY_TIMEOUT_MS 200
#define NEXTION_ADDT_DONE_TIMEOUT_MS 500
#define WEIGHT_STABLE_WINDOW_MS 2000  // how long weight must be stable before committing observation
#define WEIGHT_STABLE_THRESHOLD 0.5f  // max increase (g) over stability window
#define OBSERVATION_MAX_WAIT_MS 5000 // max time to wait for stability after brew stop
// Set when the brew page's target-curve preview has to be (re)drawn: on
// entering the page, and when the recipe changes while it is open.
bool brewPreviewNeedsDraw = false;
uint32_t lastPreviewSignature = 0;
unsigned long lastRecipeScanMs = 0;
const unsigned long RECIPE_SCAN_INTERVAL_MS = 30000;
bool sdReady = false;
Preferences preferences;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Recipes
// nunununununununununununununununununununununununununununununun
// A recipe is everything needed to pull the same coffee twice: the grinder
// setting, the dose, the yield, the position of the Mara X temperature switch
// and its own pressure curve. Recipes live on the SD card in /recipes/*.csv,
// one `key,value` per line, so unknown keys can be ignored and the format can
// grow without breaking older firmware:
//
//   # marax recipe v1
//   name,Ethiopia Guji
//   grind,2.4
//   dose,18.0
//   yield,40.0
//   temp,II
//   point,0.0,3.0,ramp
//   point,8.0,9.0,jump
//
// Grind, dose and temperature are guidance for whoever is at the machine — the
// grinder and the temperature switch are not under the controller's control.
// While brewing by recipe, the yield is the brew-by-weight target and the
// curve is what the pump follows. With brew by recipe switched off, the
// machine brews as it would without this mod: full pump, no weight cut-off.
#define MAX_RECIPES 24
#define RECIPE_GRIND_MAX_LEN 16
#define RECIPE_DOSE_MAX_G 40.0f
#define RECIPE_YIELD_MAX_G 150.0f
#define RECIPE_TEMP_UNSET -1
#define RECIPE_HOME_SLOTS 3
// Which recipe sits on which home-screen tile. Kept apart from the recipes
// themselves so a tile can only ever hold one recipe, and moving a recipe onto
// a tile is a single write rather than a rewrite of whichever file held it
// before. Not a .csv, so scanRecipes() never takes it for a recipe.
#define RECIPE_HOME_SLOTS_PATH "/recipes/home_slots.txt"
// Home tile colours (RGB565): dark slate, and the recipe in charge lighter
// with green text.
#define RECIPE_TILE_PCO          65535
#define RECIPE_TILE_BCO           8518
#define RECIPE_TILE_PCO_ACTIVE    2016
#define RECIPE_TILE_BCO_ACTIVE   17036

struct RecipeInfo
{
  char stem[RECIPE_NAME_MAX_LEN];   // file name without .csv — the recipe's identity
  char name[RECIPE_NAME_MAX_LEN];   // shown on the machine and in Home Assistant
  char grind[RECIPE_GRIND_MAX_LEN];  // free text: "2.4", "14 clicks"
  float dose;                        // grams in, 0 when not given
  float yield;                       // grams out, 0 when not given
  int temp;                          // 0, 1, 2 for the 0 / I / II switch, or RECIPE_TEMP_UNSET
};

// Every valid recipe on the card, as of the last scanRecipes().
RecipeInfo recipes[MAX_RECIPES];
int recipeCount = 0;
// Filled in by parseRecipeLine() while a recipe body is read.
RecipeInfo recipeScratch;
// The recipe the machine is set up for. An empty stem means none is.
RecipeInfo activeRecipe;
char persistedRecipeStem[RECIPE_NAME_MAX_LEN] = "";
// The settings page's one switch. Off, the machine brews as a stock Mara X
// even with a recipe selected; the recipe is kept for when it goes back on.
bool recipeModeEnabled = true;
char homeSlotStems[RECIPE_HOME_SLOTS][RECIPE_NAME_MAX_LEN];
// Filled in by parseHomeSlots(), so a rejected update leaves the tiles alone.
char homeSlotScratch[RECIPE_HOME_SLOTS][RECIPE_NAME_MAX_LEN];
// Set whenever what the home tiles show may have changed; they are rewritten
// the next time the home page is open.
bool homeSlotsUiDirty = true;
// The tile the display reported a tap on (1-based), 0 when there is none.
// Written by nextionProcessRx(), acted on by handleHomeTileTap().
int pendingHomeTileTap = 0;
// The settings switch as the display reported it: 0 off, 1 on, -1 nothing
// new. Written by nextionProcessRx(), acted on by handleRecipeModeTap().
int pendingRecipeModeTap = -1;
// Shared read buffer for whole recipe files. A body is bounded by
// what fits a single MQTT message, so nothing on the card is bigger than this.
char sdTextBuffer[RECIPE_BODY_MAX_LEN];

// Kept here rather than up with the other MQTT globals because the struct
// sizes its members with RECIPE_NAME_MAX_LEN, defined just above.
// MQTT commands are recorded by the callback and carried out later from the
// main loop. The callback runs inside mqttClient.loop(); writing to the SD card
// or publishing from in there would re-enter the client and stall the loop at
// an arbitrary moment — including in the middle of a shot.
enum RecipeCommandKind
{
  RECIPE_CMD_NONE = 0,
  RECIPE_CMD_SELECT,
  RECIPE_CMD_SAVE,
  RECIPE_CMD_DELETE,
  RECIPE_CMD_HOME,
  RECIPE_CMD_MODE,
  RECIPE_CMD_TOO_LARGE  // recorded so the sender gets an answer either way
};

struct PendingRecipeCommand
{
  RecipeCommandKind kind;
  char stem[RECIPE_NAME_MAX_LEN];
  char body[RECIPE_BODY_MAX_LEN];
  unsigned int bodyLength;
};

PendingRecipeCommand pendingRecipeCommand = { RECIPE_CMD_NONE, "", "", 0 };
// Set whenever the recipe list, the active recipe, the home tiles or the
// recipe mode change, so the retained topics are refreshed from the main loop
// rather than from wherever the change happened to occur.
bool recipeStateDirty = true;
// Set when every recipe body has to be published again: after a (re)connect,
// and when a periodic scan finds the card changed underneath us. Single saves
// and deletes publish their own body directly.
bool recipeLibraryDirty = true;

// Set when a `get` went unanswered inside NEXTION_READ_TIMEOUT_MS. A 0x71
// reply carries a bare 4-byte value and no component name, so an answer that
// arrives after its request gave up is indistinguishable from the answer to
// the next request: every following read would return the previous
// component's value. readNextionNumber() clears the stale reply before it
// asks for anything else.
bool nextionReadDesynced = false;

FelicitaScale_NimBLE scale(SCALE_BLE_DEBUG);  // see SCALE_BLE_DEBUG above
float currentWeight = 0.0f;
float prevWeight = 0.0f;
unsigned long lastWeightTime = 0;
float flowRate = 0.0f;
int lastScaleConnectedUi = -1; // -1 forces first UI update
unsigned long lastScaleConnectedUiWriteMs = 0;
unsigned long lastScaleReconnectAttemptMs = 0;
// Idle gap between two non-blocking connection attempts (an attempt itself
// occupies the radio for up to ~8s). Short enough that the scale is picked up
// within seconds of being switched back on, long enough to leave WiFi air time.
const unsigned long SCALE_RECONNECT_INTERVAL_MS = 5000;
// The icon write is re-sent this often even when nothing changed, because a
// cross-page write fails silently while the home page is not loaded.
const unsigned long SCALE_UI_REFRESH_INTERVAL_MS = 5000;

bool pendingObservation = false;
unsigned long pendingObsTime = 0;
float pendingFlowAtStop = 0.0f;
float pendingPressAtStop = 0.0f;
float pendingWeightAtStop = 0.0f;
float pendingStabilityCheckWeight = 0.0f;
unsigned long pendingStabilityCheckTime = 0;
int pendingBrewTimeS = 0;

// nunununununununununununununununununununununununununununununun
// nunununununununununununu Remembered shot times
// nunununununununununununununununununununununununununununununun
// Without the scale a recipe's yield cannot be weighed, so the shot stops after
// as long as the same recipe last took to reach it by weight. With the scale
// connected the weight always decides and the remembered time is not used.
//
// A time is only remembered from a shot nothing was wrong with. One measured
// off a scale that was not tared, got bumped or went quiet would stop every
// later shot without the scale at the wrong moment, so the scale's readings are
// watched from the lever going up to the cut-off, and the first thing that
// looks wrong rules the shot out. The time remembered before it stays.
//
// Times live in NVS, filed under a hash of the recipe's file name together
// with a fingerprint of everything in the recipe that changes how long it
// takes. An edited recipe therefore has no time until the scale has measured
// it again.
//
// Shorter than this, something went wrong rather than the coffee ran fast.
#define SHOT_TIME_MIN_SECONDS 10.0f
// Nor longer than this: a puck that choked that badly is not a shot to repeat.
#define SHOT_TIME_MAX_SECONDS 120.0f
// What the scale may read as the lever goes up: drips, not a cup.
#define SHOT_TIME_MAX_START_WEIGHT_G 2.0f
// Faster than coffee comes out of the group, in either direction. A reading
// that moves faster was the cup being set down or lifted, a bump, or a tare.
// It also bounds the whole shot: the yield cannot arrive faster than this.
#define SHOT_TIME_MAX_FLOW_GPS 8.0f
// Allowed on top of that between two readings, for the scale's own jitter.
#define SHOT_TIME_WEIGHT_JITTER_G 1.0f
// The Felicita sends ~10 readings a second. A gap this long means the weight
// the cut-off acted on may have been seconds old.
#define SHOT_TIME_MAX_SAMPLE_GAP_MS 2000

struct RememberedShotTime
{
  uint32_t fingerprint;  // recipeShotFingerprint() of the recipe it was measured on
  float seconds;         // lever up to the weight cut-off
};

// The active recipe's remembered time, 0 when it has none.
float activeShotSeconds = 0.0f;
uint32_t activeRecipeFingerprint = 0;
// Why the shot running now cannot be trusted to time the recipe, or NULL while
// nothing has looked wrong.
const char *shotTimeDoubt = NULL;
// The scale's previous reading during the shot.
unsigned long shotLastSampleMs = 0;
float shotLastSampleWeight = 0.0f;
// Lever up to cut-off of a shot the weight stopped and that passed every
// check. Written to NVS when the lever comes down; 0 when there is nothing to
// write.
float shotTimeToRemember = 0.0f;

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
// The cold threshold used to be 0.5 bar, which is not enough to tell "boiler
// full" apart from ordinary brew-line pressure: residual pressure left over
// from the last shot and the thermal expansion of a heating HX both clear
// 0.5 bar on their own, and 0.5 bar is only ~100 ADC counts above the sensor's
// 0.4 V zero, so sensor tolerance and the uncalibrated ESP32-S3 ADC eat much of
// the margin. The result was a boiler-full latch during warm-up that cut the
// pump before the GiCar could finish an autofill, leaving the boiler unfilled
// until the next brew released the latch.
const float BOILER_FULL_BAR_COLD = 1.0f;
const float BOILER_FULL_BAR_HOT  = 5.0f;

// The cut-off has to be seen continuously for this long before it latches, so
// a single noisy ADC sample cannot kill a fill in progress.
const unsigned long BOILER_FULL_DEBOUNCE_MS = 400;

// How long the latch may hold without the pressure still justifying it. The
// dimmer sits between the GiCar and the pump, so opening it while the GiCar is
// not asking for the pump does nothing at all — re-arming costs nothing and
// lets a latch that was set in error heal itself instead of waiting for a brew.
const unsigned long BOILER_FULL_LATCH_TIMEOUT_MS = 45000;

// primePressureFilter() seeds the low-pass filter from one raw ADC sample, and
// updateIdlePumpControl() runs later in the same loop() iteration, so without
// this window the very first latch decision is made with no smoothing at all.
const unsigned long PRESSURE_SETTLE_MS = 1000;

// Idle pressure above which the boiler is considered full. Follows the two
// thresholds above: hot once steam is over 80°C, cold otherwise.
float boilerFullPressureBar = BOILER_FULL_BAR_COLD;

// Dimmer debug counters (global so brewDetect can seed them at brew start)
unsigned long lastDimmerDebugMs = 0;
uint32_t lastZeroCrossCountDebug = 0;
// Latch: once pressure cuts the pump in idle, stay off until the next brew
// ends or BOILER_FULL_LATCH_TIMEOUT_MS elapses without pressure to back it up
bool boilerFullLatch = false;
// millis() when the pressure first rose above the cut-off, 0 while it is below
unsigned long boilerOverPressureSinceMs = 0;
// millis() of the last moment the pressure justified the latch — the re-arm
// timeout is measured from here, so a latch backed by real pressure never expires
unsigned long boilerFullLatchedAtMs = 0;
// millis() of the last primePressureFilter() call, start of the settle window
unsigned long pressureFilterPrimedAtMs = 0;
// Latch: once a brew by recipe has been cut — the weight reached target minus
// the pressure-dependent offset, or without the scale the remembered time ran
// out — keep the pump off until the brew ends (lever released)
bool shotCutOff = false;
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
  boilerOverPressureSinceMs = 0;
  boilerFullLatchedAtMs = 0;
  setPumpBrightness(255);
}

void primePressureFilter()
{
  sensorVal = (float)analogRead(PRESSURE_SENSOR_PIN);
  filteredVal = sensorVal;
  voltage = (filteredVal / 4096.0f) * 3.3f;
  // Opens the settle window: this single sample must not be trusted to latch
  // the boiler-full cut-off before the low-pass filter has converged.
  pressureFilterPrimedAtMs = millis();
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
  // The default 256-byte RX buffer overflows when the display sends a burst
  // (touch events plus `get` replies) while the loop is busy with SD or BLE
  // work, and a truncated packet desynchronises the parser.
  Serial2.setRxBufferSize(1024);
  Serial2.begin(115200, SERIAL_8N1, NEXTION_RX_PIN, NEXTION_TX_PIN);
  Serial.println("[DEBUG] Serial2 (Nextion) started at 115200");

  // Nextion's default bkcmd=2 returns a 4-byte error packet for every command
  // that fails — which includes every write to a component that is not on the
  // page currently loaded, something updateDisplay() does on each refresh.
  // That junk used to pile up in front of the 0x65 touch events. bkcmd=0
  // silences it; data returns (0x70/0x71 for `get`) are not affected.
  sendNextionCommand("bkcmd=0");

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

  // Re-send bkcmd: the display shares the power rail with the ESP32, so the
  // first attempt above may have been sent before it finished booting.
  sendNextionCommand("bkcmd=0");

  Serial1.write(0x11);
  Serial.println("[DEBUG] Sent 0x11 wakeup to machine");

  preferences.begin("marax", false);
  loadPersistedRecipe();

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
  if (!mqttClient.setBufferSize(MQTT_BUFFER_SIZE))
  {
    Serial.println("[MQTT] setBufferSize failed — discovery payloads will not fit");
  }

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

    if (!SD.exists("/recipes"))
      SD.mkdir("/recipes");
    if (!SD.exists("/logs"))
      SD.mkdir("/logs");

    if (!SD.exists("/logs/brews.csv"))
    {
      File f = SD.open("/logs/brews.csv", FILE_WRITE);
      if (f)
      {
        f.println("timestamp_ms,recipe,actual_weight,target_weight,brew_time_s,flow_rate_at_stop,pressure_at_stop,extra_weight");
        f.close();
      }
    }

    scanRecipes();
    lastRecipeScanMs = millis();
    loadHomeSlots();

    // Back to the recipe that was in charge before the power went.
    if (persistedRecipeStem[0] != '\0' && !loadRecipe(persistedRecipeStem))
    {
      Serial.print("[RECIPE] Could not restore '"); Serial.print(persistedRecipeStem);
      Serial.println("' — no recipe active");
      preferences.remove("recipe");
    }
  }
  else
  {
    Serial.println("[SD] FAILED to mount SD card — check wiring and card format (FAT32)");
    Serial.print("[SD]   CS=GPIO"); Serial.print(SD_CS_PIN);
    Serial.print("  SCK=GPIO"); Serial.print(SD_SCK_PIN);
    Serial.print("  MOSI=GPIO"); Serial.print(SD_MOSI_PIN);
    Serial.print("  MISO=GPIO"); Serial.println(SD_MISO_PIN);
  }

  // updateDisplay() keeps sending the mode from here on; this first write
  // saves the settings page from showing the HMI default meanwhile.
  myNex.writeNum("recipeMode", recipeModeEnabled ? 1 : 0);
  updateBrewHeader();

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
#ifndef DISABLE_WIFI_MQTT
  processPendingRecipeCommand();
#endif

  serviceNextionRx();
  handleHomeTileTap();
  handleRecipeModeTap();

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
#ifdef DISABLE_WIFI_MQTT
  (void)topic; (void)payload; (void)length;
#else
  if (topic == NULL)
  {
    return;
  }

  // One command at a time. Dropping the newer one is the safe choice: the
  // alternative is a queue that can fill up mid-shot.
  if (pendingRecipeCommand.kind != RECIPE_CMD_NONE)
  {
    Serial.println("[MQTT] Recipe command dropped — one is already pending");
    return;
  }

  RecipeCommandKind kind = RECIPE_CMD_NONE;
  const char *stem = NULL;

  if (strcmp(topic, recipe_select_topic) == 0)
  {
    kind = RECIPE_CMD_SELECT;
  }
  else if (strcmp(topic, recipe_home_set_topic) == 0)
  {
    kind = RECIPE_CMD_HOME;
  }
  else if (strcmp(topic, recipe_mode_set_topic) == 0)
  {
    kind = RECIPE_CMD_MODE;
  }
  else if (strncmp(topic, recipe_save_prefix, strlen(recipe_save_prefix)) == 0)
  {
    kind = RECIPE_CMD_SAVE;
    stem = topic + strlen(recipe_save_prefix);
  }
  else if (strncmp(topic, recipe_delete_prefix, strlen(recipe_delete_prefix)) == 0)
  {
    kind = RECIPE_CMD_DELETE;
    stem = topic + strlen(recipe_delete_prefix);
  }
  else
  {
    return;
  }

  if (length >= sizeof(pendingRecipeCommand.body))
  {
    pendingRecipeCommand.kind = RECIPE_CMD_TOO_LARGE;
    pendingRecipeCommand.bodyLength = 0;
    pendingRecipeCommand.body[0] = '\0';
    pendingRecipeCommand.stem[0] = '\0';
    return;
  }

  pendingRecipeCommand.stem[0] = '\0';
  if (stem != NULL)
  {
    strncpy(pendingRecipeCommand.stem, stem, sizeof(pendingRecipeCommand.stem) - 1);
    pendingRecipeCommand.stem[sizeof(pendingRecipeCommand.stem) - 1] = '\0';
  }

  memcpy(pendingRecipeCommand.body, payload, length);
  pendingRecipeCommand.body[length] = '\0';
  pendingRecipeCommand.bodyLength = length;
  // Written last: the main loop reads `kind` to decide whether the rest of the
  // struct is populated.
  pendingRecipeCommand.kind = kind;
#endif
}

void updateMqtt()
{
#ifndef DISABLE_WIFI_MQTT
  // Recipe changes are published as they happen rather than on the 5-second
  // tick, so the editor sees its own save land immediately.
  if (recipeStateDirty && mqttClient.connected() && !brewActive)
  {
    publishRecipeState();
    recipeStateDirty = false;
  }

  // The whole library is a burst of SD reads and retained publishes, so it is
  // held back while a shot runs like everything else here.
  if (recipeLibraryDirty && mqttClient.connected() && !brewActive)
  {
    publishRecipeLibrary();
    recipeLibraryDirty = false;
  }

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

void loadPersistedRecipe()
{
  recipeModeEnabled = preferences.getBool("recipe_mode", true);
  persistedRecipeStem[0] = '\0';
  String savedStem = preferences.getString("recipe", "");
  if (savedStem.length() > 0)
  {
    savedStem.toCharArray(persistedRecipeStem, sizeof(persistedRecipeStem));
    persistedRecipeStem[sizeof(persistedRecipeStem) - 1] = '\0';
  }
}

bool isBrewPage(uint32_t pageId)
{
  return pageId == (uint32_t)BREW_PAGE_ID ||
         pageId == (uint32_t)(0xFF000000 | BREW_PAGE_ID);
}

bool isHomePage(uint32_t pageId)
{
  return pageId == (uint32_t)HOME_PAGE_ID ||
         pageId == (uint32_t)(0xFF000000 | HOME_PAGE_ID);
}

// Single entry point for everything arriving on UART2.
//   * A 0x23 packet from a home tile or the recipe switch is recorded for
//     handleHomeTileTap() / handleRecipeModeTap().
//   * A 0x71 numeric reply is decoded into `capture`, when one is supplied.
//   * Any other byte is dropped so the stream resynchronises. The old handler
//     returned on the first unrecognised byte, which let a single stray byte
//     block every queued touch behind it indefinitely.
// Returns true once a numeric reply has been captured.
bool nextionProcessRx(uint32_t *capture)
{
  while (Serial2.available() > 0)
  {
    int head = Serial2.peek();

    if (head == 0x65)
    {
      if (Serial2.available() < 7)
      {
        return false;  // rest of the packet is still in flight
      }

      // Nothing acts on plain touch events any more — every control that
      // matters sends its own 0x23 packet — but they are still consumed
      // whole, so their bytes cannot be mistaken for the start of another
      // packet.
      uint8_t packet[7];
      Serial2.readBytes(packet, sizeof(packet));
      continue;
    }

    if (head == 0x23)
    {
      // `printh 23 02 <command> <value>` from a touch event in the HMI:
      //   'R' 1..3 — home tile rN was tapped
      //   'M' 0/1  — the settings page's "Brew by recipe" switch
      // Custom packets rather than plain touch events, so they do not depend
      // on the component ids the Nextion Editor happens to hand out.
      if (Serial2.available() < 4)
      {
        return false;
      }

      uint8_t packet[4];
      Serial2.readBytes(packet, sizeof(packet));
      if (packet[1] == 0x02 && packet[2] == 'R' &&
          packet[3] >= 1 && packet[3] <= RECIPE_HOME_SLOTS)
      {
        pendingHomeTileTap = packet[3];
      }
      else if (packet[1] == 0x02 && packet[2] == 'M' && packet[3] <= 1)
      {
        pendingRecipeModeTap = packet[3];
      }
      continue;
    }

    if (head == 0x71 && capture != NULL)
    {
      if (Serial2.available() < 8)
      {
        return false;
      }

      uint8_t packet[8];
      Serial2.readBytes(packet, sizeof(packet));
      if (packet[5] == 0xFF && packet[6] == 0xFF && packet[7] == 0xFF)
      {
        // 4-byte little-endian value.
        *capture = (uint32_t)packet[1] |
                   ((uint32_t)packet[2] << 8) |
                   ((uint32_t)packet[3] << 16) |
                   ((uint32_t)packet[4] << 24);
        return true;
      }
      continue;
    }

    Serial2.read();
  }

  return false;
}

void serviceNextionRx()
{
  nextionProcessRx(NULL);
}

// Waits for a single status byte from the display — the 0xFE / 0xFD handshake
// of `addt`. Only runs on the brew page, where no control sends anything, so
// a plain touch event arriving meanwhile is simply skipped.
bool nextionWaitForStatus(uint8_t expected, unsigned long timeoutMs)
{
  unsigned long startMs = millis();
  while (millis() - startMs < timeoutMs)
  {
    while (Serial2.available() > 0)
    {
      if (Serial2.peek() == 0x65)
      {
        if (Serial2.available() < 7)
        {
          break;  // rest of the touch packet is still in flight
        }

        uint8_t packet[7];
        Serial2.readBytes(packet, sizeof(packet));
        continue;
      }

      if ((uint8_t)Serial2.read() == expected)
      {
        return true;
      }
    }
    delay(1);
  }

  return false;
}

// Weight the pump is cut early by, so the drip-through after the pump stops
// lands the cup on target. Higher pressure at cut-off means more liquid still
// in the puck/group, hence a bigger offset. Bands are hard coded (bar → g).
float weightStopOffset(float pressure)
{
  if (pressure < 3.5f) return 0.2f;
  if (pressure < 4.5f) return 0.5f;
  if (pressure < 5.5f) return 0.75f;
  if (pressure < 6.5f) return 1.0f;
  if (pressure < 7.5f) return 1.5f;
  return 2.0f;
}

// Parses one curve line of a recipe:
//
//   point,<seconds>,<bar>[,ramp|jump]
//
// `seconds` is measured from the start of the shot, not a segment length.
// Returns false on a line that would corrupt the curve.
static bool parsePointLine(char *line)
{
  if (strtok(line, ",") == NULL)
  {
    return false;
  }

  char *secondsToken = strtok(NULL, ",");
  char *barToken = strtok(NULL, ",");
  char *modeToken = strtok(NULL, ",");
  if (secondsToken == NULL || barToken == NULL)
  {
    return false;
  }
  if (profilePointCount >= MAX_PROFILE_POINTS)
  {
    Serial.println("[SD] recipe has too many points");
    return false;
  }

  ProfilePoint &point = profilePoints[profilePointCount];
  point.seconds = constrain((float)atof(secondsToken), 0.0f, PROFILE_MAX_SECONDS);
  point.bar = constrain((float)atof(barToken), 0.0f, PROFILE_MAX_BAR);
  // Ramping is the default; only an explicit `jump` steps.
  point.jump = (modeToken != NULL && strcmp(modeToken, "jump") == 0);

  // profileTargetPressureAt() walks the points in order, so an unsorted file
  // would silently skip segments. Reject it instead.
  if (profilePointCount > 0 && point.seconds < profilePoints[profilePointCount - 1].seconds)
  {
    Serial.println("[SD] recipe points are not in ascending time order");
    return false;
  }

  profilePointCount++;
  return true;
}

// Recipe names become file names and MQTT topic levels, so only
// characters that cannot escape their directory are allowed. Rejecting '.'
// outright rules out '..' and any extension games without needing to reason
// about path traversal, and ',' can never break the comma-separated lists.
static bool sanitizeRecipeStem(const char *raw, char *out, size_t outLen)
{
  if (raw == NULL)
  {
    return false;
  }

  size_t length = strlen(raw);
  // Room for the stem plus the ".csv" the caller appends.
  if (length == 0 || length + 5 > outLen)
  {
    return false;
  }

  for (size_t i = 0; i < length; i++)
  {
    char c = raw[i];
    bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!allowed)
    {
      return false;
    }
    out[i] = c;
  }
  out[length] = '\0';
  return true;
}

// Accepts the temperature switch position the way it is printed on the
// machine (0, I, II) as well as plain digits. Anything else means "not given".
static int parseRecipeTemp(const char *value)
{
  if (strcmp(value, "0") == 0) return 0;
  if (strcmp(value, "I") == 0 || strcmp(value, "1") == 0) return 1;
  if (strcmp(value, "II") == 0 || strcmp(value, "2") == 0) return 2;
  return RECIPE_TEMP_UNSET;
}

static const char *recipeTempLabel(int temp)
{
  if (temp == 0) return "0";
  if (temp == 1) return "I";
  if (temp == 2) return "II";
  return "-";
}

// Starts recipeScratch afresh for the file `stem`. A file without a name line
// is shown under its file name.
static void resetRecipeScratch(const char *stem)
{
  memset(&recipeScratch, 0, sizeof(recipeScratch));
  strncpy(recipeScratch.stem, stem, sizeof(recipeScratch.stem) - 1);
  strncpy(recipeScratch.name, stem, sizeof(recipeScratch.name) - 1);
  recipeScratch.temp = RECIPE_TEMP_UNSET;
}

// Copies a text value without its surrounding blanks, truncating rather than
// overflowing. A double quote or backslash would end or escape the string in
// the display command that shows it, so they are swapped for harmless look-
// alikes here, once, instead of at every place the text is sent.
static void copyTrimmedValue(char *out, size_t outLen, const char *value)
{
  while (*value == ' ' || *value == '\t')
  {
    value++;
  }
  strncpy(out, value, outLen - 1);
  out[outLen - 1] = '\0';
  size_t n = strlen(out);
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
  {
    out[--n] = '\0';
  }
  for (size_t i = 0; i < n; i++)
  {
    if (out[i] == '"') out[i] = '\'';
    if (out[i] == '\\') out[i] = '/';
  }
}

// One line of a recipe. Curve lines go to parsePointLine(), the recipe keys
// fill recipeScratch. Text values run to the end of the line, so a grind
// setting written "2,4" survives intact. An unknown key is ignored rather
// than rejected, so the format can grow without breaking older firmware.
static bool parseRecipeLine(char *line)
{
  char *comma = strchr(line, ',');
  if (comma == NULL)
  {
    return true;
  }

  if (strncmp(line, "point,", 6) == 0)
  {
    return parsePointLine(line);
  }

  *comma = '\0';
  const char *key = line;
  const char *value = comma + 1;

  if (strcmp(key, "name") == 0)
  {
    char name[RECIPE_NAME_MAX_LEN];
    copyTrimmedValue(name, sizeof(name), value);
    if (name[0] != '\0')
    {
      strncpy(recipeScratch.name, name, sizeof(recipeScratch.name));
    }
  }
  else if (strcmp(key, "grind") == 0)
  {
    copyTrimmedValue(recipeScratch.grind, sizeof(recipeScratch.grind), value);
  }
  else if (strcmp(key, "dose") == 0)
  {
    recipeScratch.dose = constrain((float)atof(value), 0.0f, RECIPE_DOSE_MAX_G);
  }
  else if (strcmp(key, "yield") == 0)
  {
    recipeScratch.yield = constrain((float)atof(value), 0.0f, RECIPE_YIELD_MAX_G);
  }
  else if (strcmp(key, "temp") == 0)
  {
    char temp[4];
    copyTrimmedValue(temp, sizeof(temp), value);
    recipeScratch.temp = parseRecipeTemp(temp);
  }
  return true;
}

// Splits a recipe body into lines and hands each to parseRecipeLine(). Returns
// false on the first line that would corrupt the curve, or on a line too long
// for the parser's buffer — that is malformed, not something to truncate
// silently. The caller checks the point count.
static bool parseBodyLines(const char *body)
{
  const char *cursor = body;
  char line[RECIPE_LINE_MAX_LEN];

  // A UTF-8 byte-order mark from a desktop editor. It is invisible, and would
  // otherwise make the first key unrecognisable.
  if ((uint8_t)cursor[0] == 0xEF && (uint8_t)cursor[1] == 0xBB && (uint8_t)cursor[2] == 0xBF)
  {
    cursor += 3;
  }

  while (*cursor != '\0')
  {
    size_t n = 0;
    while (*cursor != '\0' && *cursor != '\n' && n < sizeof(line) - 1)
    {
      line[n++] = *cursor++;
    }
    if (*cursor != '\0' && *cursor != '\n')
    {
      return false;
    }
    if (*cursor == '\n')
    {
      cursor++;
    }
    line[n] = '\0';

    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
    {
      line[--n] = '\0';
    }

    if (n == 0 || line[0] == '#')
    {
      continue;
    }
    if (!parseRecipeLine(line))
    {
      return false;
    }
  }
  return true;
}

// Parses a body with the real reader, then puts back the curve that was
// loaded before. Validating with the same code that will later read the file
// off the card is the point: nothing can be accepted here and rejected on
// reload. The caller resets recipeScratch first and finds the result there;
// the pump's curve is untouched either way.
static bool validateRecipeBody(const char *body)
{
  ProfilePoint savedPoints[MAX_PROFILE_POINTS];
  int savedCount = profilePointCount;
  memcpy(savedPoints, profilePoints, sizeof(savedPoints));

  profilePointCount = 0;
  bool ok = parseBodyLines(body) && profilePointCount >= PROFILE_MIN_POINTS;

  memcpy(profilePoints, savedPoints, sizeof(profilePoints));
  profilePointCount = savedCount;
  return ok;
}

// "grind_a,grind_b," — one recipe file name per home tile, left to right, an
// empty entry for an unused tile. Every name has to be a valid file name, but
// the recipe need not exist: a tile whose recipe is missing is simply not
// shown. The result lands in homeSlotScratch so a rejected update changes
// nothing.
static bool parseHomeSlots(const char *text)
{
  memset(homeSlotScratch, 0, sizeof(homeSlotScratch));

  int slot = 0;
  const char *cursor = text;
  while (true)
  {
    const char *end = cursor;
    while (*end != '\0' && *end != ',' && *end != '\n' && *end != '\r')
    {
      end++;
    }

    char field[RECIPE_NAME_MAX_LEN];
    size_t length = (size_t)(end - cursor);
    if (length >= sizeof(field))
    {
      return false;
    }
    memcpy(field, cursor, length);
    field[length] = '\0';

    if (field[0] != '\0')
    {
      if (slot >= RECIPE_HOME_SLOTS ||
          !sanitizeRecipeStem(field, homeSlotScratch[slot], sizeof(homeSlotScratch[slot])))
      {
        return false;
      }
    }
    slot++;

    if (*end != ',')
    {
      break;
    }
    cursor = end + 1;
  }
  return true;
}

// Grams as the display shows them: "18" for a whole number, "18.5" otherwise.
static void formatGrams(float grams, char *out, size_t outLen)
{
  float rounded = (float)(int)(grams + 0.5f);
  if (grams - rounded < 0.05f && rounded - grams < 0.05f)
  {
    snprintf(out, outLen, "%d", (int)rounded);
  }
  else
  {
    snprintf(out, outLen, "%.1f", grams);
  }
}

// The text of a home tile, one fact per line — the tile is a multi-line
// button, and `\r` is the display's own line-break escape:
//
//   Ethiopia Guji
//   18g > 40g
//   Grind 2.4
//   Temp II
//
// Lines for values the recipe does not give are left out rather than shown
// empty.
static void buildHomeTileText(const RecipeInfo &recipe, char *out, size_t outLen)
{
  char dose[12];
  char yieldText[12];
  formatGrams(recipe.dose, dose, sizeof(dose));
  formatGrams(recipe.yield, yieldText, sizeof(yieldText));

  char weightLine[32] = "";
  char grindLine[32] = "";
  char tempLine[16] = "";
  if (recipe.dose > 0.0f && recipe.yield > 0.0f)
  {
    snprintf(weightLine, sizeof(weightLine), "\\r%sg > %sg", dose, yieldText);
  }
  else if (recipe.yield > 0.0f)
  {
    snprintf(weightLine, sizeof(weightLine), "\\rOut %sg", yieldText);
  }
  if (recipe.grind[0] != '\0')
  {
    snprintf(grindLine, sizeof(grindLine), "\\rGrind %s", recipe.grind);
  }
  if (recipe.temp != RECIPE_TEMP_UNSET)
  {
    snprintf(tempLine, sizeof(tempLine), "\\rTemp %s", recipeTempLabel(recipe.temp));
  }

  snprintf(out, outLen, "%s%s%s%s", recipe.name, weightLine, grindLine, tempLine);
}

// Reads a whole file into `buffer`. Refuses rather than truncates one that
// does not fit, since half a curve is worse than none.
static bool readSdTextFile(const char *path, char *buffer, size_t bufferLen)
{
  File f = SD.open(path);
  if (!f)
  {
    return false;
  }

  size_t size = f.size();
  if (size >= bufferLen)
  {
    f.close();
    Serial.print("[SD] "); Serial.print(path); Serial.println(" is too large to read");
    return false;
  }

  size_t n = f.read((uint8_t *)buffer, size);
  f.close();
  buffer[n] = '\0';
  return n == size;
}

// On the ESP32 core FILE_WRITE is "w", so this replaces the file rather than
// appending to it.
static bool writeSdTextFile(const char *path, const char *body)
{
  File f = SD.open(path, FILE_WRITE);
  if (!f)
  {
    Serial.print("[SD] cannot open "); Serial.print(path); Serial.println(" for writing");
    return false;
  }

  size_t expected = strlen(body);
  size_t written = f.print(body);
  // Every reader here takes a line at a time, so a body that does not end in
  // a newline would lose its last line on the way back in.
  if (expected > 0 && body[expected - 1] != '\n')
  {
    f.print('\n');
  }
  f.close();

  if (written != expected)
  {
    Serial.print("[SD] short write to "); Serial.println(path);
    return false;
  }
  return true;
}

static int findRecipeIndex(const char *stem)
{
  for (int i = 0; i < recipeCount; i++)
  {
    if (strcmp(recipes[i].stem, stem) == 0)
    {
      return i;
    }
  }
  return -1;
}

bool isRecipeActive()
{
  return activeRecipe.stem[0] != '\0';
}

// Reads every recipe on the card. Each one is checked with the full parser, so
// a file that could not be brewed never shows up on a home tile or in Home
// Assistant; the curve the pump follows is left alone throughout.
void scanRecipes()
{
  recipeCount = 0;
  if (!sdReady)
  {
    return;
  }

  File dir = SD.open("/recipes");
  if (!dir)
  {
    Serial.println("[SD] scanRecipes: cannot open /recipes");
    return;
  }

  while (recipeCount < MAX_RECIPES)
  {
    File entry = dir.openNextFile();
    if (!entry)
    {
      break;
    }

    String name = entry.name();
    bool isDir = entry.isDirectory();
    entry.close();

    int slashIndex = name.lastIndexOf('/');
    if (slashIndex >= 0)
    {
      name = name.substring(slashIndex + 1);
    }
    if (isDir || !name.endsWith(".csv"))
    {
      continue;
    }

    char stem[RECIPE_NAME_MAX_LEN];
    if (!sanitizeRecipeStem(name.substring(0, name.length() - 4).c_str(), stem, sizeof(stem)))
    {
      Serial.print("[SD] scanRecipes: skipped '"); Serial.print(name);
      Serial.println("' — file names may only use letters, digits, _ and -");
      continue;
    }

    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "/recipes/%s.csv", stem);
    resetRecipeScratch(stem);
    if (!readSdTextFile(path, sdTextBuffer, sizeof(sdTextBuffer)) ||
        !validateRecipeBody(sdTextBuffer))
    {
      Serial.print("[SD] scanRecipes: skipped unreadable recipe '"); Serial.print(name); Serial.println("'");
      continue;
    }

    recipes[recipeCount++] = recipeScratch;
  }
  dir.close();

  Serial.print("[SD] scanRecipes: found "); Serial.print(recipeCount); Serial.println(" recipes");
}

// Cheap fingerprint of what the recipes and home tiles show, so the periodic
// rescan can tell whether anything on the card actually changed.
uint32_t recipeLibrarySignature()
{
  uint32_t signature = 2166136261u;
  auto mix = [&signature](const char *text) {
    for (const char *c = text; *c != '\0'; c++)
    {
      signature = (signature ^ (uint8_t)*c) * 16777619u;
    }
    signature = (signature ^ 0xFFu) * 16777619u;
  };

  for (int i = 0; i < recipeCount; i++)
  {
    mix(recipes[i].stem);
    mix(recipes[i].name);
    mix(recipes[i].grind);
    signature = signature * 31u + (uint32_t)(recipes[i].dose * 10.0f);
    signature = signature * 31u + (uint32_t)(recipes[i].yield * 10.0f);
    signature = signature * 31u + (uint32_t)(recipes[i].temp + 1);
  }
  for (int slot = 0; slot < RECIPE_HOME_SLOTS; slot++)
  {
    mix(homeSlotStems[slot]);
  }
  return signature;
}

// Loads a recipe's curve into the pump and makes it the active recipe, without
// telling anyone: setup() uses this to restore the recipe from before the
// power went, selectRecipe() to pick one.
bool loadRecipe(const char *stem)
{
  if (!sdReady)
  {
    return false;
  }

  char wanted[RECIPE_NAME_MAX_LEN];
  strncpy(wanted, stem, sizeof(wanted) - 1);
  wanted[sizeof(wanted) - 1] = '\0';

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "/recipes/%s.csv", wanted);
  if (!readSdTextFile(path, sdTextBuffer, sizeof(sdTextBuffer)))
  {
    Serial.print("[RECIPE] Cannot read "); Serial.println(path);
    return false;
  }

  // Parse straight into the live curve, keeping the old one to put back: a file
  // that fails half way must not leave the pump following a mixture of the two.
  ProfilePoint previousPoints[MAX_PROFILE_POINTS];
  int previousCount = profilePointCount;
  memcpy(previousPoints, profilePoints, sizeof(previousPoints));
  profilePointCount = 0;
  resetRecipeScratch(wanted);

  if (!parseBodyLines(sdTextBuffer) || profilePointCount < PROFILE_MIN_POINTS)
  {
    memcpy(profilePoints, previousPoints, sizeof(profilePoints));
    profilePointCount = previousCount;
    Serial.print("[RECIPE] Rejected "); Serial.println(path);
    return false;
  }

  activeRecipe = recipeScratch;

  Serial.print("[RECIPE] Loaded '"); Serial.print(activeRecipe.name);
  Serial.print("' — "); Serial.print(profilePointCount);
  Serial.print(" points over "); Serial.print(profileTotalSeconds(), 1); Serial.println("s");
  loadRememberedShotTime();
  return true;
}

// Sets the machine up for a recipe: its curve for the pump, its yield for the
// weight cut-off. Picking a recipe is a request to brew by it, so brew by
// recipe is switched on as well if it was off.
bool selectRecipe(const char *stem)
{
  if (!loadRecipe(stem))
  {
    return false;
  }

  preferences.putString("recipe", activeRecipe.stem);
  setRecipeMode(true);

  updateBrewHeader();
  writeRecipePanel();
  homeSlotsUiDirty = true;
  recipeStateDirty = true;
  return true;
}

// Lets go of the active recipe. The machine then brews as a stock Mara X until
// another recipe is picked.
void clearActiveRecipe()
{
  if (!isRecipeActive())
  {
    return;
  }

  Serial.print("[RECIPE] '"); Serial.print(activeRecipe.name); Serial.println("' is no longer active");
  memset(&activeRecipe, 0, sizeof(activeRecipe));
  profilePointCount = 0;
  activeShotSeconds = 0.0f;
  preferences.remove("recipe");
  updateBrewHeader();
  writeRecipePanel();
  homeSlotsUiDirty = true;
  recipeStateDirty = true;
}

// Whether the next shot follows a recipe: brew by recipe is on and one is
// picked. Everything that shapes a shot — the pump curve, the weight cut-off,
// the preview — asks this rather than either half on its own.
bool recipeInCharge()
{
  return recipeModeEnabled && isRecipeActive();
}

// The settings page's switch, and its twin in Home Assistant. The selected
// recipe is kept either way, so switching back on returns to it.
void setRecipeMode(bool enabled)
{
  if (enabled == recipeModeEnabled)
  {
    return;
  }

  recipeModeEnabled = enabled;
  preferences.putBool("recipe_mode", enabled);
  myNex.writeNum("recipeMode", enabled ? 1 : 0);
  Serial.print("[RECIPE] Brew by recipe "); Serial.println(enabled ? "on" : "off");

  updateBrewHeader();
  writeRecipePanel();
  homeSlotsUiDirty = true;
  recipeStateDirty = true;
}

// The switch on the settings page was flipped.
void handleRecipeModeTap()
{
  int requested = pendingRecipeModeTap;
  if (requested < 0)
  {
    return;
  }
  pendingRecipeModeTap = -1;

  // A shot that has started keeps the rules it started with.
  if (brewActive)
  {
    myNex.writeNum("recipeMode", recipeModeEnabled ? 1 : 0);
    return;
  }
  setRecipeMode(requested == 1);
}

// The brew-by-weight target for the next shot: the recipe's yield, or 0 — no
// cut-off — when no recipe is in charge or it gives none.
float activeTargetWeight()
{
  return recipeInCharge() ? activeRecipe.yield : 0.0f;
}

// The tile assignment from the card. A missing or unreadable file just means
// no tiles.
void loadHomeSlots()
{
  memset(homeSlotStems, 0, sizeof(homeSlotStems));
  if (!sdReady)
  {
    return;
  }

  char text[RECIPE_HOME_SLOTS * RECIPE_NAME_MAX_LEN + 8];
  if (!readSdTextFile(RECIPE_HOME_SLOTS_PATH, text, sizeof(text)))
  {
    return;
  }
  if (!parseHomeSlots(text))
  {
    Serial.println("[SD] " RECIPE_HOME_SLOTS_PATH " is malformed — no home tiles");
    return;
  }
  memcpy(homeSlotStems, homeSlotScratch, sizeof(homeSlotStems));
}

static bool saveHomeSlots()
{
  char text[RECIPE_HOME_SLOTS * RECIPE_NAME_MAX_LEN + 8];
  snprintf(text, sizeof(text), "%s,%s,%s\n",
           homeSlotStems[0], homeSlotStems[1], homeSlotStems[2]);
  return writeSdTextFile(RECIPE_HOME_SLOTS_PATH, text);
}

// The recipe on a home tile (0-based), or -1 when the tile is unused.
static int homeSlotRecipeIndex(int slot)
{
  if (slot < 0 || slot >= RECIPE_HOME_SLOTS || homeSlotStems[slot][0] == '\0')
  {
    return -1;
  }
  return findRecipeIndex(homeSlotStems[slot]);
}

// Writes the home tiles r1..r3. The text and colours are sent whatever page is
// open — the HMI keeps them if the tiles are vscope global — but `vis` only
// works on the page that is loaded, so updateDisplay() calls this again each
// time the home page opens. The home page's own Postinitialize script hides
// an empty tile before this gets there; see doc/ui/recipes_display.md.
void updateHomeSlotsUi()
{
  bool onHomePage = isHomePage(currentPageId);

  for (int slot = 0; slot < RECIPE_HOME_SLOTS; slot++)
  {
    int index = homeSlotRecipeIndex(slot);
    String tile = "r" + String(slot + 1);
    String component = "home." + tile;

    if (index >= 0)
    {
      char text[128];
      buildHomeTileText(recipes[index], text, sizeof(text));
      bool active = recipeInCharge() && strcmp(recipes[index].stem, activeRecipe.stem) == 0;

      sendNextionCommand(component + ".txt=\"" + nextionEscapedText(text) + "\"");
      sendNextionCommand(component + ".pco=" + String(active ? RECIPE_TILE_PCO_ACTIVE : RECIPE_TILE_PCO));
      sendNextionCommand(component + ".bco=" + String(active ? RECIPE_TILE_BCO_ACTIVE : RECIPE_TILE_BCO));
    }
    else
    {
      sendNextionCommand(component + ".txt=\"\"");
    }

    if (onHomePage)
    {
      sendNextionCommand("vis " + tile + "," + String(index >= 0 ? 1 : 0));
    }
  }
}

// Fills in the recipe panel on the brew page: what to grind, how much to dose,
// where the temperature switch should be, and the yield the shot will stop at.
// With no recipe in charge every field shows a dash.
//
// The brew page's components are local to it, so this only lands while the
// page is loaded. It is called on entering the page and whenever the recipe
// or the mode changes; a missing component fails silently (bkcmd=0), so an HMI
// without the panel just ignores it.
void writeRecipePanel()
{
  char grams[12];
  char dose[16] = "-";
  char yieldText[16] = "-";
  char ratio[16] = "-";
  const char *grind = "-";
  const char *temp = "-";

  if (recipeInCharge())
  {
    if (activeRecipe.grind[0] != '\0')
    {
      grind = activeRecipe.grind;
    }
    temp = recipeTempLabel(activeRecipe.temp);
    if (activeRecipe.dose > 0.0f)
    {
      formatGrams(activeRecipe.dose, grams, sizeof(grams));
      snprintf(dose, sizeof(dose), "%s g", grams);
    }
    if (activeRecipe.yield > 0.0f)
    {
      formatGrams(activeRecipe.yield, grams, sizeof(grams));
      snprintf(yieldText, sizeof(yieldText), "%s g", grams);
    }
    if (activeRecipe.dose > 0.0f && activeRecipe.yield > 0.0f)
    {
      snprintf(ratio, sizeof(ratio), "1:%.1f", activeRecipe.yield / activeRecipe.dose);
    }
  }

  sendNextionCommand("rGrind.txt=\"" + nextionEscapedText(grind) + "\"");
  sendNextionCommand("rDose.txt=\"" + String(dose) + "\"");
  sendNextionCommand("rYield.txt=\"" + String(yieldText) + "\"");
  sendNextionCommand("rRatio.txt=\"" + String(ratio) + "\"");
  sendNextionCommand("rTemp.txt=\"" + String(temp) + "\"");
}

// The brew page's header: the recipe the shot will follow, or why none will.
// Without the scale it adds the remembered time that will end the shot:
// "Ethiopia Guji (27 s)". `profileModetxt` is the global the brew page copies
// into its header when it loads; `profileMode` is the header itself, for when
// the page is already open.
void updateBrewHeader()
{
  char timed[RECIPE_NAME_MAX_LEN + 16];
  const char *header;
  if (!recipeModeEnabled)
  {
    header = "Brew by recipe off";
  }
  else if (isRecipeActive() && !scaleConnected && activeShotSeconds > 0.0f)
  {
    snprintf(timed, sizeof(timed), "%s (%d s)", activeRecipe.name, (int)(activeShotSeconds + 0.5f));
    header = timed;
  }
  else if (isRecipeActive())
  {
    header = activeRecipe.name;
  }
  else
  {
    header = "No recipe selected";
  }

  String text = nextionEscapedText(header);
  sendNextionCommand("profileModetxt.txt=\"" + text + "\"");
  sendNextionCommand("profileMode.txt=\"" + text + "\"");
}

// A tap on a home tile: set the machine up for that recipe and go straight to
// the brew page, where the panel says what to do next.
void handleHomeTileTap()
{
  int slot = pendingHomeTileTap;
  if (slot == 0)
  {
    return;
  }
  pendingHomeTileTap = 0;

  if (brewActive)
  {
    return;
  }

  int index = homeSlotRecipeIndex(slot - 1);
  if (index < 0)
  {
    Serial.print("[RECIPE] Home tile "); Serial.print(slot); Serial.println(" has no recipe");
    return;
  }

  if (!selectRecipe(recipes[index].stem))
  {
    return;
  }

  // The display runs commands in order, so the writes after `page brew` land
  // on the freshly loaded brew page.
  sendNextionCommand("page brew");
  updateBrewHeader();
  writeRecipePanel();
}

// Everything in the active recipe that changes how long it takes to reach its
// yield — all of it but the name — and the live curve, which is the recipe's
// own once it is loaded.
uint32_t recipeShotFingerprint()
{
  uint32_t fingerprint = 2166136261u;
  for (const char *c = activeRecipe.grind; *c != '\0'; c++)
  {
    fingerprint = (fingerprint ^ (uint8_t)*c) * 16777619u;
  }
  fingerprint = fingerprint * 31u + (uint32_t)(activeRecipe.dose * 10.0f);
  fingerprint = fingerprint * 31u + (uint32_t)(activeRecipe.yield * 10.0f);
  fingerprint = fingerprint * 31u + (uint32_t)(activeRecipe.temp + 1);
  return fingerprint * 31u + profileSegmentSignature();
}

// NVS keys are at most 15 characters and a file name may be 31, so a time is
// filed under a hash of the name.
static void shotTimeKey(const char *stem, char *out, size_t outLen)
{
  uint32_t hash = 2166136261u;
  for (const char *c = stem; *c != '\0'; c++)
  {
    hash = (hash ^ (uint8_t)*c) * 16777619u;
  }
  snprintf(out, outLen, "shot%08lx", (unsigned long)hash);
}

// Looks up the active recipe's remembered time. Called whenever a recipe is
// loaded, so an edit is noticed the moment it takes effect.
void loadRememberedShotTime()
{
  activeShotSeconds = 0.0f;
  activeRecipeFingerprint = recipeShotFingerprint();

  char key[16];
  shotTimeKey(activeRecipe.stem, key, sizeof(key));
  RememberedShotTime saved;
  if (!preferences.isKey(key) ||
      preferences.getBytes(key, &saved, sizeof(saved)) != sizeof(saved))
  {
    return;
  }

  if (saved.fingerprint != activeRecipeFingerprint)
  {
    Serial.print("[SHOT] '"); Serial.print(activeRecipe.name);
    Serial.println("' changed since the scale timed it — the next shot with the scale times it again");
    return;
  }

  activeShotSeconds = saved.seconds;
  Serial.print("[SHOT] Without the scale '"); Serial.print(activeRecipe.name);
  Serial.print("' stops after "); Serial.print(activeShotSeconds, 1); Serial.println("s");
}

void forgetShotTime(const char *stem)
{
  char key[16];
  shotTimeKey(stem, key, sizeof(key));
  if (preferences.isKey(key))
  {
    preferences.remove(key);
  }
}

// The lever went up. Whether the scale was there and tared is only known now:
// by the cut-off the cup has long been filling.
void startShotEvidence()
{
  shotTimeToRemember = 0.0f;
  shotTimeDoubt = NULL;
  shotLastSampleMs = lastWeightTime;
  shotLastSampleWeight = currentWeight;

  if (!scaleConnected || lastWeightTime == 0)
  {
    shotTimeDoubt = "no scale when the lever went up";
  }
  else if (fabsf(currentWeight) > SHOT_TIME_MAX_START_WEIGHT_G)
  {
    shotTimeDoubt = "the scale was not tared when the lever went up";
  }
}

// One reading from the scale between the lever going up and the cut-off.
void noteShotWeight(unsigned long nowMs, float weight)
{
  unsigned long gapMs = nowMs - shotLastSampleMs;
  float step = weight - shotLastSampleWeight;
  shotLastSampleMs = nowMs;
  shotLastSampleWeight = weight;

  if (shotTimeDoubt != NULL)
  {
    return;
  }

  if (gapMs > SHOT_TIME_MAX_SAMPLE_GAP_MS)
  {
    shotTimeDoubt = "the scale went quiet during the shot";
  }
  else if (fabsf(step) > SHOT_TIME_MAX_FLOW_GPS * gapMs / 1000.0f + SHOT_TIME_WEIGHT_JITTER_G)
  {
    shotTimeDoubt = "the weight jumped faster than coffee flows";
  }
}

// Why a shot the weight stopped `seconds` after the lever went up should not
// time the recipe, or NULL when it can.
const char *shotTimeRejection(float seconds, float yield)
{
  if (shotTimeDoubt != NULL)
  {
    return shotTimeDoubt;
  }
  if (seconds < SHOT_TIME_MIN_SECONDS || seconds < yield / SHOT_TIME_MAX_FLOW_GPS)
  {
    return "too short to be a real shot";
  }
  if (seconds > SHOT_TIME_MAX_SECONDS)
  {
    return "too long to be a real shot";
  }
  return NULL;
}

// The weight cut-off just stopped the shot. Its time is kept for when the
// lever comes down, unless something about the shot says not to trust it.
void considerShotTime(float seconds)
{
  const char *rejection = shotTimeRejection(seconds, activeRecipe.yield);
  if (rejection != NULL)
  {
    Serial.print("[SHOT] Not remembering "); Serial.print(seconds, 1);
    Serial.print("s for '"); Serial.print(activeRecipe.name);
    Serial.print("': "); Serial.println(rejection);
    return;
  }
  shotTimeToRemember = seconds;
}

void saveRememberedShotTime()
{
  if (shotTimeToRemember <= 0.0f || !isRecipeActive())
  {
    return;
  }

  activeShotSeconds = shotTimeToRemember;
  shotTimeToRemember = 0.0f;
  updateBrewHeader();

  RememberedShotTime saved = { activeRecipeFingerprint, activeShotSeconds };
  char key[16];
  shotTimeKey(activeRecipe.stem, key, sizeof(key));
  if (preferences.putBytes(key, &saved, sizeof(saved)) != sizeof(saved))
  {
    // Still used until the next reboot.
    Serial.println("[SHOT] Could not write the shot time to NVS");
    return;
  }

  Serial.print("[SHOT] '"); Serial.print(activeRecipe.name);
  Serial.print("' took "); Serial.print(activeShotSeconds, 1);
  Serial.println("s to its yield — remembered for shots without the scale");
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

void finalizePendingObservation(float finalWeight)
{
  pendingObservation = false;

  float extraWeight = finalWeight - pendingWeightAtStop;

  // Log the shot so the hard-coded pressure/offset table can be checked
  // against what actually landed in the cup.
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
  // A recipe name is free text and may hold a comma, which would shift every
  // column after it. A shot pulled without a recipe is logged as "-".
  const char *recipeName = recipeInCharge() ? activeRecipe.name : "-";
  for (const char *c = recipeName; *c != '\0'; c++)
  {
    f.print(*c == ',' ? ';' : *c);
  }
  f.print(",");
  f.print(finalWeight, 2);
  f.print(",");
  f.print(activeTargetWeight(), 2);
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

void updateScaleConnectionUi()
{
  int connected = scaleConnected ? 1 : 0;
  bool changed = (connected != lastScaleConnectedUi);
  unsigned long now = millis();

  // Re-send periodically as well as on change: the write below is lost whenever
  // the home page is not the loaded page, so a state change that happens on the
  // brew or settings page would otherwise leave the icon stale forever.
  if (!changed && (now - lastScaleConnectedUiWriteMs) < SCALE_UI_REFRESH_INTERVAL_MS)
  {
    return;
  }

  if (changed)
  {
    Serial.print("[DEBUG] Scale connection changed: ");
    Serial.print(lastScaleConnectedUi);
    Serial.print(" -> ");
    Serial.println(connected);
    // The header shows the remembered shot time only while there is no scale.
    updateBrewHeader();
  }

  // Update global variable (used by home page timer to refresh scalecon.val)
  myNex.writeNum("scaleConnected", connected);
  // Directly update p3.pic on home page (pic 20=connected, 21=disconnected).
  // The cross-page write fails when the home page is not loaded; with bkcmd=0
  // (set in setup()) that failure is silent instead of returning a junk packet.
  myNex.writeNum("home.p3.pic", connected ? 19 : 20);

  lastScaleConnectedUi = connected;
  lastScaleConnectedUiWriteMs = now;
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
    // Time the gap from the end of an attempt, not its start, so back-to-back
    // attempts don't keep the shared 2.4GHz radio busy the whole time.
    lastScaleReconnectAttemptMs = millis();
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

#if !defined(DISABLE_WIFI_MQTT) && defined(ENABLE_HA_DISCOVERY)

// Every entity carries the same identifiers so Home Assistant files them all
// under one device instead of eight loose entities.
#define HA_DEVICE_BLOCK \
  "\"dev\":{\"ids\":[\"" ha_device_id "\"],\"name\":\"Mara X\"," \
  "\"mf\":\"Lelit\",\"mdl\":\"Mara X Pressure Mod\"}"

// Available whenever the controller itself is online.
#define HA_AVAILABILITY_BASE \
  "\"avty\":[{\"t\":\"" availability_topic "\"}]"

// Available only while the controller is online AND the machine is powered up.
// Without this the temperature entities would sit at a retained 0 °C all night
// and drag every history graph down to zero.
#define HA_AVAILABILITY_POWERED \
  "\"avty\":[{\"t\":\"" availability_topic "\"}," \
  "{\"t\":\"" power_topic "\",\"pl_avail\":\"1\",\"pl_not_avail\":\"0\"}]," \
  "\"avty_mode\":\"all\""

// Builds and publishes one retained discovery payload. `attributes` carries the
// entity-specific JSON (name, device class, unit, …) without a trailing comma.
static bool publishHaEntity(const char *component,
                            const char *objectId,
                            const char *stateTopic,
                            const char *attributes,
                            bool requiresMachinePower)
{
  String topic = String(ha_discovery_prefix "/") + component + "/" ha_device_id "/" + objectId + "/config";

  String payload = "{\"uniq_id\":\"" ha_device_id "_";
  payload += objectId;
  payload += "\",\"stat_t\":\"";
  payload += stateTopic;
  payload += "\",";
  payload += attributes;
  payload += ",";
  payload += requiresMachinePower ? HA_AVAILABILITY_POWERED : HA_AVAILABILITY_BASE;
  payload += "," HA_DEVICE_BLOCK "}";

  bool ok = mqttClient.publish(topic.c_str(), payload.c_str(), true);
  if (!ok)
  {
    Serial.print("[MQTT] Discovery publish failed for ");
    Serial.print(objectId);
    Serial.print(" (payload ");
    Serial.print(payload.length());
    Serial.println(" bytes)");
  }
  return ok;
}

// The recipe picker. Its options are the files on the card, so
// publishRecipeState() re-announces it whenever they change. The first option
// lets go of the recipe.
void publishHaRecipeSelect()
{
  String attributes = "\"name\":\"Recipe\",\"ic\":\"mdi:notebook-outline\","
                      "\"cmd_t\":\"" recipe_select_topic "\",\"ops\":[\"" RECIPE_NONE_OPTION "\"";
  for (int i = 0; i < recipeCount; i++)
  {
    attributes += ",\"";
    attributes += recipes[i].stem;
    attributes += "\"";
  }
  attributes += "]";

  publishHaEntity("select", "recipe", recipe_active_topic, attributes.c_str(), false);
}

// Re-announced on every reconnect: the payloads are retained, but republishing
// makes the setup self-healing if the broker's retained store is ever cleared.
void publishHaDiscovery()
{
  publishHaEntity("sensor", "brewtemp", brewtemp_topic,
                  "\"name\":\"Brew temperature\",\"dev_cla\":\"temperature\","
                  "\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"sug_dsp_prc\":0", true);

  publishHaEntity("sensor", "steamtemp", steamtemp_topic,
                  "\"name\":\"Steam temperature\",\"dev_cla\":\"temperature\","
                  "\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"sug_dsp_prc\":0", true);

  publishHaEntity("sensor", "steamtargettemp", steamtargettemp_topic,
                  "\"name\":\"Steam target temperature\",\"dev_cla\":\"temperature\","
                  "\"unit_of_meas\":\"°C\",\"stat_cla\":\"measurement\",\"sug_dsp_prc\":0", true);

  publishHaEntity("sensor", "fastheat_timer", fastheat_topic,
                  "\"name\":\"Fast heat countdown\",\"dev_cla\":\"duration\","
                  "\"unit_of_meas\":\"s\",\"stat_cla\":\"measurement\"", true);

  // Not power-gated: the shot count is cumulative and stays meaningful while
  // the machine is off.
  publishHaEntity("sensor", "shots", shots_topic,
                  "\"name\":\"Shot count\",\"stat_cla\":\"total_increasing\","
                  "\"ic\":\"mdi:coffee\"", false);

  publishHaEntity("sensor", "debug", debug_topic,
                  "\"name\":\"Serial frame\",\"ent_cat\":\"diagnostic\","
                  "\"ic\":\"mdi:code-string\"", true);

  publishHaEntity("binary_sensor", "heatingelement", heatingElement_topic,
                  "\"name\":\"Heating element\",\"dev_cla\":\"running\","
                  "\"pl_on\":\"1\",\"pl_off\":\"0\"", true);

  // Not power-gated — this entity *is* the power state.
  publishHaEntity("binary_sensor", "power_state", power_topic,
                  "\"name\":\"Power\",\"dev_cla\":\"power\","
                  "\"pl_on\":\"1\",\"pl_off\":\"0\"", false);

  // Every recipe command is answered here, so a rejected edit is visible in
  // Home Assistant instead of only on the serial console.
  publishHaEntity("sensor", "recipe_result", recipe_result_topic,
                  "\"name\":\"Recipe command result\",\"ent_cat\":\"diagnostic\","
                  "\"ic\":\"mdi:message-alert-outline\"", false);

  // The settings page's switch. Off, the machine brews as a stock Mara X.
  publishHaEntity("switch", "recipe_mode", recipe_mode_topic,
                  "\"name\":\"Brew by recipe\",\"ic\":\"mdi:notebook-check-outline\","
                  "\"cmd_t\":\"" recipe_mode_set_topic "\"", false);

  publishHaRecipeSelect();

  // Profiles are gone. Their retained configs would otherwise keep two dead
  // entities in Home Assistant for good; an empty retained message removes
  // an entity announced by discovery.
  mqttClient.publish(ha_discovery_prefix "/select/" ha_device_id "/profile/config", "", true);
  mqttClient.publish(ha_discovery_prefix "/sensor/" ha_device_id "/profile_result/config", "", true);

  Serial.println("[MQTT] Home Assistant discovery published");
}
#endif

#ifndef DISABLE_WIFI_MQTT

static void publishCommandResult(const char *topic, const char *text)
{
  Serial.print("[MQTT] "); Serial.print(topic); Serial.print(": "); Serial.println(text);
  // Not retained: this is the answer to one command, not a state a fresh
  // subscriber should be handed on connect.
  mqttClient.publish(topic, text, false);
}

static void publishRecipeResult(const char *text)
{
  publishCommandResult(recipe_result_topic, text);
}

// Publishes one SD file, as it is on the card, on `<topicPrefix><stem>`,
// retained. An empty retained message would delete the topic instead, so a
// file that cannot be read is skipped and logged rather than published blank.
static void publishSdFileBody(const char *directory, const char *stem, const char *topicPrefix)
{
  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/%s.csv", directory, stem);
  if (!readSdTextFile(path, sdTextBuffer, sizeof(sdTextBuffer)) || sdTextBuffer[0] == '\0')
  {
    Serial.print("[MQTT] Not publishing "); Serial.print(path); Serial.println(" — unreadable or empty");
    return;
  }

  String topic = String(topicPrefix) + stem;
  if (!mqttClient.publish(topic.c_str(), sdTextBuffer, true))
  {
    Serial.print("[MQTT] "); Serial.print(topic); Serial.println(" did not fit the buffer");
  }
}

// Clears a retained body topic after its file was deleted.
static void clearPublishedBody(const char *topicPrefix, const char *stem)
{
  String topic = String(topicPrefix) + stem;
  mqttClient.publish(topic.c_str(), "", true);
}

void publishRecipeLibrary()
{
  for (int i = 0; i < recipeCount; i++)
  {
    publishSdFileBody("/recipes", recipes[i].stem, recipe_body_prefix);
  }
}

// Refreshes the retained recipe topics: the list, the active recipe, the home
// tiles and the mode. The list is the authority on what exists — a body left retained
// on the broker after its file was removed from the card by hand is ignored by
// anything that checks it against this.
void publishRecipeState()
{
  if (!mqttClient.connected())
  {
    return;
  }

  String list = "";
  for (int i = 0; i < recipeCount; i++)
  {
    if (list.length() > 0)
    {
      list += ",";
    }
    list += recipes[i].stem;
  }
  mqttClient.publish(recipe_list_topic, list.c_str(), true);
  mqttClient.publish(recipe_active_topic, isRecipeActive() ? activeRecipe.stem : RECIPE_NONE_OPTION, true);

  String home = "";
  for (int slot = 0; slot < RECIPE_HOME_SLOTS; slot++)
  {
    if (slot > 0)
    {
      home += ",";
    }
    home += homeSlotStems[slot];
  }
  mqttClient.publish(recipe_home_topic, home.c_str(), true);
  mqttClient.publish(recipe_mode_topic, recipeModeEnabled ? "ON" : "OFF", true);

#ifdef ENABLE_HA_DISCOVERY
  publishHaRecipeSelect();
#endif
}

// The body of processPendingRecipeCommand(), which has already made sure there
// is no shot running and a card to write to.
static void processRecipeCommand()
{
  RecipeCommandKind kind = pendingRecipeCommand.kind;
  char stem[RECIPE_NAME_MAX_LEN];
  char message[96];

  if (kind == RECIPE_CMD_SELECT)
  {
    // For select the stem arrives as the payload, not in the topic.
    const char *payload = pendingRecipeCommand.body;
    if (payload[0] == '\0' || strcmp(payload, RECIPE_NONE_OPTION) == 0)
    {
      clearActiveRecipe();
      publishRecipeResult("ok: no recipe active");
      recipeStateDirty = true;
    }
    else if (!sanitizeRecipeStem(payload, stem, sizeof(stem)))
    {
      publishRecipeResult("error: invalid recipe name");
    }
    else if (findRecipeIndex(stem) < 0)
    {
      snprintf(message, sizeof(message), "error: no recipe '%s'", stem);
      publishRecipeResult(message);
    }
    else if (!selectRecipe(stem))
    {
      snprintf(message, sizeof(message), "error: could not load '%s'", stem);
      publishRecipeResult(message);
    }
    else
    {
      snprintf(message, sizeof(message), "ok: selected '%s'", stem);
      publishRecipeResult(message);
    }
  }
  else if (kind == RECIPE_CMD_SAVE)
  {
    char path[MAX_PATH_LEN];
    if (!sanitizeRecipeStem(pendingRecipeCommand.stem, stem, sizeof(stem)))
    {
      publishRecipeResult("error: invalid recipe name");
      return;
    }

    resetRecipeScratch(stem);
    snprintf(path, sizeof(path), "/recipes/%s.csv", stem);
    if (!validateRecipeBody(pendingRecipeCommand.body))
    {
      publishRecipeResult("error: recipe body rejected by the parser");
    }
    else if (findRecipeIndex(stem) < 0 && recipeCount >= MAX_RECIPES)
    {
      publishRecipeResult("error: the card already holds the most recipes the controller can list");
    }
    else if (!writeSdTextFile(path, pendingRecipeCommand.body))
    {
      publishRecipeResult("error: could not write to the SD card");
    }
    else
    {
      scanRecipes();

      // Editing the selected recipe takes effect straight away. loadRecipe()
      // rather than selectRecipe(): saving is not a request to switch brew by
      // recipe back on.
      if (isRecipeActive() && strcmp(stem, activeRecipe.stem) == 0 && loadRecipe(stem))
      {
        updateBrewHeader();
        writeRecipePanel();
      }

      publishSdFileBody("/recipes", stem, recipe_body_prefix);
      homeSlotsUiDirty = true;
      recipeStateDirty = true;
      snprintf(message, sizeof(message), "ok: saved '%s'", stem);
      publishRecipeResult(message);
    }
  }
  else if (kind == RECIPE_CMD_DELETE)
  {
    char path[MAX_PATH_LEN];
    if (!sanitizeRecipeStem(pendingRecipeCommand.stem, stem, sizeof(stem)))
    {
      publishRecipeResult("error: invalid recipe name");
      return;
    }

    snprintf(path, sizeof(path), "/recipes/%s.csv", stem);
    if (findRecipeIndex(stem) < 0)
    {
      snprintf(message, sizeof(message), "error: no recipe '%s'", stem);
      publishRecipeResult(message);
    }
    else if (!SD.remove(path))
    {
      publishRecipeResult("error: could not delete the file");
    }
    else
    {
      // The selected recipe may go too: the machine then has none, as if it
      // had been cleared, rather than pointing at a file that is gone.
      if (isRecipeActive() && strcmp(stem, activeRecipe.stem) == 0)
      {
        clearActiveRecipe();
      }
      forgetShotTime(stem);

      bool slotsChanged = false;
      for (int slot = 0; slot < RECIPE_HOME_SLOTS; slot++)
      {
        if (strcmp(homeSlotStems[slot], stem) == 0)
        {
          homeSlotStems[slot][0] = '\0';
          slotsChanged = true;
        }
      }
      if (slotsChanged)
      {
        saveHomeSlots();
      }

      scanRecipes();
      clearPublishedBody(recipe_body_prefix, stem);
      homeSlotsUiDirty = true;
      recipeStateDirty = true;
      snprintf(message, sizeof(message), "ok: deleted '%s'", stem);
      publishRecipeResult(message);
    }
  }
  else if (kind == RECIPE_CMD_HOME)
  {
    if (!parseHomeSlots(pendingRecipeCommand.body))
    {
      publishRecipeResult("error: home tiles must be up to three recipe names, comma separated");
      return;
    }

    for (int slot = 0; slot < RECIPE_HOME_SLOTS; slot++)
    {
      if (homeSlotScratch[slot][0] != '\0' && findRecipeIndex(homeSlotScratch[slot]) < 0)
      {
        snprintf(message, sizeof(message), "error: no recipe '%s'", homeSlotScratch[slot]);
        publishRecipeResult(message);
        return;
      }
    }

    memcpy(homeSlotStems, homeSlotScratch, sizeof(homeSlotStems));
    if (!saveHomeSlots())
    {
      publishRecipeResult("error: could not write to the SD card");
    }
    else
    {
      publishRecipeResult("ok: home tiles updated");
    }
    // Even after a failed write the tiles show the new assignment until the
    // next reboot, so say so rather than pretend nothing changed.
    homeSlotsUiDirty = true;
    recipeStateDirty = true;
  }
}

// Carries out whatever the MQTT callback recorded. Runs from the main loop so
// the SD card is only touched at a moment of our choosing.
void processPendingRecipeCommand()
{
  RecipeCommandKind kind = pendingRecipeCommand.kind;
  if (kind == RECIPE_CMD_NONE)
  {
    return;
  }

  if (!mqttClient.connected())
  {
    // Nowhere to report the outcome; drop it rather than acting unannounced.
    pendingRecipeCommand.kind = RECIPE_CMD_NONE;
    return;
  }

  if (kind == RECIPE_CMD_TOO_LARGE)
  {
    publishRecipeResult("error: body too large");
    pendingRecipeCommand.kind = RECIPE_CMD_NONE;
    return;
  }

  // Never write to the card or change what the pump follows mid-shot.
  // Refusing outright is clearer than queueing the command to land seconds
  // later.
  if (brewActive)
  {
    publishRecipeResult("error: brew in progress");
    pendingRecipeCommand.kind = RECIPE_CMD_NONE;
    return;
  }

  // The mode lives in NVS, so it works without a card.
  if (kind == RECIPE_CMD_MODE)
  {
    const char *payload = pendingRecipeCommand.body;
    if (strcmp(payload, "ON") == 0 || strcmp(payload, "OFF") == 0)
    {
      setRecipeMode(strcmp(payload, "ON") == 0);
      publishRecipeResult(recipeModeEnabled ? "ok: brew by recipe on" : "ok: brew by recipe off");
    }
    else
    {
      publishRecipeResult("error: brew by recipe takes ON or OFF");
    }
    recipeStateDirty = true;
    pendingRecipeCommand.kind = RECIPE_CMD_NONE;
    return;
  }

  if (!sdReady)
  {
    publishRecipeResult("error: no SD card");
    pendingRecipeCommand.kind = RECIPE_CMD_NONE;
    return;
  }

  processRecipeCommand();
  pendingRecipeCommand.kind = RECIPE_CMD_NONE;
}
#endif

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

  // Retained last will: the broker publishes "offline" here for us if the
  // connection dies without a clean disconnect.
  if (mqttClient.connect("MaraXMod", mqtt_user, mqtt_password,
                         availability_topic, 0, true, "offline"))
  {
    mqttClient.publish(availability_topic, "online", true);

    mqttClient.subscribe(recipe_select_topic);
    mqttClient.subscribe(recipe_home_set_topic);
    mqttClient.subscribe(recipe_mode_set_topic);
    mqttClient.subscribe(recipe_save_prefix "+");
    mqttClient.subscribe(recipe_delete_prefix "+");
    // The retained topics are refreshed from the main loop.
    recipeStateDirty = true;
    recipeLibraryDirty = true;

#ifdef ENABLE_HA_DISCOVERY
    publishHaDiscovery();
#endif
  }
#endif
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

  // A scale that was switched off can keep looking connected until the link
  // supervision times out, so treat a stream that went quiet as a dead link too.
  if (!scale.isConnected() || scale.linkIsStale())
  {
    Serial.println(scale.isConnected() ? "[DEBUG] Scale stopped sending data — dropping the link"
                                       : "[DEBUG] Scale disconnected");
    scale.disconnect();
    scaleConnected = false;
    flowRate = 0.0f;
    lastScaleReconnectAttemptMs = 0;  // reconnect on the next loop, don't wait out the interval
    updateScaleConnectionUi();
    return;
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

    // Up to the cut-off: what comes after it, such as taking the cup away,
    // says nothing about how long the shot took.
    if (brewActive && !shotCutOff)
    {
      noteShotWeight(now, currentWeight);
    }
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
    // current reading as finalWeight.
    finalizePendingObservation(currentWeight);
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

  // Stable within the configured window: accept this as the final weight.
  finalizePendingObservation(currentWeight);
}

// Gets "live" Info during brew
void liveData()
{
  if (brewActive)
  {
    unsigned long nowMs = millis();

    // Throttle the display writes. Unthrottled they ran on every loop
    // iteration: at 115200 baud the UART2 TX buffer fills, Serial2.print()
    // blocks, and the display ends up pacing the loop that runs pressure
    // control and the weight cut-off.
    static unsigned long lastLiveDataMs = 0;
    if (nowMs - lastLiveDataMs >= LIVE_DATA_REFRESH_MS)
    {
      lastLiveDataMs = nowMs;

      // Write Brew Temp
      myNex.writeNum("brewTemp", brewTemp);
      // Write BrewTimer
      myNex.writeNum("brewTime", (int)((nowMs - activeBrewingStart) / 1000));
      // Write Live Pressure as normal number
      float pressure = getPressure();
      // barvar is an XFloat with 2 decimal places: it displays value/100.
      myNex.writeNum("barvar.val", (int)(pressure * 100));
      // Map the pressure to wave pixels in graph (0–10 bar → 0–180 px).
      // getPressure() can return up to 12 bar, so clamp before mapping to keep
      // the trace inside the waveform.
      int pressureCentibar = constrain((int)(pressure * 100), 0, 1000);
      barGraphValue = map(pressureCentibar, 0, 1000, 0, 180);
      myNex.writeNum("barwave.val", barGraphValue);

      if (scaleConnected)
      {
        myNex.writeNum("weightVar.val", (int)(currentWeight * 10));
        myNex.writeNum("flowVar.val", (int)(flowRate * 100));
      }
    }

    // Periodic dimmer diagnostics (every 1 second during brew)
    // lastDimmerDebugMs and lastZeroCrossCountDebug are globals seeded in
    // brewDetect() at brew start, so the very first reading is always accurate.
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

// Reads a numeric component from the display. Returns false and leaves `value`
// untouched when the display did not answer in time.
//
// This deliberately does not use EasyNextionLibrary's readNumber(): that one
// begins by discarding every byte already in the RX buffer that is not part of
// its own '#' custom protocol — touch events included — and then scans past
// anything that is not 0x71, so a tile tap made while a read was in flight
// would be silently thrown away. Going through nextionProcessRx() keeps it
// while we wait for the reply.
bool readNextionNumber(const char *component, uint32_t &value)
{
  // A previous `get` gave up waiting. Its answer may still be on its way, and
  // it would look exactly like the answer to this one, so give it a moment to
  // turn up and throw it away before asking for anything else. Without this
  // every read after a single timeout is shifted by one component — the page
  // id would report the page before last.
  if (nextionReadDesynced)
  {
    uint32_t stale = 0;
    unsigned long drainStartMs = millis();
    while (millis() - drainStartMs < NEXTION_RESYNC_DRAIN_MS)
    {
      if (nextionProcessRx(&stale))
      {
        break;  // that was the late answer — discarded
      }
      delay(1);
    }
    nextionReadDesynced = false;
  }

  // Clear anything already pending (touches are kept) so a late reply to an
  // earlier `get` cannot be mistaken for the answer to this one.
  nextionProcessRx(NULL);

  sendNextionCommand("get " + String(component));

  uint32_t received = 0;
  unsigned long startMs = millis();
  while (millis() - startMs < NEXTION_READ_TIMEOUT_MS)
  {
    if (nextionProcessRx(&received))
    {
      value = received;
      return true;
    }
    delay(1);  // yield to FreeRTOS instead of spinning on the UART
  }

  nextionReadDesynced = true;
  return false;
}

// Picks up recipes changed straight on the card. Compared by content rather
// than count: an edit to a file changes what a home tile should say. Held
// back during and just after a shot, when the SD card and the loop have
// better things to do.
void rescanRecipes()
{
  if (!sdReady || brewActive || pendingObservation ||
      millis() - lastRecipeScanMs < RECIPE_SCAN_INTERVAL_MS)
  {
    return;
  }
  lastRecipeScanMs = millis();

  uint32_t recipesBefore = recipeLibrarySignature();
  scanRecipes();
  loadHomeSlots();
  if (recipeLibrarySignature() != recipesBefore)
  {
    Serial.println("[DEBUG] Recipes on the card changed");
    homeSlotsUiDirty = true;
    recipeStateDirty = true;
    recipeLibraryDirty = true;
  }
}

void updateDisplay()
{
  // Wrap-safe interval check: pageRefreshTimer holds the last refresh time.
  if ((millis() - pageRefreshTimer >= REFRESH_SCREEN_EVERY) && !brewActive)
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
    // The settings page copies this into its switch when it opens. Re-sent on
    // every refresh because the display restarts with the ESP32 and would
    // otherwise show its HMI default until the switch is next flipped.
    myNex.writeNum("recipeMode", recipeModeEnabled ? 1 : 0);

    updateScaleConnectionUi();

    // Keep the previous page id when the display does not answer, rather than
    // letting the failure sentinel masquerade as a page change.
    readNextionNumber("dp", currentPageId);

    if (currentPageId != lastPageId)
    {
      if (isBrewPage(currentPageId))
      {
        brewPreviewNeedsDraw = true;
        updateBrewHeader();
        writeRecipePanel();
      }
      if (isHomePage(currentPageId))
      {
        homeSlotsUiDirty = true;
      }
      lastPageId = currentPageId;
    }

    // Sent on any page, not just the home page: with the tiles vscope global
    // the text lands wherever the display is, so a wrong HOME_PAGE_ID costs
    // only the show/hide, not the tiles' content.
    if (homeSlotsUiDirty)
    {
      updateHomeSlotsUi();
      homeSlotsUiDirty = false;
    }

    // The preview is only ever drawn while idle: once a shot is running the
    // graph belongs to the live trace, and after it finishes the result stays
    // on screen until the page is left and re-entered.
    if (isBrewPage(currentPageId))
    {
      // Switching brew by recipe off or on changes what the preview should
      // show without changing the curve itself.
      uint32_t signature = profileSegmentSignature() * 2u + (recipeInCharge() ? 1u : 0u);
      if (signature != lastPreviewSignature)
      {
        lastPreviewSignature = signature;
        brewPreviewNeedsDraw = true;
      }

      if (brewPreviewNeedsDraw)
      {
        drawTargetProfilePreview();
        brewPreviewNeedsDraw = false;
      }
    }

    myNex.writeNum("tarsteam.val", steamTargetTemp);
    myNex.writeNum("fastheattimer.val", fastHeatingCountDown);
    myNex.writeNum("heatingel.val", heatingElementOn);

    if (!isBrewPage(currentPageId))
    {
      rescanRecipes();
    }

    // Check if we're on cleaning page
    if (currentPageId == (uint32_t)CLEANING_PAGE_ID ||
        currentPageId == (uint32_t)(0xFF000000 | CLEANING_PAGE_ID))
    {
      cleaningModeActive = true;
    }
    else
    {
      cleaningShots = 0;
      cleaningShotsWater = 0;
      cleaningModeActive = false;
    }

    pageRefreshTimer = millis();
  }
}

// Controls the dimmer during idle (not brewing).
// When pressure is detected the boiler is full — cut dimmer so pump stops.
// When pressure drops back to zero the boiler needs water — restore dimmer
// to 100% so GiCar can run the pump through it unimpeded.
//
// Cutting the pump in error is the expensive mistake here, because the GiCar
// cannot fill the boiler through a closed dimmer and nothing tells it why. So
// the cut-off is deliberately reluctant: it needs a reading above a threshold
// that ordinary warm-up pressure does not reach, held for BOILER_FULL_DEBOUNCE_MS,
// on a filter that has had PRESSURE_SETTLE_MS to converge — and even then it
// gives the pump back after BOILER_FULL_LATCH_TIMEOUT_MS without pressure.
void updateIdlePumpControl()
{
  if (brewActive) return;  // During brew, pressureProfile() owns the dimmer

  // Do not latch boiler-full before the MaraX reports that it is actually on.
  // During boot, steamTemp is still 0 and the pressure filter may not yet
  // reflect the real sensor value.
  if (!POWER_ON)
  {
    boilerOverPressureSinceMs = 0;
    if (pumpBrightness != 255) setPumpBrightness(255);
    return;
  }

  // During cleaning/backflush the GiCar drives the pump on each lever press.
  // Keep the dimmer fully open so backflush pressure can build — the idle
  // "boiler full" cut-off below must not fight it.
  if (cleaningModeActive || cleaningRunActive)
  {
    boilerOverPressureSinceMs = 0;
    if (pumpBrightness != 255) setPumpBrightness(255);
    return;
  }

  unsigned long now = millis();

  // A latch the pressure no longer backs up is almost certainly a false trip
  // (warm-up expansion, residual line pressure, a noisy sample). Release it
  // rather than holding the pump off until the next brew: the GiCar owns the
  // relay upstream of the dimmer, so an open dimmer only matters if the GiCar
  // is actually asking to fill.
  if (boilerFullLatch && (now - boilerFullLatchedAtMs > BOILER_FULL_LATCH_TIMEOUT_MS))
  {
    Serial.print("[FILL] Boiler-full latch timed out after ");
    Serial.print(BOILER_FULL_LATCH_TIMEOUT_MS / 1000);
    Serial.println("s without pressure — handing the pump back to the GiCar");
    resumeIdleBoilerFill();
  }

  boilerFullPressureBar = (steamTemp > 80) ? BOILER_FULL_BAR_HOT : BOILER_FULL_BAR_COLD;

  // Keep the filter converging, but do not let it latch anything until the
  // seed sample from primePressureFilter() has been averaged away.
  if (now - pressureFilterPrimedAtMs < PRESSURE_SETTLE_MS)
  {
    getPressure();
    boilerOverPressureSinceMs = 0;
    if (!boilerFullLatch && pumpBrightness != 255) setPumpBrightness(255);
    return;
  }

  float p = getPressure();
  if (p > boilerFullPressureBar)
  {
    // Require the cut-off to hold for BOILER_FULL_DEBOUNCE_MS before acting, so
    // one bad reading cannot cut a fill that is already running.
    if (boilerOverPressureSinceMs == 0) boilerOverPressureSinceMs = now;

    if (now - boilerOverPressureSinceMs >= BOILER_FULL_DEBOUNCE_MS)
    {
      if (!boilerFullLatch)
      {
        Serial.print("[FILL] Boiler full — ");
        Serial.print(p, 2);
        Serial.print(" bar over the ");
        Serial.print(boilerFullPressureBar, 2);
        Serial.print(" bar cut-off (steam ");
        Serial.print(steamTemp);
        Serial.println("°C), cutting the pump");
      }
      // Pressure still justifies the latch, so restart the re-arm timeout
      boilerFullLatch = true;
      boilerFullLatchedAtMs = now;
      if (pumpBrightness != 0) setPumpBrightness(0);
    }
  }
  else
  {
    boilerOverPressureSinceMs = 0;
    if (!boilerFullLatch)
    {
      // No pressure and not latched — allow GiCar to run the pump
      if (pumpBrightness != 255) setPumpBrightness(255);
    }
    // If latched and pressure is gone: stay at 0 until the brew ends or the
    // re-arm timeout above releases the latch
  }
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

// How long the profile runs: the time of its last point. Zero means nothing is
// configured, which the callers treat as "no profiling".
float profileTotalSeconds()
{
  if (profilePointCount <= 0)
  {
    return 0.0f;
  }
  return profilePoints[profilePointCount - 1].seconds;
}

// Target pressure (bar) `second` seconds into the shot, interpolated along the
// polyline. Before the first point and after the last one the nearest point's
// pressure is held — running out of profile must not drop the pump to zero
// mid-shot, or a brew-by-weight shot would stop short of its target.
float profileTargetPressureAt(float second)
{
  if (profilePointCount <= 0)
  {
    return 0.0f;
  }
  if (second <= profilePoints[0].seconds)
  {
    return profilePoints[0].bar;
  }

  for (int i = 1; i < profilePointCount; i++)
  {
    const ProfilePoint &previous = profilePoints[i - 1];
    const ProfilePoint &current = profilePoints[i];

    if (second >= current.seconds)
    {
      continue;
    }

    if (current.jump)
    {
      // Not there yet, and this point is a step: the previous pressure stands.
      return previous.bar;
    }

    float span = current.seconds - previous.seconds;
    if (span <= 0.0f)
    {
      // Two points at the same instant — a vertical edge, so the later one wins.
      return current.bar;
    }
    float progress = (second - previous.seconds) / span;
    return previous.bar + progress * (current.bar - previous.bar);
  }

  return profilePoints[profilePointCount - 1].bar;
}

// Cheap fingerprint of the polyline, used to notice that a different recipe
// was loaded — or the active one edited — while the brew page is open. Times
// and pressures are folded in at their 0.1 resolution.
uint32_t profileSegmentSignature()
{
  uint32_t signature = (uint32_t)profilePointCount;
  for (int i = 0; i < profilePointCount; i++)
  {
    signature = signature * 31u + (uint32_t)(profilePoints[i].seconds * 10.0f);
    signature = signature * 31u + (uint32_t)(profilePoints[i].bar * 10.0f);
    signature = signature * 31u + (profilePoints[i].jump ? 1u : 0u);
  }
  return signature;
}

// Pressure (bar) to a waveform sample on the brew page's 0..10 bar scale.
int pressureToWave(float bar)
{
  int wave = (int)((bar / 10.0f) * BREW_WAVEFORM_HEIGHT + 0.5f);
  return constrain(wave, 0, BREW_WAVEFORM_HEIGHT);
}

// Writes the target-pressure bar only when the sample actually changes. With
// ramped profiles the target moves continuously, and pressureProfile() runs on
// every loop iteration — writing it unconditionally would flood UART2 during a
// shot, which is the one time the loop must stay quick.
void writeSetBarValue(int wave)
{
  static int lastWave = -1;
  if (wave == lastWave)
  {
    return;
  }
  lastWave = wave;
  myNex.writeNum("setbar.val", wave);
}

// Draws the whole target curve of the active recipe into the brew page's
// waveform, so the shape of the shot is visible before the lever is pulled.
// The curve is spread across the full width of the graph, so the preview
// always fills it regardless of how long the curve runs.
//
// Two details make this work:
//   * The HMI's own timers (`waveTimer`, `tm0`) call `add` on the waveform
//     whenever the brew page is loaded, which would scroll the preview off
//     within seconds. They are stopped here and started again by brewDetect()
//     when a shot begins, which is the only time the live trace is wanted.
//   * The samples go out with `addt` (transparent data mode), one byte each.
//     350 individual `add` commands would be ~4.9 kB of UART traffic and would
//     block the loop — and therefore brew detection — for close to half a
//     second.
bool drawTargetProfilePreview()
{
  // Freeze the live plotters so the preview stays put.
  sendNextionCommand("waveTimer.en=0");
  sendNextionCommand("tm0.en=0");
  sendNextionCommand("cle " + String(BREW_WAVEFORM_ID) + ",255");

  float totalSeconds = profileTotalSeconds();
  if (!recipeInCharge() || totalSeconds <= 0.0f)
  {
    // Nothing configured to show — an empty graph is the honest answer.
    return true;
  }

  static uint8_t points[BREW_WAVEFORM_WIDTH];
  for (int i = 0; i < BREW_WAVEFORM_WIDTH; i++)
  {
    float second = ((float)i * totalSeconds) / (float)(BREW_WAVEFORM_WIDTH - 1);
    points[i] = (uint8_t)pressureToWave(profileTargetPressureAt(second));
  }

  sendNextionCommand("addt " + String(BREW_WAVEFORM_ID) + "," +
                     String(BREW_WAVEFORM_TARGET_CHANNEL) + "," +
                     String(BREW_WAVEFORM_WIDTH));

  if (!nextionWaitForStatus(0xFE, NEXTION_ADDT_READY_TIMEOUT_MS))
  {
    Serial.println("[DISPLAY] Profile preview: display never signalled ready for addt");
    return false;
  }

  Serial2.write(points, BREW_WAVEFORM_WIDTH);

  if (!nextionWaitForStatus(0xFD, NEXTION_ADDT_DONE_TIMEOUT_MS))
  {
    Serial.println("[DISPLAY] Profile preview: display did not confirm the addt transfer");
    return false;
  }

  Serial.print("[DISPLAY] Curve preview drawn: "); Serial.print(activeRecipe.name);
  Serial.print(" over "); Serial.print(totalSeconds, 1); Serial.print("s, ");
  Serial.print(profilePointCount); Serial.println(" points");
  return true;
}

void pressureProfile()
{
  // Without a recipe in charge the pump stays at the full power brewDetect()
  // gave it and nothing cuts the shot: the machine brews as a stock Mara X.
  if (brewActive && recipeInCharge())
  {
    float targetWeight = activeTargetWeight();

    // Brew-by-weight auto-stop is only valid when a scale is connected and
    // we have received at least one weight sample in this session.
    bool brewByWeightActive =
        scaleConnected &&
        targetWeight > 0.0f &&
        lastWeightTime > 0;

    // Cut-off latch: once the shot has been cut, keep the pump off for the
    // rest of this brew. Without this latch the pressure drop after the pump
    // stops would shrink the offset again, and the profile segments below
    // would switch the pump back on.
    if (shotCutOff)
    {
      setPumpBrightness(0);
      return;
    }

    float brewSecs = (millis() - activeBrewingStart) / 1000.0f;

    if (brewByWeightActive)
    {
      float pressureNow = getPressure();
      if (currentWeight >= targetWeight - weightStopOffset(pressureNow))
      {
        // Start stability window from the exact moment target stop is triggered.
        if (!pendingObservation)
        {
          startPendingObservation(flowRate, pressureNow);
        }
        shotCutOff = true;
        setPumpBrightness(0);
        writeBrewTimerColour(1535);
        writeSetBarValue(0);
        considerShotTime(brewSecs);
        return;
      }
    }
    // No scale to weigh the yield: stop when the last shot of this recipe the
    // scale stopped did. The scale dropping out mid-shot lands here too.
    else if (activeShotSeconds > 0.0f && brewSecs >= activeShotSeconds)
    {
      shotCutOff = true;
      setPumpBrightness(0);
      writeBrewTimerColour(1535);
      writeSetBarValue(0);
      Serial.print("[SHOT] No scale — stopped at "); Serial.print(activeShotSeconds, 1);
      Serial.println("s, the recipe's remembered time");
      return;
    }

    // Safety: a curve with no duration is not usable. Skip it so the pump is
    // not silently killed. This is checked after the cut-offs above so they
    // still stop the shot either way.
    float totalProfileSeconds = profileTotalSeconds();
    if (totalProfileSeconds <= 0.0f)
    {
      setPumpBrightness(255);  // No usable curve → full power
      return;
    }

    // profileTargetPressureAt() holds the last point's pressure once the
    // profile has run out, letting a cut-off above — or the lever — end the
    // shot. Cutting the pump here would end a brew-by-weight shot short
    // of its target whenever the profile is shorter than the shot takes.
    float target = profileTargetPressureAt(brewSecs);
    writeSetBarValue(pressureToWave(target));
    setPressure(target);

    if (brewSecs > totalProfileSeconds)
    {
      writeBrewTimerColour(1535);
    }
    else if (totalProfileSeconds - brewSecs <= 3.0f)
    {
      writeBrewTimerColour(64864);
    }
  }
}

// Recolours the brew timer, skipping the write when the colour is unchanged —
// this runs on every loop iteration during a shot.
void writeBrewTimerColour(uint32_t colour)
{
  static uint32_t lastColour = 0xFFFFFFFFu;
  if (colour == lastColour)
  {
    return;
  }
  lastColour = colour;
  myNex.writeNum("n0.pco", colour);
  myNex.writeNum("n1.pco", colour);
}

// Writes the brew button picture only when it actually changes. Sending it on
// every idle loop iteration flooded UART2 for no benefit.
void writeBrewButtonPicture(int picture)
{
  static int lastBrewButtonPicture = -1;
  if (picture == lastBrewButtonPicture)
  {
    return;
  }
  lastBrewButtonPicture = picture;
  myNex.writeNum("pBrew.pic", picture);
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

      // Wipe the target-curve preview and hand the waveform back to the HMI's
      // own plotters, which drawTargetProfilePreview() had stopped.
      sendNextionCommand("cle " + String(BREW_WAVEFORM_ID) + ",255");
      sendNextionCommand("waveTimer.en=1");
      sendNextionCommand("tm0.en=1");
      brewPreviewNeedsDraw = false;
      // The lever can be pulled from any page, so the brew page may only just
      // have loaded — with the recipe panel at its design-time values.
      writeRecipePanel();

      brewTimer(true);
      writeBrewButtonPicture(25);
      brewActive = true;
      shotCutOff = false;  // Fresh brew — clear the cut-off latch
      startShotEvidence();

      if (recipeInCharge() && activeRecipe.yield > 0.0f && !(scaleConnected && lastWeightTime > 0))
      {
        Serial.print("[SHOT] No scale — ");
        if (activeShotSeconds > 0.0f)
        {
          Serial.print("stopping after "); Serial.print(activeShotSeconds, 1);
          Serial.println("s, as long as the scale last took to the yield");
        }
        else
        {
          Serial.println("no remembered time for this recipe, the lever ends the shot");
        }
      }

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
    writeBrewButtonPicture(8);

    // Only hand the dimmer back to idle control on the brew→idle edge.
    // Doing this every idle loop fought updateIdlePumpControl() (255↔0
    // chatter) and reset boilerFullLatch before it could ever hold.
    if (wasBrewing)
    {
      shotCutOff = false;         // Clear cut-off latch for next brew
      resumeIdleBoilerFill();     // Unlock idle control and hand full pump control back to GiCar
      saveRememberedShotTime();   // Out of the shot, so the NVS write cannot hold up pump control
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
  static unsigned long lastFrameReceivedMs = 0;
  bool newFrameThisIteration = false;
  
  while (Serial1.available())
  {
    rc = Serial1.read();

    // Uncomment for raw byte debugging (disabled by default - confirmed working)
    // Serial.print("[RAW] 0x");
    // if ((uint8_t)rc < 0x10) Serial.print("0");
    // Serial.print((uint8_t)rc, HEX);
    // Serial.print(" (");
    // if (rc >= 0x20 && rc < 0x7F) Serial.print(rc); else Serial.print('?');
    // Serial.println(")");
    
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
      newFrameThisIteration = true;
      lastFrameReceivedMs = millis();
      
      // Only log frame if it contains new/changed data (not every poll)
      static int lastBrewTemp = -1;
      static int lastSteamTemp = -1;
      bool tempChanged = false;

      // Parse CSV format: +1.10,103,128,077,0000,1,0
      // Manual split to handle empty fields correctly (strtok skips them)
      // Field 0: Version (+1.10)
      // Field 1: Steam temp (103 = 103°C)
      // Field 2: Steam target temp (128°C, or empty)
      // Field 3: Brew temp (077 = 77°C)
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
      // field[1]=steam temp  field[2]=steam target  field[3]=brew temp
      // field[4]=countdown   field[5]=heating
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
      if (fieldCount > 1 && fields[1][0] != '\0')
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
