/*
 * FelicitaScale_NimBLE.h
 * 
 * NimBLE-based implementation for Felicita Arc scale on ESP32-S3
 * 
 * Protocol source:
 * - https://github.com/graphefruit/Beanconqueror/tree/master/src/classes/devices/felicita
 * 
 * Felicita BLE:
 *   Service:        0xFFE0
 *   Characteristic: 0xFFE1  (write + notify, same characteristic)
 * 
 * Weight packet format (18 bytes):
 *   [0-2]  unknown
 *   [3-8]  weight digits as ASCII (subtract 48), indices 7-8 are decimal places
 *          e.g. bytes [49,48,49,48,49,48] = '101010' => 1010.10g
 *   [9-10] unit as ASCII text ('g ' or 'oz')
 *   [11-14] unknown
 *   [15]   battery (129=min, 158=max)
 *   [16-17] CR LF
 */

#ifndef FELICITASCALE_NIMBLE_H
#define FELICITASCALE_NIMBLE_H

#include <Arduino.h>
#include <NimBLEDevice.h>

// Felicita Arc BLE UUIDs (confirmed by BLE discovery)
#define FELICITA_SERVICE_UUID      "ffe0"
#define FELICITA_CHAR_UUID         "ffe1"  // Single char for both write and notify

// Felicita command bytes (single-byte commands)
#define FELICITA_CMD_TARE          0x54  // 'T'
#define FELICITA_CMD_START_TIMER   0x52  // 'R'
#define FELICITA_CMD_STOP_TIMER    0x53  // 'S'
#define FELICITA_CMD_RESET_TIMER   0x43  // 'C'
#define FELICITA_CMD_TOGGLE_UNIT   0x55  // 'U'

class FelicitaScale_NimBLE {
private:
    NimBLEClient *pClient = nullptr;
    NimBLERemoteCharacteristic *pWriteChar = nullptr;
    NimBLERemoteCharacteristic *pNotifyChar = nullptr;
    NimBLEScan *pScan = nullptr;
    
    String targetMAC;
    bool debug;
    bool connected = false;
    bool weightDataAvailable = false;
    bool scanInProgress = false;
    
    float currentWeight = 0.0f;
    float battery = 0.0f;
    unsigned long lastHeartbeat = 0;
    unsigned long lastWeightUpdate = 0;
    
    static const uint32_t HEARTBEAT_INTERVAL_MS = 2000;  // Felicita doesn't need heartbeats but we use this to detect stale connection
    static const uint32_t SCAN_DURATION_MS = 5000;
    
    // Store instance pointer for callback access
    static FelicitaScale_NimBLE *callbackInstance;
    
    // Notification callback (static, forwards to instance method)
    static void notifyCallback(NimBLERemoteCharacteristic *pChar, uint8_t *pData, size_t length, bool isNotify) {
        if (callbackInstance) {
            callbackInstance->parseWeightData(pData, length);
        }
    }
    
    // Parse Felicita weight notification packet
    // Packet is 18 bytes:
    //   [0-2]  unknown
    //   [3-8]  6 ASCII digit chars: indices 3-7 = integer part, index 8 = first decimal, wait...
    //          Actually: bytes 3-8 are the 6 weight digits. Subtract 48 from each.
    //          The last 2 digits (indices 7-8) are decimal places.
    //          e.g. [49,48,49,48,49,48] -> digits 1,0,1,0,1,0 -> 1010.10g
    //   [9-10] unit chars ('g ' or 'oz')
    //   [15]   battery raw (129=empty, 158=full)
    void parseWeightData(uint8_t *data, size_t length) {
        if (length < 16) return;
        
        // Extract 6 weight digit bytes at positions 3-8
        // Each byte is an ASCII digit (ASCII '0' = 48)
        int digits[6];
        for (int i = 0; i < 6; i++) {
            digits[i] = (int)data[3 + i] - 48;
            if (digits[i] < 0 || digits[i] > 9) return;  // Not a valid digit
        }
        
        // Digits 0-3 are integer part, digits 4-5 are decimal (2 decimal places)
        // e.g. digits [1,0,1,0,1,0] = 1010.10g
        float weight = (digits[0] * 1000.0f + digits[1] * 100.0f +
                        digits[2] * 10.0f   + digits[3] * 1.0f +
                        digits[4] * 0.1f    + digits[5] * 0.01f);
        
        currentWeight = weight;
        weightDataAvailable = true;
        lastWeightUpdate = millis();
        lastHeartbeat = millis();  // Reset heartbeat timer on any packet
        
        // Battery: byte 15, range 129-158
        if (length > 15) {
            float rawBat = data[15];
            battery = constrain((rawBat - 129.0f) / (158.0f - 129.0f) * 100.0f, 0.0f, 100.0f);
        }
        
        if (debug) {
            Serial.print("[SCALE] Weight: ");
            Serial.print(currentWeight, 2);
            Serial.print("g  Battery: ");
            Serial.print((int)battery);
            Serial.println("%");
        }
    }
    
