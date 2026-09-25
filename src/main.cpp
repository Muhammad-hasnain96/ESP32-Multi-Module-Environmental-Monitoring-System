// =====================================================================
// ESP32-S3 — GENERIC SMART SENSOR HUB (True Auto-Detecting System)
// =====================================================================
// Plug-and-Play Hub Architecture:
//   - DHT11 Auto-Discovery: Scans GPIOs (4, 5, 6, 7, 1, 2, 8, 9, 10)
//     via 20ms protocol handshake pulse.
//   - I2C Auto-Discovery: Scans pin pairs for BH1750 (0x23), CCS811 (0x5A),
//     and 2004 Character LCD (0x27 / 0x3F).
//   - Dynamic 2004 LCD: Displays readings of whichever sensors are online!
//   - ThingsBoard Cloud: Sends telemetry for active sensors.
//
// Access Token: 2HGvWTV145aFOdJbjAwQ
// Server: thingsboard.cloud
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <Adafruit_CCS811.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// =====================================================================
//  WiFi & ThingsBoard Configuration
// =====================================================================
const char* WIFI_SSID = "FAST-1144";
const char* WIFI_PASS = "12345678";
const char* TB_HOST   = "thingsboard.cloud";
const char* TB_TOKEN  = "2HGvWTV145aFOdJbjAwQ";
const int   TB_PORT   = 80;

// Update intervals
const unsigned long SENSOR_INTERVAL = 3000;   // Sensor read & telemetry every 3s
const unsigned long LCD_PAGE_TIME   = 4000;   // Rotate LCD screen every 4s

// =====================================================================
//  Candidate Pin Definitions for Auto-Discovery
// =====================================================================
struct I2CPair {
    int sda;
    int scl;
    const char* label;
};

// Candidate I2C pin pairs on ESP32-S3
const I2CPair I2C_CANDIDATES[] = {
    { 17, 18, "SDA=17, SCL=18" },
    { 15, 16, "SDA=15, SCL=16" },
    { 8,  9,  "SDA=8,  SCL=9"  },
    { 1,  2,  "SDA=1,  SCL=2"  },
    { 4,  5,  "SDA=4,  SCL=5"  }
};
const int NUM_I2C_CANDIDATES = sizeof(I2C_CANDIDATES) / sizeof(I2C_CANDIDATES[0]);

// Candidate single-wire GPIOs for DHT11 auto-scan
const int DHT_CANDIDATES[] = { 4, 5, 6, 7, 1, 2, 8, 9, 10 };
const int NUM_DHT_CANDIDATES = sizeof(DHT_CANDIDATES) / sizeof(DHT_CANDIDATES[0]);

// =====================================================================
//  Global Sensor State & Pointers
// =====================================================================
// 1. I2C Bus Active Pins
int activeI2C_SDA = -1;
int activeI2C_SCL = -1;

// 2. 2004 LCD Display
LiquidCrystal_I2C* pLcd = nullptr;
bool lcdFound = false;
uint8_t lcdAddr = 0x27;

// 3. BH1750 Ambient Light Sensor
BH1750 lightMeter;
bool bh1750Found = false;
float currentLux = 0.0f;

// 4. CCS811 eCO2 / TVOC Sensor
Adafruit_CCS811 ccs;
bool ccsFound = false;
uint16_t currentCO2  = 400;
uint16_t currentTVOC = 0;

// 5. DHT11 Temperature & Humidity
DHT* pDht = nullptr;
int  dhtPin = -1;
bool dhtFound = false;
float currentTempC = 0.0f;
float currentTempF = 0.0f;
float currentHum   = 0.0f;
float currentHI    = 0.0f;
int   dhtFailCount = 0;

// Timing trackers
unsigned long lastSensorRead = 0;
unsigned long lastLcdSwitch  = 0;
int lcdPage = 0;
unsigned long loopCount = 0;

// =====================================================================
//  Utility: Uptime String & Banner Separators
// =====================================================================
String getUptime() {
    unsigned long sec = millis() / 1000;
    unsigned long m = (sec / 60) % 60;
    unsigned long h = (sec / 3600);
    unsigned long s = sec % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", h, m, s);
    return String(buf);
}

