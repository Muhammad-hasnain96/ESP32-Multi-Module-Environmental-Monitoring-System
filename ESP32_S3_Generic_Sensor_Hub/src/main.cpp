// =====================================================================
// ESP32-S3 — GENERIC SMART SENSOR HUB (True Auto-Detecting System)
// =====================================================================
// Plug-and-Play Hub Architecture:
//   - LCD 2004: Dedicated Wire on GPIO 17 (SDA) & GPIO 18 (SCL)
//   - I2C Sensors (BH1750, CCS811): Wire1 on GPIO 15 (SDA) & GPIO 16 (SCL)
//     (Supports reverse 16/15 auto-detection)
//   - DHT11 Auto-Discovery: Scans GPIOs (4, 5, 6, 7, 1, 2, 8, 9, 10)
//     with hot-unplug detection and automatic pin switching!
//   - Dynamic 2004 LCD: Rotating live multi-page display.
//   - ThingsBoard Cloud: Real-time telemetry streaming.
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
//  Hardware Pin Assignments
// =====================================================================
// 1. LCD 2004 on Dedicated Primary I2C (Wire)
#define LCD_SDA_PIN   17
#define LCD_SCL_PIN   18

// 2. Candidate pairs for Secondary I2C (Wire1 - Sensors: BH1750 / CCS811)
struct I2CPair {
    int sda;
    int scl;
    const char* label;
};

const I2CPair WIRE1_CANDIDATES[] = {
    { 15, 16, "SDA=15, SCL=16" },
    { 16, 15, "SDA=16, SCL=15" },
    { 8,  9,  "SDA=8,  SCL=9"  },
    { 1,  2,  "SDA=1,  SCL=2"  }
};
const int NUM_WIRE1_CANDIDATES = sizeof(WIRE1_CANDIDATES) / sizeof(WIRE1_CANDIDATES[0]);

// 3. Candidate single-wire GPIOs for DHT11 auto-scan
const int DHT_CANDIDATES[] = { 4, 5, 6, 7, 1, 2, 8, 9, 10 };
const int NUM_DHT_CANDIDATES = sizeof(DHT_CANDIDATES) / sizeof(DHT_CANDIDATES[0]);

// =====================================================================
//  Global Sensor State & Pointers
// =====================================================================
// LCD State (Wire)
LiquidCrystal_I2C* pLcd = nullptr;
bool lcdFound = false;
uint8_t lcdAddr = 0x27;

// BH1750 State (Wire1)
BH1750 lightMeter;
bool bh1750Found = false;
float currentLux = 0.0f;
int wire1SDA = -1;
int wire1SCL = -1;
int bh1750FailCount = 0;

// CCS811 State (Wire1)
Adafruit_CCS811 ccs;
bool ccsFound = false;
uint16_t currentCO2  = 400;
uint16_t currentTVOC = 0;

// DHT11 State (Single-Wire GPIO)
DHT* pDht = nullptr;
int  dhtPin = -1;
bool dhtFound = false;
int  dhtFailCount = 0;
float currentTempC = 0.0f;
float currentTempF = 0.0f;
float currentHum   = 0.0f;
float currentHI    = 0.0f;

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
//  LCD Initialization (Dedicated on Wire: SDA=17, SCL=18)
// =====================================================================
void initLCD() {
    if (lcdFound) return;

    const int LCD_PAIRS[][2] = {
        {17, 18}, {15, 18}, {18, 17}, {8, 9}, {1, 2}
    };
    const int NUM_LCD_PAIRS = sizeof(LCD_PAIRS) / sizeof(LCD_PAIRS[0]);

    for (int i = 0; i < NUM_LCD_PAIRS; i++) {
        int sda = LCD_PAIRS[i][0];
        int scl = LCD_PAIRS[i][1];
        if (dhtPin != -1 && (sda == dhtPin || scl == dhtPin)) continue;
        if (wire1SDA != -1 && (sda == wire1SDA || scl == wire1SCL)) continue;

        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        Wire.end();
        Wire.begin(sda, scl, 50000);
        Wire.setTimeOut(25);
        delay(20);

        byte foundAddr = 0;
        Wire.beginTransmission(0x27);
        if (Wire.endTransmission() == 0) foundAddr = 0x27;
        else {
            Wire.beginTransmission(0x3F);
            if (Wire.endTransmission() == 0) foundAddr = 0x3F;
        }

        if (foundAddr != 0) {
            lcdAddr = foundAddr;
            if (pLcd != nullptr) delete pLcd;
            pLcd = new LiquidCrystal_I2C(lcdAddr, 20, 4);
            pLcd->init();
            Wire.begin(sda, scl, 50000); // Re-assert pins
            pLcd->backlight();
            pLcd->clear();
            pLcd->setCursor(0, 0);
            pLcd->print(F("===================="));
            pLcd->setCursor(0, 1);
            pLcd->print(F(" ESP32-S3 GENERIC   "));
            pLcd->setCursor(0, 2);
            pLcd->print(F(" SMART SENSOR HUB   "));
            pLcd->setCursor(0, 3);
            pLcd->print(F("===================="));
            lcdFound = true;
            Serial.printf("  [+] 2004 LCD Initialized on Wire (SDA=%d, SCL=%d) at 0x%02X\n",
                          sda, scl, lcdAddr);
            return;
        }
    }
}