    // Send single-byte command to Felicita scale
    bool sendCommand(uint8_t cmd) {
        if (!connected || !pWriteChar) return false;
        pWriteChar->writeValue(&cmd, 1, false);
        if (debug) {
            Serial.print("[SCALE] Sent cmd 0x");
            Serial.println(cmd, HEX);
        }
        return true;
    }

    bool ensureBleReady() {
        static bool nimbleInitialized = false;

        if (!nimbleInitialized) {
            NimBLEDevice::init("MaraXController");
            NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Max power for all BLE operations (scan, connect, advertise)
            nimbleInitialized = true;
            if (debug) Serial.println("[SCALE] NimBLE initialized with max power");
        }

        if (!pScan) {
            pScan = NimBLEDevice::getScan();
            if (!pScan) {
                if (debug) Serial.println("[SCALE] ERROR: Failed to get scan object!");
                return false;
            }
        }

        // NOTE: NimBLE v2.x uses milliseconds for interval/window (not 0.625ms units).
        pScan->setActiveScan(true);
        pScan->setInterval(100);
        pScan->setWindow(100);
        pScan->setDuplicateFilter(false);
        return true;
    }

    bool isTargetDevice(const NimBLEAdvertisedDevice *device) {
        if (!device) return false;

        String deviceName = String(device->getName().c_str());
        String deviceAddr = String(device->getAddress().toString().c_str());

        if (targetMAC.length() > 0) {
            return deviceAddr.equalsIgnoreCase(targetMAC);
        }

        return deviceName.indexOf("ACAIA") >= 0 ||
               deviceName.indexOf("FELICITA") >= 0 ||
               deviceName.indexOf("LUNAR") >= 0 ||
               deviceName.indexOf("PYXIS") >= 0;
    }

public:
    FelicitaScale_NimBLE(bool enableDebug = false) : debug(enableDebug) {
        callbackInstance = this;  // Set static instance pointer for callbacks
    }
    
    ~FelicitaScale_NimBLE() {
        if (pClient) {
            disconnect();
        }
        if (callbackInstance == this) {
            callbackInstance = nullptr;
        }
    }
    
    // Start a background connection attempt.
    // macAddress: BLE MAC address (e.g. "B0:10:A0:8E:81:67") or empty string for auto-discovery
    bool beginConnect(const char *macAddress = "") {
        targetMAC = String(macAddress);

        if (isConnected()) {
            connected = true;
            return true;
        }

        if (connected) {
            disconnect();
        }

        if (debug) {
            Serial.println("[SCALE] Initializing NimBLE...");
            if (targetMAC.length() > 0) {
                Serial.print("[SCALE] Scanning for MAC: ");
                Serial.println(targetMAC);
            } else {
                Serial.println("[SCALE] Auto-discovering Felicita/Acaia scale...");
            }
        }

        if (!ensureBleReady()) {
            return false;
        }

        if (pScan->isScanning()) {
            scanInProgress = true;
            return false;
        }

        pScan->clearResults();

        if (debug) {
            Serial.print("[SCALE] Starting ");
            Serial.print(SCAN_DURATION_MS);
            Serial.println("ms BLE scan in background...");
        }

        if (!pScan->start(SCAN_DURATION_MS, false, true)) {
            if (debug) Serial.println("[SCALE] ERROR: Failed to start BLE scan");
            scanInProgress = false;
            return false;
        }

        scanInProgress = true;
        return false;
    }

    bool init(const char *macAddress = "") {
        return beginConnect(macAddress);
    }

    bool pollConnect() {
        if (isConnected()) {
            connected = true;
            return true;
        }

        if (!scanInProgress || !pScan) {
            return false;
        }

        if (pScan->isScanning()) {
            return false;
        }

        scanInProgress = false;
        NimBLEScanResults results = pScan->getResults();

        if (debug) {
            Serial.print("[SCALE] Background scan completed, found ");
            Serial.print(results.getCount());
            Serial.println(" devices:");
        }

        for (int i = 0; i < results.getCount(); i++) {
            const NimBLEAdvertisedDevice *device = results.getDevice(i);
            if (!device) continue;

            String deviceName = String(device->getName().c_str());
            String deviceAddr = String(device->getAddress().toString().c_str());

            if (debug) {
                Serial.print("[SCALE]   [");
                Serial.print(i);
                Serial.print("] ");
                Serial.print(deviceAddr);
                Serial.print(" - ");
                Serial.print(deviceName.length() > 0 ? deviceName : "(no name)");
                Serial.print(" RSSI:");
                Serial.println(device->getRSSI());
            }

            if (!isTargetDevice(device)) {
                if (debug && targetMAC.length() > 0) {
                    Serial.print("[SCALE]       Comparing '");
                    Serial.print(deviceAddr);
                    Serial.print("' with target '");
                    Serial.print(targetMAC);
                    Serial.println("' - no match");
                }
                continue;
            }

            if (debug) {
                Serial.print("[SCALE] Found: ");
                Serial.print(deviceName);
                Serial.print(" (");
                Serial.print(deviceAddr);
                Serial.println(")");
            }

            bool connectedNow = connectToScale(device);
            pScan->clearResults();
            return connectedNow;
        }

        pScan->clearResults();
        if (debug) {
            Serial.println("[SCALE] Scale not found");
        }
        return false;
    }