void printLine(char c = '=') {
    for (int i = 0; i < 62; i++) Serial.print(c);
    Serial.println();
}

// =====================================================================
//  Auto-Discovery: DHT11 Handshake Probe
// =====================================================================
// Sends a 20ms LOW start pulse, releases line with internal pull-up,
// and checks for the DHT11's unique 80us LOW + 80us HIGH response.
bool probeDHT11(int pin) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delay(20);                     // Start pulse (min 18ms)
    digitalWrite(pin, HIGH);
    delayMicroseconds(30);
    pinMode(pin, INPUT_PULLUP);

    // Wait for DHT11 to pull line LOW
    unsigned long t0 = micros();
    while (digitalRead(pin) == HIGH) {
        if (micros() - t0 > 150) {
            pinMode(pin, INPUT);
            return false;
        }
    }

    // Measure DHT11 LOW response pulse (~80us)
    t0 = micros();
    while (digitalRead(pin) == LOW) {
        if (micros() - t0 > 150) {
            pinMode(pin, INPUT);
            return false;
        }
    }

    // Measure DHT11 HIGH response pulse (~80us)
    t0 = micros();
    while (digitalRead(pin) == HIGH) {
        if (micros() - t0 > 150) {
            pinMode(pin, INPUT);
            return false;
        }
    }

    return true; // Valid DHT11 handshake confirmed!
}

// =====================================================================
//  Auto-Discovery: I2C Scanner
// =====================================================================
void scanAndInitI2C() {
    Serial.println(F("  [>>] Scanning candidate pin pairs for I2C devices..."));

    // Target devices we look for:
    // LCD (0x27, 0x3F), BH1750 (0x23, 0x5C), CCS811 (0x5A, 0x5B)
    const uint8_t TARGET_ADDRS[] = { 0x27, 0x3F, 0x23, 0x5C, 0x5A, 0x5B };
    const int NUM_TARGETS = sizeof(TARGET_ADDRS) / sizeof(TARGET_ADDRS[0]);

    for (int p = 0; p < NUM_I2C_CANDIDATES; p++) {
        int sda = I2C_CANDIDATES[p].sda;
        int scl = I2C_CANDIDATES[p].scl;

        Wire.end();
        Wire.begin(sda, scl, 100000);
        Wire.setTimeOut(25); // Prevent hanging if bus is floating
        delay(20);

        int devicesOnThisPair = 0;
        bool hasLCD = false, hasBH = false, hasCCS = false;
        uint8_t foundLcdAddr = 0;

        for (int i = 0; i < NUM_TARGETS; i++) {
            uint8_t addr = TARGET_ADDRS[i];
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                devicesOnThisPair++;
                if (addr == 0x27 || addr == 0x3F) {
                    hasLCD = true;
                    foundLcdAddr = addr;
                } else if (addr == 0x23 || addr == 0x5C) {
                    hasBH = true;
                } else if (addr == 0x5A || addr == 0x5B) {
                    hasCCS = true;
                }
            }
        }

        if (devicesOnThisPair > 0) {
            activeI2C_SDA = sda;
            activeI2C_SCL = scl;
            Serial.printf("    [+] I2C Bus LOCKED on %s (%d device%s found)\n",
                          I2C_CANDIDATES[p].label, devicesOnThisPair,
                          devicesOnThisPair > 1 ? "s" : "");

            // Initialize LCD if detected
            if (hasLCD && !lcdFound) {
                lcdAddr = foundLcdAddr;
                pLcd = new LiquidCrystal_I2C(lcdAddr, 20, 4);
                pLcd->init();
                pLcd->backlight();
                pLcd->clear();
                pLcd->setCursor(0, 0);
                pLcd->print(F("ESP32-S3 GENERIC HUB"));
                pLcd->setCursor(0, 1);
                pLcd->print(F("Auto-Detecting...   "));
                lcdFound = true;
                Serial.printf("        -> 2004 LCD Initialized at 0x%02X\n", lcdAddr);
            }

            // Initialize BH1750 if detected
            if (hasBH && !bh1750Found) {
                if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
                    bh1750Found = true;
                    Serial.println(F("        -> BH1750 Light Sensor Online at 0x23"));
                }
            }

            // Initialize CCS811 if detected
            if (hasCCS && !ccsFound) {
                if (ccs.begin(0x5A, &Wire)) {
                    ccsFound = true;
                    Serial.println(F("        -> CCS811 CO2/TVOC Sensor Online at 0x5A"));
                }
            }
            break; // Active I2C pair selected
        }
    }

    if (activeI2C_SDA == -1) {
        Serial.println(F("    [-] No I2C devices detected on any candidate pair."));
    }
}