// =====================================================================
//  I2C Sensors Initialization (Wire1: BH1750 & CCS811)
// =====================================================================
void initI2CSensors() {
    if (bh1750Found && ccsFound) return;

    for (int p = 0; p < NUM_WIRE1_CANDIDATES; p++) {
        int sda = WIRE1_CANDIDATES[p].sda;
        int scl = WIRE1_CANDIDATES[p].scl;

        // Skip pins if currently used by DHT11 or LCD
        if (dhtPin != -1 && (sda == dhtPin || scl == dhtPin)) continue;
        if (sda == LCD_SDA_PIN || scl == LCD_SCL_PIN) continue;

        pinMode(sda, INPUT_PULLUP);
        pinMode(scl, INPUT_PULLUP);
        Wire1.end();
        Wire1.begin(sda, scl, 50000);
        Wire1.setTimeOut(25);
        delay(20);

        bool foundAny = false;

        // Check BH1750 (0x23, 0x5C)
        if (!bh1750Found) {
            byte bhAddr = 0;
            Wire1.beginTransmission(0x23);
            if (Wire1.endTransmission() == 0) bhAddr = 0x23;
            else {
                Wire1.beginTransmission(0x5C);
                if (Wire1.endTransmission() == 0) bhAddr = 0x5C;
            }
            if (bhAddr != 0) {
                if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, bhAddr, &Wire1)) {
                    bh1750Found = true;
                    wire1SDA = sda;
                    wire1SCL = scl;
                    foundAny = true;
                    Serial.printf("  [+] BH1750 Light Sensor Online on Wire1 (%s) at 0x%02X\n",
                                  WIRE1_CANDIDATES[p].label, bhAddr);
                }
            }
        }

        // Check CCS811 (0x5A)
        if (!ccsFound) {
            Wire1.beginTransmission(0x5A);
            if (Wire1.endTransmission() == 0) {
                if (ccs.begin(0x5A, &Wire1)) {
                    ccsFound = true;
                    wire1SDA = sda;
                    wire1SCL = scl;
                    foundAny = true;
                    Serial.printf("  [+] CCS811 CO2 Sensor Online on Wire1 (%s) at 0x5A\n",
                                  WIRE1_CANDIDATES[p].label);
                }
            }
        }

        if (foundAny) break; // Locked onto active sensor pair
    }
}

// =====================================================================
//  DHT11 Handshake Probe & Pin Scanner
// =====================================================================
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

void scanAndInitDHT() {
    for (int i = 0; i < NUM_DHT_CANDIDATES; i++) {
        int pin = DHT_CANDIDATES[i];

        // Skip pins used by LCD or Wire1
        if (pin == LCD_SDA_PIN || pin == LCD_SCL_PIN) continue;
        if (wire1SDA != -1 && (pin == wire1SDA || pin == wire1SCL)) continue;

        if (probeDHT11(pin)) {
            dhtPin = pin;
            dhtFound = true;
            dhtFailCount = 0;
            if (pDht != nullptr) delete pDht;
            pDht = new DHT(dhtPin, DHT11);
            pDht->begin();
            Serial.printf("  [+] DHT11 DETECTED & INITIALIZED on GPIO %d!\n", dhtPin);
            return;
        }
    }
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
    Serial.println(F("  Architecture: Dual I2C (Wire=LCD, Wire1=Sensors) + Auto DHT"));
    printLine('=');

    // 1. Initialize 2004 LCD on Dedicated Wire (SDA=17, SCL=18)
    initLCD();

    // 2. Initialize Sensors on Wire1 (SDA=15, SCL=16)
    initI2CSensors();

    // 3. Scan and initialize DHT11
    scanAndInitDHT();

    // 4. Connect to WiFi
    connectWiFi();

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

        // 2. Read BH1750 Light (if discovered on Wire1)
        if (bh1750Found) {
            float lux = lightMeter.readLightLevel();
            if (lux >= 0.0f) {
                currentLux = lux;
                bh1750FailCount = 0;
            } else {
                bh1750FailCount++;
                if (bh1750FailCount >= 2) {
                    Serial.printf("  [!] BH1750 UNPLUGGED from Wire1 (SDA=%d, SCL=%d)! Re-enabling auto scan...\n", wire1SDA, wire1SCL);
                    bh1750Found = false;
                    currentLux = 0.0f;
                    wire1SDA = -1;
                    wire1SCL = -1;
                    bh1750FailCount = 0;
                }
            }
        }

        // 3. Read CCS811 CO2 (if discovered on Wire1)
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
            Serial.printf("  [+] BH1750 [Auto-detected at I2C 0x23 on Wire1 (SDA=%d, SCL=%d)]\n", wire1SDA, wire1SCL);
            Serial.printf("      Light Level : %6.1f Lux\n", currentLux);
        } else {
            Serial.println(F("  [-] BH1750 [NOT CONNECTED / NOT DETECTED]"));
        }
        printLine('-');

        // Section: CCS811
        if (ccsFound) {
            Serial.printf("  [+] CCS811 [Auto-detected at I2C 0x5A on Wire1 (SDA=%d, SCL=%d)]\n", wire1SDA, wire1SCL);
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
        if (!lcdFound) {
            initLCD();
        }
        if (!bh1750Found || !ccsFound) {
            initI2CSensors();
        }
        if (!dhtFound) {
            scanAndInitDHT();
        }
    }

    delay(20);
}