    bool isConnectionAttemptInProgress() {
        return scanInProgress || (pScan && pScan->isScanning());
    }

    void cancelPendingConnect() {
        if (pScan && pScan->isScanning()) {
            pScan->stop();
        }
        if (pScan) {
            pScan->clearResults();
        }
        scanInProgress = false;
    }
    
    // Connect to discovered scale
    bool connectToScale(const NimBLEAdvertisedDevice *device) {
        if (pClient) {
            disconnect();
        }

        pClient = NimBLEDevice::createClient();
        if (!pClient->connect(device)) {
            if (debug) Serial.println("[SCALE] Connection failed");
            disconnect();
            return false;
        }
        
        if (debug) Serial.println("[SCALE] Connected, discovering services...");
        
        // Felicita Arc uses service 0xFFE0, characteristic 0xFFE1 (write + notify)
        NimBLERemoteService *pService = pClient->getService(FELICITA_SERVICE_UUID);
        if (!pService) {
            if (debug) {
                // Log actual services for debugging
                const std::vector<NimBLERemoteService*> &services = pClient->getServices(true);
                Serial.print("[SCALE] Service 0xFFE0 not found. Found ");
                Serial.print(services.size());
                Serial.println(" services:");
                for (NimBLERemoteService *svc : services) {
                    Serial.print("[SCALE]   ");
                    Serial.println(svc->getUUID().toString().c_str());
                }
            }
            disconnect();
            return false;
        }
        
        // Get the single FFE1 characteristic (used for both write and notify)
        pWriteChar = pService->getCharacteristic(FELICITA_CHAR_UUID);
        if (!pWriteChar) {
            if (debug) Serial.println("[SCALE] Characteristic 0xFFE1 not found");
            disconnect();
            return false;
        }
        pNotifyChar = pWriteChar;  // Same characteristic for notifications
        
        if (debug) Serial.println("[SCALE] Found Felicita service/characteristic");
        
        // Subscribe to notifications
        if (pNotifyChar->canNotify()) {
            pNotifyChar->subscribe(true, notifyCallback);
        }
        
        connected = true;
        lastHeartbeat = millis();
        
        // Felicita doesn't need initialization commands - it streams weight automatically
        // after subscribing to notifications
        
        if (debug) Serial.println("[SCALE] Ready! Receiving weight notifications.");
        return true;
    }
    
    // Disconnect from scale
    void disconnect() {
        if (pClient) {
            pClient->disconnect();
            NimBLEDevice::deleteClient(pClient);  // Don't use delete directly - NimBLE manages clients
            pClient = nullptr;
        }
        pWriteChar = nullptr;
        pNotifyChar = nullptr;
        connected = false;
    }
    
    // Check if connected
    bool isConnected() {
        return connected && pClient && pClient->isConnected();
    }
    
    // heartbeat: Felicita streams data automatically, no keepalive needed.
    // We use lastHeartbeat as a staleness check - if no packet in 5s, connection is dead.
    bool heartbeatRequired() {
        return (millis() - lastHeartbeat) > 5000;  // 5s without data = dead
    }
    
    void heartbeat() {
        // No-op for Felicita - just reset the timer to prevent false disconnects
        // (called by updateScale() when heartbeatRequired() is true, which means
        //  we actually haven't received data - this indicates a real disconnect)
        lastHeartbeat = millis();
    }
    
    // Check if new weight data is available
    bool newWeightAvailable() {
        if (weightDataAvailable) {
            weightDataAvailable = false;
            return true;
        }
        return false;
    }
    
    // Get current weight in grams
    float getWeight() {
        return currentWeight;
    }
    
    // Get battery level (0-100%)
    float getBattery() {
        return battery;
    }
    
    // Tare the scale
    bool tare() {
        return sendCommand(FELICITA_CMD_TARE);
    }
    
    // Start timer
    bool startTimer() {
        return sendCommand(FELICITA_CMD_START_TIMER);
    }
    
    // Stop timer
    bool stopTimer() {
        return sendCommand(FELICITA_CMD_STOP_TIMER);
    }
    
    // Reset timer
    bool resetTimer() {
        return sendCommand(FELICITA_CMD_RESET_TIMER);
    }
};

// Define static member
FelicitaScale_NimBLE *FelicitaScale_NimBLE::callbackInstance = nullptr;

#endif // FELICITASCALE_NIMBLE_H