// =====================================================================
//  Auto-Discovery: DHT11 Pin Scanner
// =====================================================================
void scanAndInitDHT() {
    Serial.println(F("  [>>] Scanning GPIO pins for DHT11 handshake pulse..."));

    for (int i = 0; i < NUM_DHT_CANDIDATES; i++) {
        int pin = DHT_CANDIDATES[i];

        // Skip pins already reserved for active I2C bus
        if (pin == activeI2C_SDA || pin == activeI2C_SCL) {
            continue;
        }

        if (probeDHT11(pin)) {
            dhtPin = pin;
            dhtFound = true;
            dhtFailCount = 0;
            if (pDht != nullptr) delete pDht;
            pDht = new DHT(dhtPin, DHT11);
            pDht->begin();
            Serial.printf("    [+] DHT11 DETECTED & INITIALIZED on GPIO %d!\n", dhtPin);
            return;
        }
    }

    Serial.println(F("    [-] DHT11 not detected on candidate pins."));
}

// =====================================================================
//  WiFi Connection (Non-blocking retry)
// =====================================================================
void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt < 15000 && lastAttempt != 0) return;
    lastAttempt = millis();

    Serial.print(F("  [..] WiFi Connecting to "));
    Serial.println(WIFI_SSID);
    WiFi.disconnect(true);
    delay(50);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

// =====================================================================
//  ThingsBoard Telemetry Transmitter
// =====================================================================
void sendThingsBoardTelemetry() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/%s/telemetry", TB_HOST, TB_PORT, TB_TOKEN);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    // Dynamically build JSON payload based on which sensors are active
    String payload = "{";
    payload += "\"hub_status\":\"ONLINE\",";
    payload += "\"loop_count\":" + String(loopCount) + ",";
    payload += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
    payload += "\"uptime_sec\":" + String(millis() / 1000) + ",";

    // DHT11 telemetry
    if (dhtFound) {
        payload += "\"dht11_online\":true,";
        payload += "\"dht11_pin\":" + String(dhtPin) + ",";
        payload += "\"temperature\":" + String(currentTempC, 1) + ",";
        payload += "\"temperature_f\":" + String(currentTempF, 1) + ",";
        payload += "\"humidity\":" + String(currentHum, 1) + ",";
        payload += "\"heatIndex\":" + String(currentHI, 1) + ",";
    } else {
        payload += "\"dht11_online\":false,";
    }

    // BH1750 telemetry
    if (bh1750Found) {
        payload += "\"bh1750_online\":true,";
        payload += "\"light_lux\":" + String(currentLux, 1) + ",";
    } else {
        payload += "\"bh1750_online\":false,";
    }

    // CCS811 telemetry
    if (ccsFound) {
        payload += "\"ccs811_online\":true,";
        payload += "\"co2_ppm\":" + String(currentCO2) + ",";
        payload += "\"tvoc_ppb\":" + String(currentTVOC) + ",";
    } else {
        payload += "\"ccs811_online\":false,";
    }

    // Close JSON
    if (payload.endsWith(",")) {
        payload.remove(payload.length() - 1);
    }
    payload += "}";

    int code = http.POST(payload);
    Serial.printf("  CLOUD -> ThingsBoard Telemetry (Token: %s...) ... HTTP %d\n",
                  String(TB_TOKEN).substring(0, 6).c_str(), code);
    http.end();
}

// =====================================================================
//  2004 LCD Display (Dynamic Page Rotator)
// =====================================================================
void updateLCD() {
    if (!lcdFound || pLcd == nullptr) return;

    pLcd->clear();
    char buf[21];

    if (lcdPage == 0) {
        // --- SCREEN 1: Generic Hub Status & Auto-Detected Ports ---
        pLcd->setCursor(0, 0);
        pLcd->print(F("ESP32-S3 GENERIC HUB"));

        pLcd->setCursor(0, 1);
        if (dhtFound) {
            snprintf(buf, sizeof(buf), "DHT11: GPIO %-2d [ON] ", dhtPin);
        } else {
            snprintf(buf, sizeof(buf), "DHT11: NOT FOUND    ");
        }
        pLcd->print(buf);

        pLcd->setCursor(0, 2);
        if (bh1750Found) {
            snprintf(buf, sizeof(buf), "BH1750 Light:   [ON] ");
        } else {
            snprintf(buf, sizeof(buf), "BH1750 Light:  [OFF] ");
        }
        pLcd->print(buf);

        pLcd->setCursor(0, 3);
        if (ccsFound) {
            snprintf(buf, sizeof(buf), "CCS811 CO2:     [ON] ");
        } else {
            snprintf(buf, sizeof(buf), "CCS811 CO2:    [OFF] ");
        }
        pLcd->print(buf);

    } else if (lcdPage == 1) {
        // --- SCREEN 2: Live Sensor Readings ---
        pLcd->setCursor(0, 0);
        if (dhtFound) {
            snprintf(buf, sizeof(buf), "T:%4.1fC  H:%4.1f%%", currentTempC, currentHum);
        } else {
            snprintf(buf, sizeof(buf), "DHT11: (Not Pluggd)");
        }
        pLcd->print(buf);

        pLcd->setCursor(0, 1);
        if (bh1750Found) {
            snprintf(buf, sizeof(buf), "Light: %6.1f Lux   ", currentLux);
        } else {
            snprintf(buf, sizeof(buf), "Light: (Not Pluggd) ");
        }
        pLcd->print(buf);

        pLcd->setCursor(0, 2);
        if (ccsFound) {
            snprintf(buf, sizeof(buf), "CO2: %4u ppm", currentCO2);
        } else {
            snprintf(buf, sizeof(buf), "CO2: (Not Pluggd)  ");
        }
        pLcd->print(buf);

        pLcd->setCursor(0, 3);
        snprintf(buf, sizeof(buf), "WiFi:%-3s IP:..%-4s",
                 WiFi.status() == WL_CONNECTED ? "OK" : "NO",
                 WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().substring(10).c_str() : "OFF");
        pLcd->print(buf);
    }
}

// =====================================================================
//  Setup Routine
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.setTxTimeoutMs(0);

    Serial.println();
    printLine('=');
    Serial.println(F("  ESP32-S3 GENERIC AUTO-SENSING HUB (PLUG & PLAY)"));
    Serial.println(F("  Features: Auto Pin Scanner, I2C Auto-Discovery & Cloud"));
    printLine('=');

    // 1. Scan and initialize I2C Devices
    scanAndInitI2C();

    // 2. Scan and initialize DHT11
    scanAndInitDHT();

    // 3. Connect to WiFi
    connectWiFi();

    // 4. Initial LCD splash
    if (lcdFound && pLcd != nullptr) {
        delay(1200);
        updateLCD();
    }

    printLine('=');
    Serial.println(F("  System Initialized. Entering Main Telemetry Loop..."));
    Serial.println();
}

// =====================================================================
//  Main Loop
// =====================================================================
void loop() {
    connectWiFi();

    unsigned long now = millis();

    // Periodic sensor read and telemetry transmission
    if (now - lastSensorRead >= SENSOR_INTERVAL) {
        lastSensorRead = now;
        loopCount++;

        // 1. Read DHT11 (if discovered)
        if (dhtFound && pDht != nullptr) {
            float h = pDht->readHumidity();
            float t = pDht->readTemperature();
            if (!isnan(h) && !isnan(t)) {
                currentHum   = h;
                currentTempC = t;
                currentTempF = (t * 9.0f / 5.0f) + 32.0f;
                currentHI    = pDht->computeHeatIndex(t, h, false);
                dhtFailCount = 0;
            } else {
                dhtFailCount++;
                if (dhtFailCount >= 2) {
                    Serial.printf("  [!] DHT11 UNPLUGGED from GPIO %d! Re-enabling auto pin scan...\n", dhtPin);
                    delete pDht;
                    pDht = nullptr;
                    dhtFound = false;
                    dhtPin = -1;
                    dhtFailCount = 0;
                    currentHum = 0.0f;
                    currentTempC = 0.0f;
                    currentTempF = 0.0f;
                    currentHI = 0.0f;
                }
            }
        }

        // 2. Read BH1750 Light (if discovered)
        if (bh1750Found) {
            float lux = lightMeter.readLightLevel();
            if (lux >= 0.0f) {
                currentLux = lux;
            }
        }

        // 3. Read CCS811 CO2 (if discovered)
        if (ccsFound) {
            if (ccs.available() && !ccs.readData()) {
                currentCO2  = ccs.geteCO2();
                currentTVOC = ccs.getTVOC();
            }
        }

        // 4. Print Dashboard to Serial Monitor
        Serial.println();
        printLine('=');
        Serial.printf("  GENERIC HUB | READING #%-4lu | UPTIME: %s | WiFi: %s\n",
                      loopCount, getUptime().c_str(),
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Disconnected");
        printLine('-');

        // Section: DHT11
        if (dhtFound) {
            Serial.printf("  [+] DHT11  [Auto-detected on GPIO %d]\n", dhtPin);
            Serial.printf("      Temperature : %5.1f C  (%5.1f F)\n", currentTempC, currentTempF);
            Serial.printf("      Humidity    : %5.1f %%\n", currentHum);
            Serial.printf("      Heat Index  : %5.1f C\n", currentHI);
        } else {
            Serial.println(F("  [-] DHT11  [NOT CONNECTED / NOT DETECTED]"));
        }
        printLine('-');

        // Section: BH1750
        if (bh1750Found) {
            Serial.printf("  [+] BH1750 [Auto-detected at I2C 0x23 on SDA=%d, SCL=%d]\n", activeI2C_SDA, activeI2C_SCL);
            Serial.printf("      Light Level : %6.1f Lux\n", currentLux);
        } else {
            Serial.println(F("  [-] BH1750 [NOT CONNECTED / NOT DETECTED]"));
        }
        printLine('-');

        // Section: CCS811
        if (ccsFound) {
            Serial.printf("  [+] CCS811 [Auto-detected at I2C 0x5A on SDA=%d, SCL=%d]\n", activeI2C_SDA, activeI2C_SCL);
            Serial.printf("      eCO2        : %5u ppm\n", currentCO2);
            Serial.printf("      TVOC        : %5u ppb\n", currentTVOC);
        } else {
            Serial.println(F("  [-] CCS811 [NOT CONNECTED / NOT DETECTED]"));
        }
        printLine('-');

        // Send to ThingsBoard
        sendThingsBoardTelemetry();
        printLine('=');
    }

    // Periodic LCD screen rotation
    if (now - lastLcdSwitch >= LCD_PAGE_TIME) {
        lastLcdSwitch = now;
        lcdPage = (lcdPage + 1) % 2;
        updateLCD();
    }

    // Hot-plug auto-detection for sensors not yet connected
    static unsigned long lastAutoScan = 0;
    if (now - lastAutoScan >= 6000) {
        lastAutoScan = now;
        if (!lcdFound || !bh1750Found || !ccsFound) {
            scanAndInitI2C();
        }
        if (!dhtFound) {
            scanAndInitDHT();
        }
    }

    delay(20);
}
