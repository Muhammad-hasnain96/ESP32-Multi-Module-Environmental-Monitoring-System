/**
 * ============================================================================
 * ESP32-S3 16-Channel Climate Hub with 2004 I2C LCD & 4 Physical Buttons
 * (Includes Full-Range Auto I2C Scanner & Web Status)
 * ============================================================================
 * 
 * Hardware:
 * - ESP32-S3 DevKitC-1
 * - Module 1 (8-ch Relay): GPIO 4, 5, 6, 7, 15, 16, 21, 47 (Relays 1-8: IN1-IN8)
 * - Module 2 (8-ch Relay): GPIO 1, 2, 9, 10, 11, 12, 13, 14 (Relays 9-16: IN1-IN8)
 * - DHT11 Sensors (x3 Multi-Zone / Average):
 *   Sensor 1 (T1): GPIO 8
 *   Sensor 2 (T2): GPIO 48
 *   Sensor 3 (T3): GPIO 42
 * - Water Flow Sensor:      GPIO 46 (Digital Pulse - Display Only)
 * - MQ-137 Ammonia Sensor:  AO -> GPIO 3 (ADC1_CH2), DO -> GPIO 45 (Alert - Display Only)
 * - 2004 I2C LCD Display:
 *   SDA -> GPIO 17
 *   SCL -> GPIO 18
 *   VCC -> 5V (VIN)
 *   GND -> GND
 * - 4 Push Buttons (Active-LOW, Leg 1 -> GPIO, Leg 2 -> GND):
 *   BTN 1 (MENU / SELECT): GPIO 38
 *   BTN 2 (UP / +):        GPIO 39
 *   BTN 3 (DOWN / -):      GPIO 40
 *   BTN 4 (BACK / SAVE):   GPIO 41
 * ============================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- HARDWARE PINS ---
const int NUM_RELAYS = 16;
const int RELAY_PINS[NUM_RELAYS] = {
    4, 5, 6, 7, 15, 16, 21, 47,    // Module 1 (8-ch Relay Board #1: IN1-IN8 -> Relays 1-8)
    1, 2, 9, 10,                   // Module 2 (8-ch Relay Board #2: IN1-IN4 -> Relays 9-12)
    11, 12, 13, 14                 // Module 2 (8-ch Relay Board #2: IN5-IN8 -> Relays 13-16)
};

// 3x DHT11 Sensors (T1 on GPIO 8, T2 on GPIO 48, T3 on GPIO 42)
#define DHTPIN1 8
#define DHTPIN2 48
#define DHTPIN3 42
#define DHTTYPE DHT11

DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);
DHT dht3(DHTPIN3, DHTTYPE);

// Water Flow Sensor (Digital Pulse Output on GPIO 46 - Display Only, Relays Unaffected)
#define PIN_FLOW_SENSOR 46
volatile unsigned long flowPulseCount = 0;
float flowRate = 0.0;       // Live Flow Rate in L/min
float totalLiters = 0.0;    // Accumulated volume in Liters
unsigned long totalPulses = 0;

void IRAM_ATTR flowPulseISR() {
    flowPulseCount++;
}

// MQ-137 Ammonia (NH3) Sensor (Display Only, Relays Unaffected)
// AO on GPIO 3 (ADC1_CH2), DO on GPIO 45
#define MQ137_AO 3
#define MQ137_DO 45
float currentNH3 = 0.0f;
float currentMQ137V = 0.0f;
bool  currentNH3Alert = false;
float mq137Baseline = 0.0f;
bool  mq137Warmed = false;
unsigned long mq137WarmStart = 0;
const unsigned long MQ137_WARMUP = 30000UL;

int avgADC(int pin, int n = 10) {
    long sum = 0;
    for (int i = 0; i < n; i++) { sum += analogRead(pin); delay(2); }
    return (int)(sum / n);
}

float readMQ137(float &V) {
    int raw = avgADC(MQ137_AO);
    V = (raw / 4095.0f) * 3.3f;
    if (!mq137Warmed) {
        if (millis() - mq137WarmStart >= MQ137_WARMUP) {
            mq137Baseline = max(V, 0.10f);
            mq137Warmed = true;
        } else {
            mq137Baseline = (mq137Baseline == 0) ? V
                          : mq137Baseline * 0.95f + V * 0.05f;
            return 0.0f;
        }
    }
    if (V <= mq137Baseline) return 0.0f;
    float r = (V - mq137Baseline) / (3.3f - mq137Baseline);
    return constrain(r * 500.0f, 0.0f, 500.0f);
}

// I2C LCD Pins (Proven Default I2C Pins: SDA=17, SCL=18)
#define I2C_SDA 17
#define I2C_SCL 18

LiquidCrystal_I2C* pLcd = nullptr;
bool lcdAvailable = false;
uint8_t detectedLcdAddr = 0;
String lcdStatusStr = "Scanning I2C...";

// 4 Push Buttons
#define PIN_BTN_MENU 38
#define PIN_BTN_UP   39
#define PIN_BTN_DOWN 40
#define PIN_BTN_BACK 41

// Active-LOW relay logic
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// Relay Config
struct RelayConfig {
    char name[16];   // Custom name (e.g. "Exhaust Fan 1")
    int sensor;      // 0: None/Manual, 1: Avg Temp, 2: Avg Hum, 3: DHT1 Temp, 4: DHT1 Hum, 5: DHT2 Temp, 6: DHT2 Hum, 7: DHT3 Temp, 8: DHT3 Hum, 9: NH3, 10: Flow, 11: Pure Timer
    int mode;        // 0: Threshold Only, 1: Timer + Threshold Override, 2: Pure Timer, 3: Manual
    float minVal;    // Min threshold
    float maxVal;    // Max threshold
    int action;      // 0: Outside Range ON (<=Min or >=Max), 1: Inside Range ON, 2: Above Max ON, 3: Below Min ON
    int timerOnSec;  // Cyclic Run time in seconds
    int timerOffSec; // Cyclic Pause time in seconds
};

RelayConfig configs[NUM_RELAYS];
bool relayStates[NUM_RELAYS] = { false };
String relayReason[NUM_RELAYS];

// Runtime tracking for cyclic timers
unsigned long relayTimerLastSwitch[NUM_RELAYS] = { 0 };
bool          relayTimerInOnPhase[NUM_RELAYS]  = { false };

// Sensor readings (3x DHT11 & Averages)
float temp1 = 0.0, temp2 = 0.0, temp3 = 0.0;
float hum1  = 0.0, hum2  = 0.0, hum3  = 0.0;
bool valid1 = false, valid2 = false, valid3 = false;
float avgTemp = 0.0;
float avgHum  = 0.0;
float currentTemp = 0.0;
float currentHum  = 0.0;
bool sensorValid = false;

// Sensor Calibration Config & Offsets
struct CalibrationConfig {
    float t1Offset;    // °C offset
    float h1Offset;    // % offset
    float t2Offset;    // °C offset
    float h2Offset;    // % offset
    float t3Offset;    // °C offset
    float h3Offset;    // % offset
    float nh3Factor;   // Multiplier factor (default 1.0)
    float flowFactor;  // Pulses per sec per L/min (default 7.5)
};
CalibrationConfig calib = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 7.5f };

// Raw sensor readings before calibration
float rawTemp1 = 0.0, rawTemp2 = 0.0, rawTemp3 = 0.0;
float rawHum1  = 0.0, rawHum2  = 0.0, rawHum3  = 0.0;
float rawNH3   = 0.0;

// Helper: Sensor names and units
const char* getSensorName(int s) {
    switch (s) {
        case 1: return "Avg Temp";
        case 2: return "Avg Hum";
        case 3: return "Sens1 T";
        case 4: return "Sens1 H";
        case 5: return "Sens2 T";
        case 6: return "Sens2 H";
        case 7: return "Sens3 T";
        case 8: return "Sens3 H";
        case 9: return "NH3 Gas";
        case 10: return "Flow";
        case 11: return "PureTmr";
        default: return "Manual";
    }
}

const char* getSensorUnit(int s) {
    if (s == 1 || s == 3 || s == 5 || s == 7) return "C";
    if (s == 2 || s == 4 || s == 6 || s == 8) return "%";
    if (s == 9) return "p"; // ppm
    if (s == 10) return "L"; // L/min
    return "";
}

bool getSensorReading(int s, float &val, bool &isValid) {
    isValid = false;
    val = 0.0f;
    switch (s) {
        case 1: val = avgTemp; isValid = sensorValid; break;
        case 2: val = avgHum; isValid = sensorValid; break;
        case 3: val = temp1; isValid = valid1; break;
        case 4: val = hum1; isValid = valid1; break;
        case 5: val = temp2; isValid = valid2; break;
        case 6: val = hum2; isValid = valid2; break;
        case 7: val = temp3; isValid = valid3; break;
        case 8: val = hum3; isValid = valid3; break;
        case 9: val = currentNH3; isValid = (millis() - mq137WarmStart >= MQ137_WARMUP); break;
        case 10: val = flowRate; isValid = true; break;
        default: isValid = false; break;
    }
    return isValid;
}

// Network & Web
const char* AP_SSID = "ESP32-Relay-Controller";
const char* AP_PASS = "12345678";
WebServer server(80);
Preferences prefWifi;
Preferences prefRelay;
String saved_ssid = "";
String saved_pass = "";
String sta_status_str = "Disconnected";

// LCD Menu State Machine
enum MenuState {
    STATE_HOME = 0,
    STATE_MAIN_MENU,       // 1. Temp, 2. Hum, 3. Relays
    STATE_GLOBAL_TARGET,   // Choose Min vs Max for Global Temp or Hum
    STATE_GLOBAL_EDIT,     // Edit Global Min or Max
    STATE_SELECT_RELAY,    // Scroll Relays 1-16
    STATE_RELAY_MENU,      // Mode, Min, Max for selected Relay
    STATE_EDIT_VALUE       // Edit Min or Max for selected Relay
};

MenuState currentMenu = STATE_HOME;
int mainMenuIndex = 0;         // 0: Temp, 1: Hum, 2: Relays
int globalTarget = 0;          // 0: Min, 1: Max
float globalTempMin = 28.0;
float globalTempMax = 32.0;
float globalHumMin = 40.0;
float globalHumMax = 75.0;
float globalNH3Min = 10.0;
float globalNH3Max = 25.0;

int selectedRelay = 0;         // 0 to 15
int relayMenuIndex = 0;        // 0: Mode, 1: Min, 2: Max
int selectedTarget = 0;        // 0: Min, 1: Max
float editingVal = 0.0;
unsigned long lastMenuActivity = 0;

// Button Debounce & Auto-Repeat
struct Button {
    int pin;
    const char* name;
    bool lastState;
    unsigned long lastDebounceTime;
    bool pressed;
    unsigned long pressStartTime;
    unsigned long lastRepeatTime;
};

Button btnMenu = { PIN_BTN_MENU, "MENU", HIGH, 0, false, 0, 0 };
Button btnUp   = { PIN_BTN_UP,   "UP",   HIGH, 0, false, 0, 0 };
Button btnDown = { PIN_BTN_DOWN, "DOWN", HIGH, 0, false, 0, 0 };
Button btnBack = { PIN_BTN_BACK, "BACK", HIGH, 0, false, 0, 0 };

// Function Declarations
void loadRelayConfigs();
void saveRelayConfig(int idx);
void setRelay(int index, bool state, String reason);
void turnAll(bool state);
void evaluateAutoRules();
void evaluateSingleRelay(int i, bool isConfigUpdate = false);
void readDHTSensor();
void initLCD();
void updateButtons();
void updateLCD();
void drawHomeScreen();
void drawMainMenuScreen();
void drawGlobalTargetScreen();
void drawGlobalEditScreen();
void drawSelectRelayScreen();
void drawRelayMenuScreen();
void drawEditValueScreen();

void handleRoot();
void handleRelayToggle();
void handleAllRelays();
void handleAllAuto();
void handleGetStatus();
void handleSaveRelayConfig();
void handleBatchRange();
void handleScanNetworks();
void handleSaveWifi();
void handleResetWifi();
void setAllAuto();
void saveCalibration();
void handleSaveCalibration();

void setup() {
    Serial.begin(115200);

    // Relays Initialization
    for (int i = 0; i < NUM_RELAYS; i++) {
        digitalWrite(RELAY_PINS[i], RELAY_OFF);
        pinMode(RELAY_PINS[i], OUTPUT);
        relayStates[i] = false;
        relayReason[i] = "Safe Startup OFF";
    }

    // Buttons Initialization (Internal Pullup)
    pinMode(PIN_BTN_MENU, INPUT_PULLUP);
    pinMode(PIN_BTN_UP,   INPUT_PULLUP);
    pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
    pinMode(PIN_BTN_BACK, INPUT_PULLUP);

    // Allow LCD backpack power to stabilize
    delay(250);

    // Initialize I2C & LCD with Auto Scanner
    initLCD();
    if (!lcdAvailable) {
        delay(150);
        initLCD();
    }

    // Start 3x DHT11 Sensors (GPIO 8, 3, 42)
    pinMode(DHTPIN1, INPUT_PULLUP);
    pinMode(DHTPIN2, INPUT_PULLUP);
    pinMode(DHTPIN3, INPUT_PULLUP);
    dht1.begin();
    dht2.begin();
    dht3.begin();

    // Start Water Flow Sensor on GPIO 46
    pinMode(PIN_FLOW_SENSOR, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_FLOW_SENSOR), flowPulseISR, RISING);

    // Start MQ-137 Ammonia Gas Sensor (AO on GPIO 3, DO on GPIO 45)
    pinMode(MQ137_AO, INPUT);
    pinMode(MQ137_DO, INPUT);
    analogReadResolution(12);
    mq137WarmStart = millis();

    // Load Relay Thresholds
    loadRelayConfigs();

    // --- SEQUENTIAL STARTUP SEQUENCE ("BAARI BAARI" ALL RELAYS ON) ---
    Serial.println("⚡ Running Sequential Relay Startup Sequence across all 16 relays...");
    if (lcdAvailable && pLcd != nullptr) {
        pLcd->clear();
        pLcd->setCursor(0, 0); pLcd->print("= SEQUENTIAL START =");
        pLcd->setCursor(0, 1); pLcd->print("16 Relays Turning ON");
    }
    for (int i = 0; i < NUM_RELAYS; i++) {
        digitalWrite(RELAY_PINS[i], RELAY_ON);
        relayStates[i] = true;
        relayReason[i] = "Startup Sequence ON";
        if (lcdAvailable && pLcd != nullptr) {
            pLcd->setCursor(0, 2);
            char b[21]; snprintf(b, sizeof(b), "Relay %02d ON [GP%02d]  ", i + 1, RELAY_PINS[i]);
            pLcd->print(b);
        }
        Serial.printf("⚡ Startup: Relay %02d (GPIO %02d) -> ON\n", i + 1, RELAY_PINS[i]);
        delay(250); // 250ms spacing between each relay turning ON
    }
    if (lcdAvailable && pLcd != nullptr) {
        pLcd->setCursor(0, 2); pLcd->print("All 16 Relays ON    ");
        pLcd->setCursor(0, 3); pLcd->print("Applying Sensor Mode");
    }
    Serial.println("✅ All 16 relays ON. Holding for 1.2s before applying sensor rules...");
    delay(1200);

    readDHTSensor();
    evaluateAutoRules();
    Serial.println("\n\n========================================================");
    Serial.println("  ⚡ ESP32-S3 16-RELAY SMART CLIMATE HUB (LCD + BUTTONS)");
    Serial.println("========================================================");
    for (int i = 0; i < NUM_RELAYS; i++) {
        Serial.printf("📌 R%02d [%s]: Mode=%d, Sensor=%d(%s), Min=%.1f, Max=%.1f, Act=%d -> %s | %s\n",
            i + 1, configs[i].name, configs[i].mode, configs[i].sensor, getSensorName(configs[i].sensor),
            configs[i].minVal, configs[i].maxVal, configs[i].action,
            relayStates[i] ? "ON" : "OFF", relayReason[i].c_str());
    }

    // WiFi Setup
    prefWifi.begin("wifi-config", false);
    saved_ssid = prefWifi.getString("ssid", "DS-Guest");
    saved_pass = prefWifi.getString("pass", "Shahid786");

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);

    if (saved_ssid.length() > 0) {
        sta_status_str = "Connecting...";
        Serial.printf("📡 Connecting to WiFi: %s ...\n", saved_ssid.c_str());

        // Configure Static IP 192.168.0.22 for Router
        IPAddress local_IP(192, 168, 0, 22);
        IPAddress gateway(192, 168, 0, 1);
        IPAddress subnet(255, 255, 255, 0);
        IPAddress primaryDNS(192, 168, 0, 1);
        IPAddress secondaryDNS(8, 8, 8, 8);
        WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);

        WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
        int count = 0;
        while (WiFi.status() != WL_CONNECTED && count < 20) {
            delay(350);
            count++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            sta_status_str = "Connected";
            Serial.printf("✅ Connected to '%s'! IP Address: %s\n", saved_ssid.c_str(), WiFi.localIP().toString().c_str());
            if (lcdAvailable && pLcd != nullptr) {
                pLcd->clear();
                pLcd->setCursor(0, 0); pLcd->print("= WIFI CONNECTED! = ");
                pLcd->setCursor(0, 1);
                char bSsid[21]; snprintf(bSsid, sizeof(bSsid), "SSID:%-15.15s", saved_ssid.c_str());
                pLcd->print(bSsid);
                pLcd->setCursor(0, 2);
                char bIp[21]; snprintf(bIp, sizeof(bIp), "IP: %-16.16s", WiFi.localIP().toString().c_str());
                pLcd->print(bIp);
                pLcd->setCursor(0, 3); pLcd->print("PORT: 80 (HTTP)     ");
                delay(1500);
                pLcd->clear();
            }
        } else {
            sta_status_str = "Not Connected";
            Serial.println("⚠️ Could not connect to Station WiFi, AP mode remains active.");
        }
    }

    if (MDNS.begin("esp32relay")) {
        MDNS.addService("http", "tcp", 80);
    }

    // Web Routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/toggle", HTTP_GET, handleRelayToggle);
    server.on("/all", HTTP_GET, handleAllRelays);
    server.on("/allauto", HTTP_GET, handleAllAuto);
    server.on("/status", HTTP_GET, handleGetStatus);
    server.on("/saveconfig", HTTP_POST, handleSaveRelayConfig);
    server.on("/batchrange", HTTP_POST, handleBatchRange);
    server.on("/scan", HTTP_GET, handleScanNetworks);
    server.on("/savewifi", HTTP_POST, handleSaveWifi);
    server.on("/resetwifi", HTTP_POST, handleResetWifi);
    server.on("/savecalib", HTTP_POST, handleSaveCalibration);

    server.begin();
    Serial.println("🚀 Web Server started successfully!");
}

void loop() {
    server.handleClient();

    // Non-blocking check for LCD every 3 seconds if not yet connected
    static unsigned long lastI2CRetry = 0;
    if (!lcdAvailable && millis() - lastI2CRetry > 3000) {
        lastI2CRetry = millis();
        initLCD();
    }

    // Calculate Water Flow Rate (L/min) & Total Liters every 1000ms
    static unsigned long lastFlowCalc = 0;
    static unsigned long prevPulseCount = 0;
    if (millis() - lastFlowCalc >= 1000) {
        unsigned long now = millis();
        unsigned long dt = now - lastFlowCalc;
        lastFlowCalc = now;

        noInterrupts();
        unsigned long currentPulses = flowPulseCount;
        interrupts();

        unsigned long deltaPulses = (currentPulses >= prevPulseCount) ? (currentPulses - prevPulseCount) : currentPulses;
        prevPulseCount = currentPulses;
        totalPulses = currentPulses;

        // Flow sensor calibration using dynamic calib.flowFactor (default 7.5 pulses per sec = 1.0 L/min)
        float kFactor = (calib.flowFactor > 0.1f) ? calib.flowFactor : 7.5f;
        flowRate = ((float)deltaPulses * 1000.0f / (float)dt) / kFactor;
        if (flowRate < 0.05f) flowRate = 0.0f; // Noise suppression
        totalLiters += (flowRate / 60.0f) * ((float)dt / 1000.0f);
    }

    // Read Sensors every 2 seconds
    static unsigned long lastSensor = 0;
    if (millis() - lastSensor > 2000) {
        lastSensor = millis();
        readDHTSensor();

        // Read MQ-137 Ammonia Sensor
        float mqV = 0.0f;
        rawNH3 = readMQ137(mqV);
        float nh3Mult = (calib.nh3Factor > 0.01f) ? calib.nh3Factor : 1.0f;
        currentNH3 = rawNH3 * nh3Mult;
        currentMQ137V = mqV;
        currentNH3Alert = (digitalRead(MQ137_DO) == LOW); // LOW = Gas alert triggered
    }

    // Evaluate Relay Rules & Cyclic Timers every 500ms
    static unsigned long lastRuleEval = 0;
    if (millis() - lastRuleEval >= 500) {
        lastRuleEval = millis();
        evaluateAutoRules();
    }

    static unsigned long lastRelayReport = 0;
    if (millis() - lastRelayReport > 5000) {
        lastRelayReport = millis();
        Serial.printf("⚡ RELAY STATUS [AvgTemp: %.1fC, AvgHum: %.1f%%, Sensor: %s]:\n", avgTemp, avgHum, sensorValid ? "OK" : "NC");
        for (int i = 0; i < NUM_RELAYS; i++) {
            Serial.printf("  R%02d [%s]: Mode=%d, Sens=%d, St=%s | %s\n",
                i + 1, configs[i].name, configs[i].mode, configs[i].sensor,
                relayStates[i] ? "ON" : "OFF", relayReason[i].c_str());
        }
    }

    // Read Buttons
    updateButtons();

    // Refresh LCD every 400ms
    static unsigned long lastLcdUpdate = 0;
    if (millis() - lastLcdUpdate > 400) {
        lastLcdUpdate = millis();
        updateLCD();
    }

    // Menu Inactivity Timeout (Return to Home after 20s)
    if (currentMenu != STATE_HOME && millis() - lastMenuActivity > 20000) {
        currentMenu = STATE_HOME;
        if (lcdAvailable && pLcd != nullptr) pLcd->clear();
        updateLCD();
    }
}

// --- PROVEN BATTLE-TESTED LCD INITIALIZER ---

void initLCD() {
    Wire.begin(I2C_SDA, I2C_SCL, 100000);

    byte lcdAddr = 0;
    // Fast check 0x27 and 0x3F first
    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() == 0) {
        lcdAddr = 0x27;
    } else {
        Wire.beginTransmission(0x3F);
        if (Wire.endTransmission() == 0) lcdAddr = 0x3F;
    }

    // Full scan across all standard I2C addresses (0x08 to 0x77) if not found
    if (lcdAddr == 0) {
        for (byte a = 0x08; a <= 0x77; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) {
                lcdAddr = a;
                break;
            }
        }
    }

    if (lcdAddr != 0) {
        detectedLcdAddr = lcdAddr;
        Serial.printf("🎯 LCD ONLINE at 0x%02X [OK]\n", detectedLcdAddr);
        lcdStatusStr = "Connected at 0x" + String(detectedLcdAddr, HEX) + " (SDA:17, SCL:18)";

        if (pLcd != nullptr) delete pLcd;
        pLcd = new LiquidCrystal_I2C(detectedLcdAddr, 20, 4);
        pLcd->init();
        pLcd->backlight();
        pLcd->clear();

        pLcd->setCursor(0, 0); pLcd->print("====================");
        pLcd->setCursor(0, 1); pLcd->print("  16-CH CLIMATE HUB ");
        pLcd->setCursor(0, 2); pLcd->print("  ESP32-S3 + RELAYS ");
        pLcd->setCursor(0, 3); pLcd->print("====================");

        lcdAvailable = true;
        delay(800);
        pLcd->clear();
        updateLCD(); // Immediately render active screen!
    } else {
        lcdStatusStr = "Not Detected (Check SDA 17, SCL 18)";
        lcdAvailable = false;
        Serial.println("❌ LCD NOT DETECTED on SDA 17, SCL 18");
    }
}

// --- BUTTONS LOGIC & NAVIGATION ---

bool checkButton(Button &btn) {
    int reading = digitalRead(btn.pin);
    bool triggered = false;

    // Check if state changed
    if (reading != btn.lastState) {
        btn.lastDebounceTime = millis();
        btn.lastState = reading;
    }

    if ((millis() - btn.lastDebounceTime) > 25) {
        if (reading == LOW) {
            if (!btn.pressed) {
                btn.pressed = true;
                btn.pressStartTime = millis();
                btn.lastRepeatTime = millis();
                triggered = true;
                Serial.printf("🔘 Button [%s] (GPIO %d) -> Menu: %d\n", btn.name, btn.pin, currentMenu);
            } else if ((millis() - btn.pressStartTime > 350) && (millis() - btn.lastRepeatTime > 120)) {
                btn.lastRepeatTime = millis();
                triggered = true;
            }
        } else {
            btn.pressed = false;
        }
    }

    return triggered;
}

void updateButtons() {
    bool bMenu = checkButton(btnMenu);
    bool bUp   = checkButton(btnUp);
    bool bDown = checkButton(btnDown);
    bool bBack = checkButton(btnBack);

    if (bMenu || bUp || bDown || bBack) {
        lastMenuActivity = millis();
    } else {
        return;
    }

    switch (currentMenu) {
        case STATE_HOME:
            if (bMenu || bUp || bDown) {
                currentMenu = STATE_MAIN_MENU;
                mainMenuIndex = 0;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            }
            break;

        case STATE_MAIN_MENU:
            if (bUp) {
                mainMenuIndex = (mainMenuIndex - 1 + 3) % 3;
                updateLCD();
            } else if (bDown) {
                mainMenuIndex = (mainMenuIndex + 1) % 3;
                updateLCD();
            } else if (bBack) {
                currentMenu = STATE_HOME;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            } else if (bMenu) {
                if (mainMenuIndex == 0 || mainMenuIndex == 1) {
                    currentMenu = STATE_GLOBAL_TARGET;
                    globalTarget = 0;
                } else {
                    currentMenu = STATE_SELECT_RELAY;
                    selectedRelay = 0;
                }
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            }
            break;

        case STATE_GLOBAL_TARGET:
            if (bUp || bDown) {
                globalTarget = 1 - globalTarget;
                updateLCD();
            } else if (bBack) {
                currentMenu = STATE_MAIN_MENU;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            } else if (bMenu) {
                currentMenu = STATE_GLOBAL_EDIT;
                if (mainMenuIndex == 0) {
                    editingVal = (globalTarget == 0) ? globalTempMin : globalTempMax;
                } else {
                    editingVal = (globalTarget == 0) ? globalHumMin : globalHumMax;
                }
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            }
            break;

        case STATE_GLOBAL_EDIT:
            if (bUp) {
                if (mainMenuIndex == 0) {
                    editingVal += 0.5;
                    if (editingVal > 60.0) editingVal = 60.0;
                } else {
                    editingVal += 1.0;
                    if (editingVal > 99.0) editingVal = 99.0;
                }
                updateLCD();
            } else if (bDown) {
                if (mainMenuIndex == 0) {
                    editingVal -= 0.5;
                    if (editingVal < 10.0) editingVal = 10.0;
                } else {
                    editingVal -= 1.0;
                    if (editingVal < 10.0) editingVal = 10.0;
                }
                updateLCD();
            } else if (bBack) {
                // Cancel without saving
                if (lcdAvailable && pLcd != nullptr) {
                    pLcd->clear();
                    pLcd->setCursor(2, 1);
                    pLcd->print("** CANCELLED **");
                    delay(400);
                    pLcd->clear();
                }
                currentMenu = STATE_GLOBAL_TARGET;
                updateLCD();
            } else if (bMenu) {
                // Save to ALL 16 Relays!
                if (mainMenuIndex == 0) {
                    // Temperature
                    if (globalTarget == 0) globalTempMin = editingVal;
                    else globalTempMax = editingVal;

                    if (globalTempMin > globalTempMax) {
                        float t = globalTempMin; globalTempMin = globalTempMax; globalTempMax = t;
                    }
                    prefRelay.putFloat("gTMin", globalTempMin);
                    prefRelay.putFloat("gTMax", globalTempMax);

                    for (int i = 0; i < NUM_RELAYS; i++) {
                        configs[i].mode = 1; // Auto Temp
                        configs[i].action = 0; // Outside Range ON
                        configs[i].minVal = globalTempMin;
                        configs[i].maxVal = globalTempMax;
                        saveRelayConfig(i);
                    }
                    readDHTSensor();
                    for (int i = 0; i < NUM_RELAYS; i++) {
                        evaluateSingleRelay(i, true);
                    }
                    Serial.printf("⚡ LCD Global Temp Applied to ALL: Min=%.1fC, Max=%.1fC\n", globalTempMin, globalTempMax);
                } else {
                    // Humidity
                    if (globalTarget == 0) globalHumMin = editingVal;
                    else globalHumMax = editingVal;

                    if (globalHumMin > globalHumMax) {
                        float t = globalHumMin; globalHumMin = globalHumMax; globalHumMax = t;
                    }
                    prefRelay.putFloat("gHMin", globalHumMin);
                    prefRelay.putFloat("gHMax", globalHumMax);

                    for (int i = 0; i < NUM_RELAYS; i++) {
                        configs[i].mode = 2; // Auto Hum
                        configs[i].action = 0; // Outside Range ON
                        configs[i].minVal = globalHumMin;
                        configs[i].maxVal = globalHumMax;
                        saveRelayConfig(i);
                    }
                    readDHTSensor();
                    for (int i = 0; i < NUM_RELAYS; i++) {
                        evaluateSingleRelay(i, true);
                    }
                    Serial.printf("⚡ LCD Global Hum Applied to ALL: Min=%.1f%%, Max=%.1f%%\n", globalHumMin, globalHumMax);
                }

                if (lcdAvailable && pLcd != nullptr) {
                    pLcd->clear();
                    pLcd->setCursor(3, 1);
                    pLcd->print("*** SAVED! ***");
                    pLcd->setCursor(1, 2);
                    pLcd->print("Applied to ALL 16!");
                    delay(800);
                    pLcd->clear();
                }
                currentMenu = STATE_GLOBAL_TARGET;
                updateLCD();
            }
            break;

        case STATE_SELECT_RELAY:
            if (bUp) {
                selectedRelay = (selectedRelay + 1) % NUM_RELAYS;
                updateLCD();
            } else if (bDown) {
                selectedRelay = (selectedRelay - 1 + NUM_RELAYS) % NUM_RELAYS;
                updateLCD();
            } else if (bBack) {
                currentMenu = STATE_MAIN_MENU;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            } else if (bMenu) {
                currentMenu = STATE_RELAY_MENU;
                relayMenuIndex = 0;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            }
            break;

        case STATE_RELAY_MENU:
            if (bUp) {
                relayMenuIndex = (relayMenuIndex - 1 + 6) % 6;
                updateLCD();
            } else if (bDown) {
                relayMenuIndex = (relayMenuIndex + 1) % 6;
                updateLCD();
            } else if (bBack) {
                currentMenu = STATE_SELECT_RELAY;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            } else if (bMenu) {
                if (relayMenuIndex == 0) {
                    // Cycle Sensor: 1(AvgT) -> 2(AvgH) -> 3(DHT1 T) -> ... -> 11(PureTmr) -> 0(Manual) -> 1
                    configs[selectedRelay].sensor = (configs[selectedRelay].sensor + 1) % 12;
                    relayTimerLastSwitch[selectedRelay] = 0;
                    saveRelayConfig(selectedRelay);
                    readDHTSensor();
                    evaluateSingleRelay(selectedRelay, true);

                    if (lcdAvailable && pLcd != nullptr) {
                        pLcd->clear();
                        pLcd->setCursor(2, 1);
                        pLcd->print("Sensor Assigned!");
                        pLcd->setCursor(2, 2);
                        char sbuf[21]; snprintf(sbuf, sizeof(sbuf), "-> %s", getSensorName(configs[selectedRelay].sensor));
                        pLcd->print(sbuf);
                        delay(600);
                        pLcd->clear();
                    }
                    updateLCD();
                } else if (relayMenuIndex == 1) {
                    // Cycle Mode: 0(Thresh Only) -> 1(Timer+Override) -> 2(Pure Timer) -> 3(Manual) -> 0
                    configs[selectedRelay].mode = (configs[selectedRelay].mode + 1) % 4;
                    relayTimerLastSwitch[selectedRelay] = 0;
                    saveRelayConfig(selectedRelay);
                    readDHTSensor();
                    evaluateSingleRelay(selectedRelay, true);

                    if (lcdAvailable && pLcd != nullptr) {
                        pLcd->clear();
                        pLcd->setCursor(3, 1);
                        pLcd->print("Mode Updated!");
                        pLcd->setCursor(1, 2);
                        if (configs[selectedRelay].mode == 0) pLcd->print("-> Threshold Only");
                        else if (configs[selectedRelay].mode == 1) pLcd->print("-> Timer+Override");
                        else if (configs[selectedRelay].mode == 2) pLcd->print("-> Pure Cyclic Tmr");
                        else pLcd->print("-> Manual Control");
                        delay(600);
                        pLcd->clear();
                    }
                    updateLCD();
                } else {
                    currentMenu = STATE_EDIT_VALUE;
                    if (relayMenuIndex == 2) {
                        selectedTarget = 0; // Min
                        editingVal = configs[selectedRelay].minVal;
                    } else if (relayMenuIndex == 3) {
                        selectedTarget = 1; // Max
                        editingVal = configs[selectedRelay].maxVal;
                    } else if (relayMenuIndex == 4) {
                        selectedTarget = 2; // T-On
                        editingVal = configs[selectedRelay].timerOnSec;
                    } else if (relayMenuIndex == 5) {
                        selectedTarget = 3; // T-Off
                        editingVal = configs[selectedRelay].timerOffSec;
                    }
                    if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                    updateLCD();
                }
            }
            break;

        case STATE_EDIT_VALUE:
            if (bUp) {
                if (selectedTarget == 2 || selectedTarget == 3) { // Timer On or Off
                    editingVal += (editingVal >= 60 ? 10 : 5);
                    if (editingVal > 3600) editingVal = 3600;
                } else if (configs[selectedRelay].sensor == 9) { // NH3
                    editingVal += 1.0;
                    if (editingVal > 200.0) editingVal = 200.0;
                } else if (configs[selectedRelay].sensor == 10) { // Flow
                    editingVal += 0.5;
                    if (editingVal > 50.0) editingVal = 50.0;
                } else if (configs[selectedRelay].sensor == 2 || configs[selectedRelay].sensor == 4 || 
                           configs[selectedRelay].sensor == 6 || configs[selectedRelay].sensor == 8) { // Humidity
                    editingVal += 1.0;
                    if (editingVal > 99.0) editingVal = 99.0;
                } else { // Temperature
                    editingVal += 0.5;
                    if (editingVal > 60.0) editingVal = 60.0;
                }
                updateLCD();
            } else if (bDown) {
                if (selectedTarget == 2 || selectedTarget == 3) { // Timer On or Off
                    editingVal -= (editingVal > 60 ? 10 : 5);
                    if (editingVal < 5) editingVal = 5;
                } else if (configs[selectedRelay].sensor == 9) { // NH3
                    editingVal -= 1.0;
                    if (editingVal < 0.0) editingVal = 0.0;
                } else if (configs[selectedRelay].sensor == 10) { // Flow
                    editingVal -= 0.5;
                    if (editingVal < 0.0) editingVal = 0.0;
                } else if (configs[selectedRelay].sensor == 2 || configs[selectedRelay].sensor == 4 || 
                           configs[selectedRelay].sensor == 6 || configs[selectedRelay].sensor == 8) { // Humidity
                    editingVal -= 1.0;
                    if (editingVal < 10.0) editingVal = 10.0;
                } else { // Temperature
                    editingVal -= 0.5;
                    if (editingVal < 10.0) editingVal = 10.0;
                }
                updateLCD();
            } else if (bBack) {
                // Cancel without saving
                if (lcdAvailable && pLcd != nullptr) {
                    pLcd->clear();
                    pLcd->setCursor(2, 1);
                    pLcd->print("** CANCELLED **");
                    delay(400);
                    pLcd->clear();
                }
                currentMenu = STATE_RELAY_MENU;
                updateLCD();
            } else if (bMenu) {
                // Save for selected relay
                if (selectedTarget == 0) {
                    configs[selectedRelay].minVal = editingVal;
                } else if (selectedTarget == 1) {
                    configs[selectedRelay].maxVal = editingVal;
                } else if (selectedTarget == 2) {
                    configs[selectedRelay].timerOnSec = (int)editingVal;
                    relayTimerLastSwitch[selectedRelay] = 0;
                } else if (selectedTarget == 3) {
                    configs[selectedRelay].timerOffSec = (int)editingVal;
                    relayTimerLastSwitch[selectedRelay] = 0;
                }

                if (configs[selectedRelay].minVal > configs[selectedRelay].maxVal) {
                    float t = configs[selectedRelay].minVal;
                    configs[selectedRelay].minVal = configs[selectedRelay].maxVal;
                    configs[selectedRelay].maxVal = t;
                }
                saveRelayConfig(selectedRelay);
                readDHTSensor();
                evaluateSingleRelay(selectedRelay, true);
                Serial.printf("⚡ LCD Saved Relay %d [%s]: Sensor=%d, Mode=%d, Min=%.1f, Max=%.1f, Ton=%d, Toff=%d -> State: %s (%s)\n",
                    selectedRelay + 1, configs[selectedRelay].name, configs[selectedRelay].sensor, configs[selectedRelay].mode,
                    configs[selectedRelay].minVal, configs[selectedRelay].maxVal,
                    configs[selectedRelay].timerOnSec, configs[selectedRelay].timerOffSec,
                    relayStates[selectedRelay] ? "ON" : "OFF", relayReason[selectedRelay].c_str());

                if (lcdAvailable && pLcd != nullptr) {
                    pLcd->clear();
                    pLcd->setCursor(3, 1);
                    pLcd->print("*** SAVED! ***");
                    pLcd->setCursor(1, 2);
                    char sb[21];
                    if (selectedTarget == 2) {
                        snprintf(sb, sizeof(sb), "Run Time: %d sec", configs[selectedRelay].timerOnSec);
                    } else if (selectedTarget == 3) {
                        snprintf(sb, sizeof(sb), "Pause: %d sec", configs[selectedRelay].timerOffSec);
                    } else {
                        const char* u = getSensorUnit(configs[selectedRelay].sensor);
                        snprintf(sb, sizeof(sb), "Mn:%3.0f%s  Mx:%3.0f%s", configs[selectedRelay].minVal, u, configs[selectedRelay].maxVal, u);
                    }
                    pLcd->print(sb);
                    delay(700);
                    pLcd->clear();
                }
                currentMenu = STATE_RELAY_MENU;
                updateLCD();
            }
            break;
    }
}

// --- LCD RENDERING ---

void updateLCD() {
    if (!lcdAvailable || pLcd == nullptr) return;

    switch (currentMenu) {
        case STATE_HOME:          drawHomeScreen(); break;
        case STATE_MAIN_MENU:     drawMainMenuScreen(); break;
        case STATE_GLOBAL_TARGET: drawGlobalTargetScreen(); break;
        case STATE_GLOBAL_EDIT:   drawGlobalEditScreen(); break;
        case STATE_SELECT_RELAY:  drawSelectRelayScreen(); break;
        case STATE_RELAY_MENU:    drawRelayMenuScreen(); break;
        case STATE_EDIT_VALUE:    drawEditValueScreen(); break;
    }
}

void drawHomeScreen() {
    // Auto-cycle home screen every 5 seconds between Temperature (0) and Humidity (1)
    static unsigned long lastHomeCycle = 0;
    static int homePage = 0;

    if (millis() - lastHomeCycle > 5000) {
        lastHomeCycle = millis();
        homePage = (homePage + 1) % 4;
        if (lcdAvailable && pLcd != nullptr) pLcd->clear();
    }

    if (homePage == 0) {
        // --- PAGE 1: TEMPERATURE (T1, T2, T3, AVG) ---
        pLcd->setCursor(0, 0);
        if (valid1) {
            char b[21]; snprintf(b, sizeof(b), " Temp 1 :   %4.1f %cC ", temp1, 223);
            pLcd->print(b);
        } else {
            pLcd->print(" Temp 1 :   --.- \xDF" "C ");
        }

        pLcd->setCursor(0, 1);
        if (valid2) {
            char b[21]; snprintf(b, sizeof(b), " Temp 2 :   %4.1f %cC ", temp2, 223);
            pLcd->print(b);
        } else {
            pLcd->print(" Temp 2 :   --.- \xDF" "C ");
        }

        pLcd->setCursor(0, 2);
        if (valid3) {
            char b[21]; snprintf(b, sizeof(b), " Temp 3 :   %4.1f %cC ", temp3, 223);
            pLcd->print(b);
        } else {
            pLcd->print(" Temp 3 :   --.- \xDF" "C ");
        }

        pLcd->setCursor(0, 3);
        if (sensorValid) {
            char b[21]; snprintf(b, sizeof(b), ">AVG TEMP:  %4.1f %cC ", avgTemp, 223);
            pLcd->print(b);
        } else {
            pLcd->print(">AVG TEMP:  --.- \xDF" "C ");
        }
    } else if (homePage == 1) {
        // --- PAGE 2: HUMIDITY (H1, H2, H3, AVG) ---
        pLcd->setCursor(0, 0);
        if (valid1) {
            char b[21]; snprintf(b, sizeof(b), " Humid 1:   %4.1f %%  ", hum1);
            pLcd->print(b);
        } else {
            pLcd->print(" Humid 1:   --.- %   ");
        }

        pLcd->setCursor(0, 1);
        if (valid2) {
            char b[21]; snprintf(b, sizeof(b), " Humid 2:   %4.1f %%  ", hum2);
            pLcd->print(b);
        } else {
            pLcd->print(" Humid 2:   --.- %   ");
        }

        pLcd->setCursor(0, 2);
        if (valid3) {
            char b[21]; snprintf(b, sizeof(b), " Humid 3:   %4.1f %%  ", hum3);
            pLcd->print(b);
        } else {
            pLcd->print(" Humid 3:   --.- %   ");
        }

        pLcd->setCursor(0, 3);
        if (sensorValid) {
            char b[21]; snprintf(b, sizeof(b), ">AVG HUM :  %4.1f %%  ", avgHum);
            pLcd->print(b);
        } else {
            pLcd->print(">AVG HUM :  --.- %   ");
        }
    } else if (homePage == 2) {
        // --- PAGE 3: WATER FLOW (Rate, Total, Pulses - Display Only) ---
        pLcd->setCursor(0, 0);
        pLcd->print("=== WATER SENSOR ===");

        pLcd->setCursor(0, 1);
        char b1[21];
        snprintf(b1, sizeof(b1), " Rate :   %4.1f L/min ", flowRate);
        pLcd->print(b1);

        pLcd->setCursor(0, 2);
        char b2[21];
        snprintf(b2, sizeof(b2), " Total:  %5.1f Liter ", totalLiters);
        pLcd->print(b2);

        pLcd->setCursor(0, 3);
        char b3[21];
        snprintf(b3, sizeof(b3), " Pulse:  %-7lu    ", totalPulses);
        pLcd->print(b3);
    } else {
        // --- PAGE 4: MQ-137 AMMONIA GAS SENSOR (PPM, Status, Voltage - Display Only) ---
        pLcd->setCursor(0, 0);
        pLcd->print("=== AMMONIA SENSOR =");

        pLcd->setCursor(0, 1);
        char b1[21];
        if (!mq137Warmed) {
            unsigned long rem = (MQ137_WARMUP > (millis() - mq137WarmStart)) ? (MQ137_WARMUP - (millis() - mq137WarmStart)) / 1000 : 0;
            snprintf(b1, sizeof(b1), " Status: WARMING %2lus", rem);
        } else if (currentNH3Alert) {
            snprintf(b1, sizeof(b1), " Status: GAS ALERT! ");
        } else {
            snprintf(b1, sizeof(b1), " Status: NORMAL     ");
        }
        pLcd->print(b1);

        pLcd->setCursor(0, 2);
        char b2[21];
        snprintf(b2, sizeof(b2), " NH3   : %5.1f ppm  ", currentNH3);
        pLcd->print(b2);

        pLcd->setCursor(0, 3);
        char b3[21];
        snprintf(b3, sizeof(b3), " Volt  : %4.2fV  D:%s ", currentMQ137V, currentNH3Alert ? "ACT" : "OK ");
        pLcd->print(b3);
    }
}

void drawMainMenuScreen() {
    pLcd->setCursor(0, 0);
    pLcd->print("==== MAIN MENU =====");

    pLcd->setCursor(0, 1);
    pLcd->print(mainMenuIndex == 0 ? "> 1. Temperature    " : "  1. Temperature    ");

    pLcd->setCursor(0, 2);
    pLcd->print(mainMenuIndex == 1 ? "> 2. Humidity       " : "  2. Humidity       ");

    pLcd->setCursor(0, 3);
    pLcd->print(mainMenuIndex == 2 ? "> 3. Relays (1-16)  " : "  3. Relays (1-16)  ");
}

void drawGlobalTargetScreen() {
    pLcd->setCursor(0, 0);
    if (mainMenuIndex == 0) {
        pLcd->print("= GLOBAL TEMP (ALL)=");
    } else {
        pLcd->print("= GLOBAL HUM  (ALL)=");
    }

    // Line 1: Live 3-sensor readings and live Average
    pLcd->setCursor(0, 1);
    if (sensorValid) {
        char buf[21];
        if (mainMenuIndex == 0) {
            char s1[4], s2[4], s3[4], sAvg[4];
            if (valid1) snprintf(s1, sizeof(s1), "%2.0f", temp1); else strcpy(s1, "--");
            if (valid2) snprintf(s2, sizeof(s2), "%2.0f", temp2); else strcpy(s2, "--");
            if (valid3) snprintf(s3, sizeof(s3), "%2.0f", temp3); else strcpy(s3, "--");
            snprintf(sAvg, sizeof(sAvg), "%2.0f", avgTemp);
            snprintf(buf, sizeof(buf), "T: %s, %s, %s Avg.%s", s1, s2, s3, sAvg);
        } else {
            char h1s[4], h2s[4], h3s[4], hAvgs[4];
            if (valid1) snprintf(h1s, sizeof(h1s), "%2.0f", hum1); else strcpy(h1s, "--");
            if (valid2) snprintf(h2s, sizeof(h2s), "%2.0f", hum2); else strcpy(h2s, "--");
            if (valid3) snprintf(h3s, sizeof(h3s), "%2.0f", hum3); else strcpy(h3s, "--");
            snprintf(hAvgs, sizeof(hAvgs), "%2.0f", avgHum);
            snprintf(buf, sizeof(buf), "H: %s, %s, %s Avg.%s", h1s, h2s, h3s, hAvgs);
        }
        pLcd->print(buf);
    } else {
        pLcd->print(mainMenuIndex == 0 ? "T: --, --, -- Avg.--" : "H: --, --, -- Avg.--");
    }

    // Line 2: Min / Max setting targets with selector
    pLcd->setCursor(0, 2);
    char b2[21];
    if (mainMenuIndex == 0) {
        snprintf(b2, sizeof(b2), "%sMn:%4.1fC  %sMx:%4.1fC", 
                 globalTarget == 0 ? ">" : " ", globalTempMin,
                 globalTarget == 1 ? ">" : " ", globalTempMax);
    } else {
        snprintf(b2, sizeof(b2), "%sMn:%4.1f%% %sMx:%4.1f%%", 
                 globalTarget == 0 ? ">" : " ", globalHumMin,
                 globalTarget == 1 ? ">" : " ", globalHumMax);
    }
    pLcd->print(b2);

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU]Edit  [BCK]Bck");
}

void drawGlobalEditScreen() {
    pLcd->setCursor(0, 0);
    if (mainMenuIndex == 0) {
        pLcd->print(globalTarget == 0 ? "ALL RELAYS: MIN TEMP" : "ALL RELAYS: MAX TEMP");
    } else {
        pLcd->print(globalTarget == 0 ? "ALL RELAYS: MIN HUM " : "ALL RELAYS: MAX HUM ");
    }

    pLcd->setCursor(0, 1);
    char refBuf[21];
    if (mainMenuIndex == 0) {
        snprintf(refBuf, sizeof(refBuf), "Live Avg: %4.1f %cC   ", avgTemp, 223);
    } else {
        snprintf(refBuf, sizeof(refBuf), "Live Avg: %4.1f %%    ", avgHum);
    }
    pLcd->print(refBuf);

    pLcd->setCursor(0, 2);
    char b2[21];
    if (mainMenuIndex == 0) {
        snprintf(b2, sizeof(b2), " >>> [ %4.1f %cC ] <<<", editingVal, 223);
    } else {
        snprintf(b2, sizeof(b2), " >>> [ %4.1f %%  ] <<<", editingVal);
    }
    pLcd->print(b2);

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU]Apply [BCK]Esc");
}

void drawSelectRelayScreen() {
    pLcd->setCursor(0, 0);
    pLcd->print("--- SELECT RELAY ---");

    pLcd->setCursor(0, 1);
    char buf1[21];
    // Show custom name (or Relay 01) up to 13 chars
    snprintf(buf1, sizeof(buf1), "> R%02d: %-13.13s", selectedRelay + 1, configs[selectedRelay].name);
    pLcd->print(buf1);

    pLcd->setCursor(0, 2);
    char buf2[21];
    const char* mShort = "Thrs";
    if (configs[selectedRelay].mode == 1) mShort = "T+Sn";
    else if (configs[selectedRelay].mode == 2) mShort = "Pure";
    else if (configs[selectedRelay].mode == 3) mShort = "Man ";

    const char* sName = getSensorName(configs[selectedRelay].sensor);
    const char* stStr = relayStates[selectedRelay] ? "ON " : "OFF";
    snprintf(buf2, sizeof(buf2), "S:%-7.7s %-4.4s %-3s", sName, mShort, stStr);
    pLcd->print(buf2);

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU]Conf  [BCK]Bck");
}

void drawRelayMenuScreen() {
    pLcd->setCursor(0, 0);
    char b0[21];
    snprintf(b0, sizeof(b0), "-- R%02d: %-10.10s -", selectedRelay + 1, configs[selectedRelay].name);
    pLcd->print(b0);

    const char* mStr = "Threshold";
    if (configs[selectedRelay].mode == 1) mStr = "Tmr+Sens ";
    else if (configs[selectedRelay].mode == 2) mStr = "Pure Tmr ";
    else if (configs[selectedRelay].mode == 3) mStr = "Manual   ";

    const char* uStr = getSensorUnit(configs[selectedRelay].sensor);

    // Sliding window of 3 rows across 6 items (0 to 5)
    int startIdx = (relayMenuIndex >= 3) ? (relayMenuIndex - 2) : 0;
    if (startIdx > 3) startIdx = 3;

    for (int row = 1; row <= 3; row++) {
        int item = startIdx + (row - 1);
        pLcd->setCursor(0, row);
        char lineBuf[21];
        char cursorChar = (item == relayMenuIndex) ? '>' : ' ';

        if (item == 0) {
            snprintf(lineBuf, sizeof(lineBuf), "%c1.Sens:%-10.10s", cursorChar, getSensorName(configs[selectedRelay].sensor));
        } else if (item == 1) {
            snprintf(lineBuf, sizeof(lineBuf), "%c2.Mode:%-10.10s", cursorChar, mStr);
        } else if (item == 2) {
            snprintf(lineBuf, sizeof(lineBuf), "%c3.Min :%5.1f%-3s", cursorChar, configs[selectedRelay].minVal, uStr);
        } else if (item == 3) {
            snprintf(lineBuf, sizeof(lineBuf), "%c4.Max :%5.1f%-3s", cursorChar, configs[selectedRelay].maxVal, uStr);
        } else if (item == 4) {
            snprintf(lineBuf, sizeof(lineBuf), "%c5.T-On:%4d sec", cursorChar, configs[selectedRelay].timerOnSec);
        } else if (item == 5) {
            snprintf(lineBuf, sizeof(lineBuf), "%c6.T-Off:%3d sec", cursorChar, configs[selectedRelay].timerOffSec);
        }
        pLcd->print(lineBuf);
    }
}

void drawEditValueScreen() {
    pLcd->setCursor(0, 0);
    char b0[21];
    if (selectedTarget == 2) {
        snprintf(b0, sizeof(b0), "R%02d: RUN TIME (SEC)", selectedRelay + 1);
    } else if (selectedTarget == 3) {
        snprintf(b0, sizeof(b0), "R%02d: PAUSE TIM(SEC)", selectedRelay + 1);
    } else {
        const char* sName = getSensorName(configs[selectedRelay].sensor);
        snprintf(b0, sizeof(b0), "R%02d %s: %-8.8s", selectedRelay + 1, selectedTarget == 0 ? "MIN" : "MAX", sName);
    }
    pLcd->print(b0);

    pLcd->setCursor(0, 1);
    pLcd->print("Press UP/DN to set: ");

    pLcd->setCursor(0, 2);
    char b2[21];
    if (selectedTarget == 2 || selectedTarget == 3) {
        int sec = (int)editingVal;
        if (sec >= 60 && sec % 60 == 0) {
            snprintf(b2, sizeof(b2), " >>> [ %3ds (%dm) ] <<<", sec, sec / 60);
        } else {
            snprintf(b2, sizeof(b2), " >>> [ %4d sec ] <<<", sec);
        }
    } else {
        const char* u = getSensorUnit(configs[selectedRelay].sensor);
        snprintf(b2, sizeof(b2), " >>> [ %5.1f %-2s ] <<<", editingVal, u);
    }
    pLcd->print(b2);

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU]Save  [BCK]Esc");
}

// --- SENSOR & AUTO EVALUATION ---

void readDHTSensor() {
    float t1 = dht1.readTemperature();
    float h1 = dht1.readHumidity();
    float t2 = dht2.readTemperature();
    float h2 = dht2.readHumidity();
    float t3 = dht3.readTemperature();
    float h3 = dht3.readHumidity();

    bool ok1 = !isnan(t1) && !isnan(h1) && t1 > -20.0 && t1 < 80.0;
    bool ok2 = !isnan(t2) && !isnan(h2) && t2 > -20.0 && t2 < 80.0;
    bool ok3 = !isnan(t3) && !isnan(h3) && t3 > -20.0 && t3 < 80.0;

    if (ok1) {
        rawTemp1 = t1;
        rawHum1 = h1;
        temp1 = rawTemp1 + calib.t1Offset;
        hum1 = constrain(rawHum1 + calib.h1Offset, 0.0f, 100.0f);
        valid1 = true;
    }
    if (ok2) {
        rawTemp2 = t2;
        rawHum2 = h2;
        temp2 = rawTemp2 + calib.t2Offset;
        hum2 = constrain(rawHum2 + calib.h2Offset, 0.0f, 100.0f);
        valid2 = true;
    }
    if (ok3) {
        rawTemp3 = t3;
        rawHum3 = h3;
        temp3 = rawTemp3 + calib.t3Offset;
        hum3 = constrain(rawHum3 + calib.h3Offset, 0.0f, 100.0f);
        valid3 = true;
    }

    float sumT = 0.0;
    float sumH = 0.0;
    int count = 0;

    if (valid1) { sumT += temp1; sumH += hum1; count++; }
    if (valid2) { sumT += temp2; sumH += hum2; count++; }
    if (valid3) { sumT += temp3; sumH += hum3; count++; }

    if (count > 0) {
        avgTemp = sumT / count;
        avgHum = sumH / count;
        currentTemp = avgTemp; // Relay decisions are made on average value
        currentHum = avgHum;
        sensorValid = true;
    } else {
        sensorValid = false;
    }

    static unsigned long lastLog = 0;
    if (millis() - lastLog > 4000) {
        lastLog = millis();
        Serial.printf("🌡️ Sensors (Calibrated) -> S1:%s(%.1fC, raw:%.1f) | S2:%s(%.1fC, raw:%.1f) | S3:%s(%.1fC, raw:%.1f) => AVG: %.1fC, %.1f%%\n",
            valid1 ? "OK" : "NC", temp1, rawTemp1,
            valid2 ? "OK" : "NC", temp2, rawTemp2,
            valid3 ? "OK" : "NC", temp3, rawTemp3,
            avgTemp, avgHum);
    }
}

void evaluateSingleRelay(int i, bool isConfigUpdate) {
    if (configs[i].mode == 3 || configs[i].sensor == 0) return; // Manual mode

    unsigned long now = millis();

    // Reset timer phase on explicit config change
    if (isConfigUpdate && (configs[i].mode == 1 || configs[i].mode == 2 || configs[i].sensor == 11)) {
        relayTimerLastSwitch[i] = 0;
    }

    int onDur  = (configs[i].timerOnSec > 0) ? configs[i].timerOnSec : 30;
    int offDur = (configs[i].timerOffSec > 0) ? configs[i].timerOffSec : 120;

    // --- MODE 2 or SENSOR 11: PURE CYCLIC TIMER ---
    if (configs[i].mode == 2 || configs[i].sensor == 11) {
        if (relayTimerLastSwitch[i] == 0) {
            relayTimerLastSwitch[i] = now;
            relayTimerInOnPhase[i] = true;
            setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(onDur) + "s)");
            return;
        }
        unsigned long elapsed = (now - relayTimerLastSwitch[i]) / 1000;
        if (relayTimerInOnPhase[i]) {
            if (elapsed >= (unsigned long)onDur) {
                relayTimerInOnPhase[i] = false;
                relayTimerLastSwitch[i] = now;
                setRelay(i, false, "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(offDur) + "s)");
            } else {
                int left = onDur - elapsed;
                if (!relayStates[i]) setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(left) + "s left)");
                else relayReason[i] = "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(left) + "s left)";
            }
        } else {
            if (elapsed >= (unsigned long)offDur) {
                relayTimerInOnPhase[i] = true;
                relayTimerLastSwitch[i] = now;
                setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(onDur) + "s)");
            } else {
                int left = offDur - elapsed;
                if (relayStates[i]) setRelay(i, false, "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(left) + "s left)");
                else relayReason[i] = "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(left) + "s left)";
            }
        }
        return;
    }

    // --- SENSOR READING FOR ASSIGNED SENSOR ---
    float val = 0.0f;
    bool isValid = false;
    getSensorReading(configs[i].sensor, val, isValid);
    if (!isValid) {
        relayReason[i] = "⚠️ Sensor Offline (" + String(getSensorName(configs[i].sensor)) + ")";
        return;
    }

    const char* uStr = getSensorUnit(configs[i].sensor);
    const char* sName = getSensorName(configs[i].sensor);
    float minVal = configs[i].minVal;
    float maxVal = configs[i].maxVal;
    if (minVal > maxVal) { float t = minVal; minVal = maxVal; maxVal = t; }

    bool thresholdTriggered = false;
    bool targetState = false;
    String trigReason = "";

    if (configs[i].action == 0) {
        // Outside Range ON (≤Min or ≥Max)
        if (val >= maxVal) {
            thresholdTriggered = true; targetState = true;
            trigReason = "⚠️ " + String(configs[i].name) + " ON: " + sName + " " + String(val, 1) + uStr + " >= Max " + String(maxVal, 1) + uStr;
        } else if (val <= minVal) {
            thresholdTriggered = true; targetState = true;
            trigReason = "⚠️ " + String(configs[i].name) + " ON: " + sName + " " + String(val, 1) + uStr + " <= Min " + String(minVal, 1) + uStr;
        } else {
            thresholdTriggered = false; targetState = false;
            trigReason = "Safe Range: " + String(val, 1) + uStr + " (" + String(minVal, 1) + "-" + String(maxVal, 1) + uStr + ")";
        }
    } else if (configs[i].action == 1) {
        // Inside Range ON (Min to Max)
        if (val >= minVal && val <= maxVal) {
            thresholdTriggered = true; targetState = true;
            trigReason = String("In Range: ") + sName + " " + String(val, 1) + uStr + " (ON)";
        } else {
            thresholdTriggered = false; targetState = false;
            trigReason = String("Out of Range: ") + sName + " " + String(val, 1) + uStr + " (OFF)";
        }
    } else if (configs[i].action == 2) {
        // Above Max ON (Cooling / Exhaust Fan)
        if (val >= maxVal) {
            thresholdTriggered = true; targetState = true;
            trigReason = String("Above Max: ") + sName + " " + String(val, 1) + uStr + " >= " + String(maxVal, 1) + uStr + " (ON)";
        } else if (val <= minVal) {
            thresholdTriggered = false; targetState = false;
            trigReason = String("Below Min: ") + sName + " " + String(val, 1) + uStr + " <= " + String(minVal, 1) + uStr + " (OFF)";
        } else {
            // Deadband hysteresis: maintain current state
            targetState = relayStates[i];
            thresholdTriggered = relayStates[i];
            trigReason = String("Normal Band: ") + sName + " " + String(val, 1) + uStr + " (Hold " + (targetState ? "ON" : "OFF") + ")";
        }
    } else if (configs[i].action == 3) {
        // Below Min ON (Heating / Scrub)
        if (val <= minVal) {
            thresholdTriggered = true; targetState = true;
            trigReason = String("Below Min: ") + sName + " " + String(val, 1) + uStr + " <= " + String(minVal, 1) + uStr + " (ON)";
        } else if (val >= maxVal) {
            thresholdTriggered = false; targetState = false;
            trigReason = String("Above Max: ") + sName + " " + String(val, 1) + uStr + " >= " + String(maxVal, 1) + uStr + " (OFF)";
        } else {
            // Deadband hysteresis: maintain current state
            targetState = relayStates[i];
            thresholdTriggered = relayStates[i];
            trigReason = String("Normal Band: ") + sName + " " + String(val, 1) + uStr + " (Hold " + (targetState ? "ON" : "OFF") + ")";
        }
    }

    // MODE 1: TIMER + SENSOR THRESHOLD OVERRIDE
    if (configs[i].mode == 1) {
        if (thresholdTriggered && targetState) {
            // Safety threshold OVERRIDES the timer!
            if (relayStates[i] != targetState) {
                setRelay(i, targetState, trigReason + " [OVERRIDE]");
            } else {
                relayReason[i] = trigReason + " [OVERRIDE]";
            }
            relayTimerLastSwitch[i] = now;
            relayTimerInOnPhase[i] = false;
            return;
        }

        // Within safe range: Cyclic Timer Duty Cycle runs!
        if (relayTimerLastSwitch[i] == 0) {
            relayTimerLastSwitch[i] = now;
            relayTimerInOnPhase[i] = true;
            setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(onDur) + "s) [Safe " + String(val, 1) + uStr + "]");
            return;
        }

        unsigned long elapsed = (now - relayTimerLastSwitch[i]) / 1000;
        if (relayTimerInOnPhase[i]) {
            if (elapsed >= (unsigned long)onDur) {
                relayTimerInOnPhase[i] = false;
                relayTimerLastSwitch[i] = now;
                setRelay(i, false, "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(offDur) + "s) [Safe " + String(val, 1) + uStr + "]");
            } else {
                int left = onDur - elapsed;
                if (!relayStates[i]) setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(left) + "s) [Safe " + String(val, 1) + uStr + "]");
                else relayReason[i] = "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(left) + "s) [Safe " + String(val, 1) + uStr + "]";
            }
        } else {
            if (elapsed >= (unsigned long)offDur) {
                relayTimerInOnPhase[i] = true;
                relayTimerLastSwitch[i] = now;
                setRelay(i, true, "⏱️ " + String(configs[i].name) + ": Timer ON (" + String(onDur) + "s) [Safe " + String(val, 1) + uStr + "]");
            } else {
                int left = offDur - elapsed;
                if (relayStates[i]) setRelay(i, false, "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(left) + "s) [Safe " + String(val, 1) + uStr + "]");
                else relayReason[i] = "⏱️ " + String(configs[i].name) + ": Timer OFF (" + String(left) + "s) [Safe " + String(val, 1) + uStr + "]";
            }
        }
        return;
    }

    // MODE 0: SENSOR THRESHOLD ONLY (NO TIMER)
    if (configs[i].mode == 0) {
        if (relayStates[i] != targetState) {
            setRelay(i, targetState, trigReason);
        } else {
            relayReason[i] = trigReason;
        }
    }
}

void evaluateAutoRules() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        bool prevState = relayStates[i];
        evaluateSingleRelay(i, false);
        // Stagger switching so multiple relays turn ON/OFF sequentially ("baari baari")
        if (prevState != relayStates[i]) {
            delay(200);
        }
    }
}

void setRelay(int index, bool state, String reason) {
    if (index < 0 || index >= NUM_RELAYS) return;
    relayStates[index] = state;
    relayReason[index] = reason;
    digitalWrite(RELAY_PINS[index], state ? RELAY_ON : RELAY_OFF);
    Serial.printf("⚡ %s (GPIO %02d) -> %s | %s\n", configs[index].name, RELAY_PINS[index], state ? "ON" : "OFF", reason.c_str());
}

void turnAll(bool state) {
    for (int i = 0; i < NUM_RELAYS; i++) {
        configs[i].mode = 3; // Manual
        setRelay(i, state, state ? "Manual Master ON" : "Manual Master OFF");
    }
}

void setAllAuto() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        configs[i].mode = 0; // 0 = Threshold Only
        if (configs[i].sensor == 0) {
            configs[i].sensor = 1; // 1 = Avg Temp if unassigned
        }
        saveRelayConfig(i);
    }
    evaluateAutoRules();
}

void handleAllAuto() {
    setAllAuto();
    server.send(200, "text/plain", "OK");
}

// --- NVS STORAGE ---

void loadRelayConfigs() {
    prefRelay.begin("r16-cfg", false);

    globalTempMin = prefRelay.getFloat("gTMin", 28.0f);
    globalTempMax = prefRelay.getFloat("gTMax", 32.0f);
    globalHumMin  = prefRelay.getFloat("gHMin", 40.0f);
    globalHumMax  = prefRelay.getFloat("gHMax", 75.0f);

    float defaultMins[16] = {
        26.5, 26.8, 27.2, 27.5, 27.8, 28.2, 28.5, 28.8,
        29.2, 29.5, 29.8, 30.2, 30.5, 30.8, 31.2, 31.5
    };
    float defaultMaxs[16] = {
        27.0, 27.3, 27.6, 28.0, 28.3, 28.6, 29.0, 29.3,
        29.7, 30.0, 30.3, 30.7, 31.0, 31.3, 31.7, 32.0
    };

    for (int i = 0; i < NUM_RELAYS; i++) {
        String keyNm  = "nm" + String(i);
        String keyS   = "s" + String(i);
        String keyM   = "m" + String(i);
        String keyMin = "min" + String(i);
        String keyMax = "max" + String(i);
        String keyA   = "a" + String(i);
        String keyTon = "ton" + String(i);
        String keyToff= "toff" + String(i);

        char defName[16];
        snprintf(defName, sizeof(defName), "Relay %02d", i + 1);
        String sName = prefRelay.getString(keyNm.c_str(), defName);
        strncpy(configs[i].name, sName.c_str(), sizeof(configs[i].name) - 1);
        configs[i].name[sizeof(configs[i].name) - 1] = '\0';

        configs[i].sensor = prefRelay.getInt(keyS.c_str(), 1); // Default Avg Temp
        configs[i].mode   = prefRelay.getInt(keyM.c_str(), 0); // Default Threshold Only

        // Auto-recover any relay stuck in Manual mode (3) that has an assigned sensor
        if (configs[i].mode == 3 && configs[i].sensor > 0) {
            configs[i].mode = 0;
            saveRelayConfig(i);
        }

        configs[i].minVal = prefRelay.getFloat(keyMin.c_str(), defaultMins[i]);
        configs[i].maxVal = prefRelay.getFloat(keyMax.c_str(), defaultMaxs[i]);
        configs[i].action = prefRelay.getInt(keyA.c_str(), 0);
        configs[i].timerOnSec  = prefRelay.getInt(keyTon.c_str(), 30);
        configs[i].timerOffSec = prefRelay.getInt(keyToff.c_str(), 120);
        if (configs[i].timerOnSec <= 0) configs[i].timerOnSec = 30;
        if (configs[i].timerOffSec <= 0) configs[i].timerOffSec = 120;
    }

    // Load Sensor Calibrations & Offsets
    calib.t1Offset   = prefRelay.getFloat("calT1", 0.0f);
    calib.h1Offset   = prefRelay.getFloat("calH1", 0.0f);
    calib.t2Offset   = prefRelay.getFloat("calT2", 0.0f);
    calib.h2Offset   = prefRelay.getFloat("calH2", 0.0f);
    calib.t3Offset   = prefRelay.getFloat("calT3", 0.0f);
    calib.h3Offset   = prefRelay.getFloat("calH3", 0.0f);
    calib.nh3Factor  = prefRelay.getFloat("calNH3", 1.0f);
    calib.flowFactor = prefRelay.getFloat("calFlow", 7.5f);
    if (calib.flowFactor < 0.1f) calib.flowFactor = 7.5f;
    if (calib.nh3Factor <= 0.0f) calib.nh3Factor = 1.0f;
}

void saveRelayConfig(int idx) {
    if (idx < 0 || idx >= NUM_RELAYS) return;
    String keyNm  = "nm" + String(idx);
    String keyS   = "s" + String(idx);
    String keyM   = "m" + String(idx);
    String keyMin = "min" + String(idx);
    String keyMax = "max" + String(idx);
    String keyA   = "a" + String(idx);
    String keyTon = "ton" + String(idx);
    String keyToff= "toff" + String(idx);

    prefRelay.putString(keyNm.c_str(), configs[idx].name);
    prefRelay.putInt(keyS.c_str(), configs[idx].sensor);
    prefRelay.putInt(keyM.c_str(), configs[idx].mode);
    prefRelay.putFloat(keyMin.c_str(), configs[idx].minVal);
    prefRelay.putFloat(keyMax.c_str(), configs[idx].maxVal);
    prefRelay.putInt(keyA.c_str(), configs[idx].action);
    prefRelay.putInt(keyTon.c_str(), configs[idx].timerOnSec);
    prefRelay.putInt(keyToff.c_str(), configs[idx].timerOffSec);
}

void saveCalibration() {
    prefRelay.putFloat("calT1", calib.t1Offset);
    prefRelay.putFloat("calH1", calib.h1Offset);
    prefRelay.putFloat("calT2", calib.t2Offset);
    prefRelay.putFloat("calH2", calib.h2Offset);
    prefRelay.putFloat("calT3", calib.t3Offset);
    prefRelay.putFloat("calH3", calib.h3Offset);
    prefRelay.putFloat("calNH3", calib.nh3Factor);
    prefRelay.putFloat("calFlow", calib.flowFactor);
    Serial.printf("⚡ Saved Calibration: T1:%+.1f, H1:%+.1f, T2:%+.1f, H2:%+.1f, T3:%+.1f, H3:%+.1f, NH3:%.2fx, Flow:%.2f\n",
        calib.t1Offset, calib.h1Offset, calib.t2Offset, calib.h2Offset, calib.t3Offset, calib.h3Offset, calib.nh3Factor, calib.flowFactor);
}

void handleSaveCalibration() {
    if (server.hasArg("t1"))   calib.t1Offset   = server.arg("t1").toFloat();
    if (server.hasArg("h1"))   calib.h1Offset   = server.arg("h1").toFloat();
    if (server.hasArg("t2"))   calib.t2Offset   = server.arg("t2").toFloat();
    if (server.hasArg("h2"))   calib.h2Offset   = server.arg("h2").toFloat();
    if (server.hasArg("t3"))   calib.t3Offset   = server.arg("t3").toFloat();
    if (server.hasArg("h3"))   calib.h3Offset   = server.arg("h3").toFloat();
    if (server.hasArg("nh3"))  calib.nh3Factor  = server.arg("nh3").toFloat();
    if (server.hasArg("flow")) calib.flowFactor = server.arg("flow").toFloat();

    if (calib.flowFactor < 0.1f) calib.flowFactor = 7.5f;
    if (calib.nh3Factor <= 0.0f) calib.nh3Factor = 1.0f;

    saveCalibration();
    readDHTSensor();
    evaluateAutoRules();
    server.send(200, "text/plain", "Calibration Saved");
}

// --- WEB HANDLERS ---

void handleRelayToggle() {
    if (server.hasArg("id") && server.hasArg("state")) {
        int id = server.arg("id").toInt() - 1;
        bool state = (server.arg("state") == "1");
        if (id >= 0 && id < NUM_RELAYS) {
            configs[id].mode = 3; // Manual mode
            saveRelayConfig(id);
            setRelay(id, state, "Manual Web Toggle");
            server.send(200, "text/plain", "OK");
            return;
        }
    }
    server.send(400, "text/plain", "Bad Request");
}

void handleAllRelays() {
    if (server.hasArg("state")) {
        bool state = (server.arg("state") == "1");
        turnAll(state);
        server.send(200, "text/plain", "OK");
        return;
    }
    server.send(400, "text/plain", "Bad Request");
}

void handleSaveRelayConfig() {
    if (server.hasArg("id")) {
        int id = server.arg("id").toInt() - 1;
        if (id >= 0 && id < NUM_RELAYS) {
            if (server.hasArg("name")) {
                String n = server.arg("name");
                n.trim();
                if (n.length() > 0) {
                    strncpy(configs[id].name, n.c_str(), sizeof(configs[id].name) - 1);
                    configs[id].name[sizeof(configs[id].name) - 1] = '\0';
                }
            }
            if (server.hasArg("sensor")) configs[id].sensor = server.arg("sensor").toInt();
            if (server.hasArg("mode"))   configs[id].mode   = server.arg("mode").toInt();
            if (server.hasArg("min"))    configs[id].minVal = server.arg("min").toFloat();
            if (server.hasArg("max"))    configs[id].maxVal = server.arg("max").toFloat();
            if (server.hasArg("action")) configs[id].action = server.arg("action").toInt();
            if (server.hasArg("ton"))    configs[id].timerOnSec  = server.arg("ton").toInt();
            if (server.hasArg("toff"))   configs[id].timerOffSec = server.arg("toff").toInt();

            if (configs[id].timerOnSec <= 0) configs[id].timerOnSec = 30;
            if (configs[id].timerOffSec <= 0) configs[id].timerOffSec = 120;

            // Ensure minVal is lower than maxVal
            if (configs[id].minVal > configs[id].maxVal) {
                float t = configs[id].minVal;
                configs[id].minVal = configs[id].maxVal;
                configs[id].maxVal = t;
            }

            relayTimerLastSwitch[id] = 0; // Trigger timer fresh
            saveRelayConfig(id);
            readDHTSensor();
            evaluateSingleRelay(id, true);
            Serial.printf("⚡ Web Saved Relay %d [%s]: Sensor=%d, Mode=%d, Min=%.1f, Max=%.1f, Ton=%d, Toff=%d -> State: %s (%s)\n",
                id + 1, configs[id].name, configs[id].sensor, configs[id].mode, configs[id].minVal, configs[id].maxVal,
                configs[id].timerOnSec, configs[id].timerOffSec,
                relayStates[id] ? "ON" : "OFF", relayReason[id].c_str());
            server.send(200, "text/plain", "Config Saved");
            return;
        }
    }
    server.send(400, "text/plain", "Bad Request");
}

void handleBatchRange() {
    if (server.hasArg("min") && server.hasArg("max")) {
        float bMin = server.arg("min").toFloat();
        float bMax = server.arg("max").toFloat();
        if (bMin > bMax) { float t = bMin; bMin = bMax; bMax = t; }

        for (int i = 0; i < NUM_RELAYS; i++) {
            configs[i].sensor = 1; // Auto Avg Temp
            configs[i].mode   = 0; // Threshold Only
            configs[i].action = 0; // Outside Range ON
            configs[i].minVal = bMin;
            configs[i].maxVal = bMax;
            saveRelayConfig(i);
        }
        readDHTSensor();
        for (int i = 0; i < NUM_RELAYS; i++) {
            evaluateSingleRelay(i, true);
        }
        Serial.printf("⚡ Batch Range Applied: %.1fC - %.1fC across 16 relays\n", bMin, bMax);
        server.send(200, "text/plain", "Batch Range Applied");
        return;
    }
    server.send(400, "text/plain", "Bad Request");
}

void handleGetStatus() {
    String json = "{";
    json += "\"temp\":" + String(sensorValid ? String(currentTemp, 1) : "\"--\"") + ",";
    json += "\"temp1\":" + String(valid1 ? String(temp1, 1) : "\"--\"") + ",";
    json += "\"temp2\":" + String(valid2 ? String(temp2, 1) : "\"--\"") + ",";
    json += "\"temp3\":" + String(valid3 ? String(temp3, 1) : "\"--\"") + ",";
    json += "\"hum\":" + String(sensorValid ? String(currentHum, 1) : "\"--\"") + ",";
    json += "\"hum1\":" + String(valid1 ? String(hum1, 1) : "\"--\"") + ",";
    json += "\"hum2\":" + String(valid2 ? String(hum2, 1) : "\"--\"") + ",";
    json += "\"hum3\":" + String(valid3 ? String(hum3, 1) : "\"--\"") + ",";
    json += "\"sensorOk\":" + String(sensorValid ? "true" : "false") + ",";
    json += "\"lcdStatus\":\"" + lcdStatusStr + "\",";
    json += "\"flowRate\":" + String(flowRate, 1) + ",";
    json += "\"totalLiters\":" + String(totalLiters, 1) + ",";
    json += "\"flowPulses\":" + String(totalPulses) + ",";
    json += "\"nh3\":" + String(currentNH3, 1) + ",";
    json += "\"nh3V\":" + String(currentMQ137V, 2) + ",";
    json += "\"nh3Alert\":" + String(currentNH3Alert ? "true" : "false") + ",";
    json += "\"nh3Warmed\":" + String(mq137Warmed ? "true" : "false") + ",";

    json += "\"relays\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"id\":" + String(i + 1) + ",";
        json += "\"name\":\"" + String(configs[i].name) + "\",";
        json += "\"pin\":" + String(RELAY_PINS[i]) + ",";
        json += "\"state\":" + String(relayStates[i] ? "1" : "0") + ",";
        json += "\"sensor\":" + String(configs[i].sensor) + ",";
        json += "\"mode\":" + String(configs[i].mode) + ",";
        json += "\"min\":" + String(configs[i].minVal, 1) + ",";
        json += "\"max\":" + String(configs[i].maxVal, 1) + ",";
        json += "\"action\":" + String(configs[i].action) + ",";
        json += "\"ton\":" + String(configs[i].timerOnSec) + ",";
        json += "\"toff\":" + String(configs[i].timerOffSec) + ",";
        json += "\"reason\":\"" + relayReason[i] + "\"";
        json += "}";
    }
    json += "],";

    json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"staSSID\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : saved_ssid) + "\",";
    json += "\"staIP\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Not Connected") + "\",";
    json += "\"staStatus\":\"" + sta_status_str + "\",";
    json += "\"apIP\":\"" + WiFi.softAPIP().toString() + "\",";

    json += "\"calib\":{";
    json += "\"t1\":" + String(calib.t1Offset, 1) + ",";
    json += "\"h1\":" + String(calib.h1Offset, 1) + ",";
    json += "\"t2\":" + String(calib.t2Offset, 1) + ",";
    json += "\"h2\":" + String(calib.h2Offset, 1) + ",";
    json += "\"t3\":" + String(calib.t3Offset, 1) + ",";
    json += "\"h3\":" + String(calib.h3Offset, 1) + ",";
    json += "\"nh3\":" + String(calib.nh3Factor, 2) + ",";
    json += "\"flow\":" + String(calib.flowFactor, 2) + ",";
    json += "\"rawT1\":" + String(rawTemp1, 1) + ",";
    json += "\"rawH1\":" + String(rawHum1, 1) + ",";
    json += "\"rawT2\":" + String(rawTemp2, 1) + ",";
    json += "\"rawH2\":" + String(rawHum2, 1) + ",";
    json += "\"rawT3\":" + String(rawTemp3, 1) + ",";
    json += "\"rawH3\":" + String(rawHum3, 1) + ",";
    json += "\"rawNH3\":" + String(rawNH3, 1);
    json += "}";
    json += "}";
    server.send(200, "application/json", json);
}

void handleScanNetworks() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleSaveWifi() {
    if (server.hasArg("ssid")) {
        String new_ssid = server.arg("ssid");
        String new_pass = server.hasArg("pass") ? server.arg("pass") : "";

        prefWifi.putString("ssid", new_ssid);
        prefWifi.putString("pass", new_pass);
        saved_ssid = new_ssid;
        saved_pass = new_pass;

        server.send(200, "text/plain", "WiFi saved. Restarting...");
        delay(1000);
        ESP.restart();
        return;
    }
    server.send(400, "text/plain", "Missing Parameters");
}

void handleResetWifi() {
    prefWifi.remove("ssid");
    prefWifi.remove("pass");
    saved_ssid = "";
    saved_pass = "";
    WiFi.disconnect();
    server.send(200, "text/plain", "WiFi settings cleared.");
}

// --- HTML / CSS / JS WEB DASHBOARD ---
void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 16-Relay Climate Hub</title>
    <style>
        :root {
            --bg: #0b1120;
            --card: #1e293b;
            --text: #f8fafc;
            --text-dim: #94a3b8;
            --accent: #38bdf8;
            --on-color: #22c55e;
            --off-color: #475569;
            --danger: #ef4444;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg); color: var(--text); padding: 12px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 760px; display: flex; flex-direction: column; gap: 12px; }
        
        .header { text-align: center; padding: 6px; }
        .header h1 { font-size: 1.4rem; color: var(--accent); }
        .header p { font-size: 0.8rem; color: var(--text-dim); }

        .hero-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .sensor-card { background: linear-gradient(135deg, #1e293b, #0f172a); border: 1px solid rgba(255,255,255,0.08); border-radius: 12px; padding: 12px; text-align: center; }
        .sensor-val { font-size: 2rem; font-weight: 700; margin: 4px 0; }
        .sensor-label { font-size: 0.78rem; color: var(--text-dim); text-transform: uppercase; letter-spacing: 0.5px; }

        .card { background: var(--card); border-radius: 12px; padding: 14px; border: 1px solid rgba(255,255,255,0.06); }
        .card-title { font-size: 0.95rem; font-weight: 600; margin-bottom: 10px; display: flex; justify-content: space-between; align-items: center; }

        .mod-header {
            font-size: 0.82rem;
            font-weight: 700;
            color: var(--accent);
            text-transform: uppercase;
            letter-spacing: 0.6px;
            margin: 14px 0 8px 0;
        }

        .btn-group { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
        .btn { padding: 10px; border-radius: 8px; border: none; font-weight: 600; cursor: pointer; font-size: 0.88rem; transition: 0.2s; }
        .btn-on { background: #16a34a; color: #fff; }
        .btn-off { background: #334155; color: #fff; }
        .btn-primary { background: #0284c7; color: #fff; }
        .btn-danger { background: var(--danger); color: #fff; }

        .relay-item {
            background: #0f172a;
            border-radius: 10px;
            padding: 10px 12px;
            margin-bottom: 8px;
            border: 2px solid var(--off-color);
            transition: all 0.2s ease;
        }
        .relay-item.active {
            border-color: var(--on-color);
            background: #064e3b18;
            box-shadow: 0 0 12px rgba(34, 197, 94, 0.2);
        }
        .relay-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; }
        .relay-title { font-weight: 600; font-size: 0.9rem; display: flex; align-items: center; gap: 8px; }
        .state-badge { font-size: 0.7rem; padding: 2px 8px; border-radius: 12px; font-weight: 700; }
        .state-on { background: #16a34a; color: #fff; }
        .state-off { background: #475569; color: #cbd5e1; }

        .config-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 8px; padding-top: 8px; border-top: 1px solid rgba(255,255,255,0.06); }
        .field label { font-size: 0.7rem; color: var(--text-dim); display: block; margin-bottom: 2px; }
        .input-sm { width: 100%; padding: 6px 8px; font-size: 0.8rem; background: #1e293b; color: #fff; border: 1px solid #334155; border-radius: 6px; }
        .reason-text { font-size: 0.73rem; color: var(--accent); margin-top: 6px; font-style: italic; }
        .btn-save { padding: 5px 12px; font-size: 0.78rem; background: #2563eb; color: #fff; border-radius: 6px; font-weight: 600; cursor: pointer; border: none; transition: 0.2s; }
        .btn-save:hover { background: #1d4ed8; }
        .btn-preset { padding: 4px 8px; font-size: 0.72rem; border-radius: 6px; border: none; font-weight: 700; cursor: pointer; transition: 0.2s; }
        .btn-preset-on { background: #059669; color: #fff; }
        .btn-preset-on:hover { background: #047857; }
        .btn-preset-off { background: #475569; color: #cbd5e1; }
        .btn-preset-off:hover { background: #334155; }

        .switch { position: relative; display: inline-block; width: 42px; height: 22px; }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: var(--off-color); transition: .25s; border-radius: 22px; }
        .slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 3px; bottom: 3px; background-color: white; transition: .25s; border-radius: 50%; }
        input:checked + .slider { background-color: var(--on-color); }
        input:checked + .slider:before { transform: translateX(20px); }

        .net-info { font-size: 0.8rem; color: var(--text-dim); display: flex; flex-direction: column; gap: 4px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>⚡ 16-Channel Climate Hub</h1>
            <p>ESP32-S3 | 2004 LCD + 4 Buttons | 16 Relays</p>
        </div>

        <div class="hero-grid" style="display:grid; grid-template-columns:repeat(auto-fit, minmax(210px, 1fr)); gap:10px;">
            <div class="sensor-card">
                <div class="sensor-label">🌡️ Avg Temperature</div>
                <div class="sensor-val" id="tempVal" style="color:#38bdf8;">--.- °C</div>
                <span id="tempStatus" style="font-size:0.75rem; color:var(--text-dim);">S1: -- | S2: -- | S3: --</span>
            </div>
            <div class="sensor-card">
                <div class="sensor-label">💧 Avg Humidity</div>
                <div class="sensor-val" id="humVal" style="color:#06b6d4;">--.- %</div>
                <span id="humStatus" style="font-size:0.75rem; color:var(--text-dim);">S1: -- | S2: -- | S3: --</span>
            </div>
            <div class="sensor-card">
                <div class="sensor-label">🌊 Water Flow (GPIO 46)</div>
                <div class="sensor-val" id="flowVal" style="color:#3b82f6;">0.0 L/m</div>
                <span id="flowStatus" style="font-size:0.75rem; color:var(--text-dim);">Total: 0.0 L | 0 Pulses</span>
            </div>
            <div class="sensor-card">
                <div class="sensor-label">☣️ Ammonia NH3 (GPIO 3/45)</div>
                <div class="sensor-val" id="nh3Val" style="color:#eab308;">0.0 ppm</div>
                <span id="nh3Status" style="font-size:0.75rem; color:var(--text-dim);">Status: Warming | 0.00V</span>
            </div>
        </div>

        <!-- Hardware Status -->
        <div class="card" style="padding:10px 14px;">
            <div style="font-size:0.82rem; display:flex; justify-content:space-between; align-items:center;">
                <span>📟 <b>LCD 2004 Status:</b> <span id="lcdDiag" style="color:var(--accent); font-weight:600;">Detecting...</span></span>
                <span style="font-size:0.72rem; color:var(--text-dim);">SDA: GPIO 17 | SCL: GPIO 18</span>
            </div>
        </div>

        <div class="card">
            <div class="btn-group" style="display:grid; grid-template-columns:1fr 1fr 1fr; gap:8px;">
                <button class="btn btn-primary" style="background:#2563eb;" onclick="setAllAuto()">🤖 ALL AUTO</button>
                <button class="btn btn-on" onclick="setAll(1)">⚡ ALL ON</button>
                <button class="btn btn-off" onclick="setAll(0)">⬛ ALL OFF</button>
            </div>
        </div>

        <div class="card">
            <div class="card-title">
                <span>⚡ Set Range for All 16 Relays</span>
                <span style="font-size:0.75rem; color:var(--text-dim)">ON if ≤ Min or ≥ Max | OFF Between</span>
            </div>
            <div style="display:grid; grid-template-columns:1fr 1fr auto; gap:8px; align-items:end;">
                <div>
                    <label style="font-size:0.72rem; color:var(--text-dim); display:block; margin-bottom:2px;">Min Temp (°C)</label>
                    <input type="number" step="0.5" id="batchMin" class="input-sm" value="27.0">
                </div>
                <div>
                    <label style="font-size:0.72rem; color:var(--text-dim); display:block; margin-bottom:2px;">Max Temp (°C)</label>
                    <input type="number" step="0.5" id="batchMax" class="input-sm" value="32.0">
                </div>
                <button class="btn btn-primary" onclick="applyBatchRange()" style="padding:7px 14px; font-size:0.82rem; height:34px;">🚀 Apply All</button>
            </div>
            <p id="batchMsg" style="font-size:0.75rem; color:#22c55e; margin-top:6px; font-weight:bold;"></p>
        </div>

        <div class="card">
            <div class="card-title">
                <span>Relay Automation & Thresholds</span>
                <span style="font-size:0.75rem; color:var(--text-dim)">Total: 16 Relays</span>
            </div>
            <div id="relayList"></div>
        </div>

        <!-- Sensors Calibration Panel -->
        <div class="card">
            <div class="card-title">
                <span>⚙️ Sensors Calibration & Offsets</span>
                <span style="font-size:0.75rem; color:var(--text-dim)">Calibrated = Raw + Offset</span>
            </div>
            
            <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap:10px;">
                <!-- DHT1 -->
                <div style="background:#0f172a; padding:10px; border-radius:8px; border:1px solid #334155;">
                    <div style="font-size:0.8rem; font-weight:700; color:var(--accent); margin-bottom:6px;">🌡️ Sensor #1 (GPIO 8)</div>
                    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:8px;">
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Temp Offset (°C)</label>
                            <input type="number" step="0.1" id="calT1" class="input-sm" value="0.0">
                            <span id="calT1Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Hum Offset (%)</label>
                            <input type="number" step="0.5" id="calH1" class="input-sm" value="0.0">
                            <span id="calH1Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                    </div>
                </div>

                <!-- DHT2 -->
                <div style="background:#0f172a; padding:10px; border-radius:8px; border:1px solid #334155;">
                    <div style="font-size:0.8rem; font-weight:700; color:var(--accent); margin-bottom:6px;">🌡️ Sensor #2 (GPIO 48)</div>
                    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:8px;">
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Temp Offset (°C)</label>
                            <input type="number" step="0.1" id="calT2" class="input-sm" value="0.0">
                            <span id="calT2Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Hum Offset (%)</label>
                            <input type="number" step="0.5" id="calH2" class="input-sm" value="0.0">
                            <span id="calH2Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                    </div>
                </div>

                <!-- DHT3 -->
                <div style="background:#0f172a; padding:10px; border-radius:8px; border:1px solid #334155;">
                    <div style="font-size:0.8rem; font-weight:700; color:var(--accent); margin-bottom:6px;">🌡️ Sensor #3 (GPIO 42)</div>
                    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:8px;">
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Temp Offset (°C)</label>
                            <input type="number" step="0.1" id="calT3" class="input-sm" value="0.0">
                            <span id="calT3Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Hum Offset (%)</label>
                            <input type="number" step="0.5" id="calH3" class="input-sm" value="0.0">
                            <span id="calH3Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                    </div>
                </div>

                <!-- Gas & Flow -->
                <div style="background:#0f172a; padding:10px; border-radius:8px; border:1px solid #334155;">
                    <div style="font-size:0.8rem; font-weight:700; color:var(--accent); margin-bottom:6px;">☣️ Gas & 🌊 Flow</div>
                    <div style="display:grid; grid-template-columns: 1fr 1fr; gap:8px;">
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">NH3 Multiplier (x)</label>
                            <input type="number" step="0.05" id="calNH3" class="input-sm" value="1.00">
                            <span id="calNH3Live" style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Raw: --</span>
                        </div>
                        <div>
                            <label style="font-size:0.7rem; color:var(--text-dim); display:block; margin-bottom:2px;">Flow Factor (pulses/s)</label>
                            <input type="number" step="0.1" id="calFlow" class="input-sm" value="7.5">
                            <span style="font-size:0.7rem; color:#94a3b8; display:block; margin-top:2px;">Std: 7.5</span>
                        </div>
                    </div>
                </div>
            </div>

            <div style="display:flex; justify-content:space-between; align-items:center; margin-top:12px; gap:8px; flex-wrap:wrap;">
                <button class="btn btn-off" style="padding:7px 12px; font-size:0.78rem;" onclick="resetCalibration()">🔄 Reset to 0</button>
                <div style="display:flex; align-items:center; gap:8px;">
                    <span id="calibMsg" style="font-size:0.78rem; color:#22c55e; font-weight:bold;"></span>
                    <button class="btn btn-primary" id="btnSaveCalib" style="padding:7px 16px; font-size:0.82rem;" onclick="saveCalibration()">💾 Save Calibration</button>
                </div>
            </div>
        </div>

        <div class="card">
            <div class="card-title">
                <span>📡 Network Status</span>
                <button class="btn btn-primary" style="padding: 4px 8px; font-size: 0.75rem;" onclick="scanWiFi()">🔍 Scan</button>
            </div>
            <div class="net-info">
                <div>Direct Hotspot IP: <b style="color:#fff">192.168.4.1</b></div>
                <div>Router WiFi Status: <span id="staStatus" style="color:var(--accent)">Checking...</span></div>
                <div>Router IP: <span id="staIP" style="color:#22c55e; font-weight:bold;">-</span></div>
            </div>
            <div id="scanResult" style="max-height:100px; overflow-y:auto; margin-top:8px; display:none;"></div>
            <div style="margin-top:10px; display:grid; grid-template-columns:1fr 1fr; gap:8px;">
                <input type="text" id="wifi_ssid" class="input-sm" placeholder="WiFi SSID">
                <input type="password" id="wifi_pass" class="input-sm" placeholder="Password">
            </div>
            <div style="margin-top:8px; display:flex; gap:8px;">
                <button class="btn btn-primary" style="flex:1" onclick="saveWiFi()">💾 Connect</button>
                <button class="btn btn-danger" style="padding:6px 12px; font-size:0.8rem;" onclick="resetWiFi()">Forget</button>
            </div>
            <p id="wifiMsg" style="font-size:0.75rem; color:var(--accent); margin-top:6px; text-align:center;"></p>
        </div>
    </div>

    <script>
        const PINS = [4, 5, 6, 7, 15, 16, 21, 47, 1, 2, 9, 10, 11, 12, 13, 14];
        const container = document.getElementById('relayList');

        for(let i = 1; i <= 16; i++) {
            if(i === 1) container.innerHTML += `<div class="mod-header">📦 Module 1 (8-ch Relay Board: R1 - R8)</div>`;
            if(i === 9) container.innerHTML += `<div class="mod-header">📦 Module 2 (8-ch Relay Board: R9 - R16)</div>`;

            const card = document.createElement('div');
            card.className = 'relay-item';
            card.id = 'rCard' + i;
            card.innerHTML = `
                <div class="relay-header">
                    <div class="relay-title">
                        <span id="rTitleText${i}">Relay ${i}</span>
                        <span style="font-size:0.72rem; color:var(--text-dim)">(GPIO ${PINS[i-1]})</span>
                    </div>
                    <div style="display:flex; align-items:center; gap:8px;">
                        <span class="state-badge state-off" id="rBadge${i}">OFF</span>
                        <label class="switch">
                            <input type="checkbox" id="rSwitch${i}" onchange="toggleRelay(${i}, this.checked)">
                            <span class="slider"></span>
                        </label>
                    </div>
                </div>

                <div style="display:flex; align-items:center; gap:8px; margin: 4px 0 8px 0;">
                    <label style="font-size:0.75rem; color:var(--text-dim); font-weight:600; min-width:48px;">🏷️ Name:</label>
                    <input type="text" id="rName${i}" class="input-sm" maxlength="15" placeholder="e.g. Exhaust Fan ${i}" style="font-weight:600; color:var(--accent);">
                </div>

                <div class="config-grid">
                    <div class="field">
                        <label>📡 Sensor Source</label>
                        <select id="rSensor${i}" class="input-sm" onchange="updateSensorUnits(${i})">
                            <option value="1">🌡️ Avg Temperature (°C)</option>
                            <option value="2">💧 Avg Humidity (%)</option>
                            <option value="3">🌡️ Sensor 1 Temp (°C)</option>
                            <option value="4">💧 Sensor 1 Hum (%)</option>
                            <option value="5">🌡️ Sensor 2 Temp (°C)</option>
                            <option value="6">💧 Sensor 2 Hum (%)</option>
                            <option value="7">🌡️ Sensor 3 Temp (°C)</option>
                            <option value="8">💧 Sensor 3 Hum (%)</option>
                            <option value="9">☣️ Ammonia NH3 (ppm)</option>
                            <option value="10">🌊 Water Flow Rate (L/min)</option>
                            <option value="11">⏱️ Pure Cyclic Timer (No Sensor)</option>
                            <option value="0">✋ None (Manual Control)</option>
                        </select>
                    </div>
                    <div class="field">
                        <label>⚙️ Control Mode</label>
                        <select id="rMode${i}" class="input-sm" onchange="updateModeUnits(${i})">
                            <option value="0">📊 Sensor Threshold Only</option>
                            <option value="1">⏱️ Timer + Sensor Override</option>
                            <option value="2">⏱️ Pure Cyclic Timer</option>
                            <option value="3">✋ Manual Control</option>
                        </select>
                    </div>
                    <div class="field">
                        <label>⚡ Action</label>
                        <select id="rAction${i}" class="input-sm">
                            <option value="0">⚡ Outside Range ON (≤Min or ≥Max)</option>
                            <option value="1">🟢 Inside Range ON (Min to Max)</option>
                            <option value="2">❄️ Above Max ON (Cooling/Exhaust)</option>
                            <option value="3">🔥 Below Min ON (Heating/Scrub)</option>
                        </select>
                    </div>
                    <div class="field">
                        <label id="rMaxLbl${i}">🔥 Max Threshold [ON if ≥]</label>
                        <input type="number" step="0.5" id="rMax${i}" class="input-sm">
                    </div>
                    <div class="field">
                        <label id="rMinLbl${i}">❄️ Min Threshold [ON if ≤]</label>
                        <input type="number" step="0.5" id="rMin${i}" class="input-sm">
                    </div>
                    <div class="field" id="rTimerBox${i}" style="grid-column: 1 / -1; display: grid; grid-template-columns: 1fr 1fr; gap: 8px; background: rgba(56,189,248,0.06); padding: 8px; border-radius: 8px; border: 1px dashed rgba(56,189,248,0.3); margin-top: 4px;">
                        <div>
                            <label style="font-size:0.72rem; color:var(--accent); font-weight:600;">⏱️ Run Time (Sec)</label>
                            <input type="number" id="rTon${i}" class="input-sm" min="5" max="3600" value="30">
                        </div>
                        <div>
                            <label style="font-size:0.72rem; color:var(--accent); font-weight:600;">⏸️ Pause Time (Sec)</label>
                            <input type="number" id="rToff${i}" class="input-sm" min="5" max="3600" value="120">
                        </div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; flex-wrap:wrap; gap:6px;">
                    <div style="display:flex; gap:6px;">
                        <button class="btn-preset btn-preset-on" title="Auto-set thresholds to turn ON now" onclick="quickSet(${i}, true)">🎯 Set ON</button>
                        <button class="btn-preset btn-preset-off" title="Auto-set thresholds to turn OFF now" onclick="quickSet(${i}, false)">⭕ Set OFF</button>
                    </div>
                    <button class="btn-save" id="rSaveBtn${i}" onclick="saveConfig(${i})">💾 Save</button>
                </div>
                <div class="reason-text" id="rReason${i}">--</div>
            `;
            container.appendChild(card);
        }

        let liveSensorTemp = 30.0;
        let liveSensorHum = 60.0;
        let liveSensorFlow = 0.0;
        let liveSensorNH3 = 0.0;

        function updateUI(d) {
            if(d.sensorOk) {
                liveSensorTemp = parseFloat(d.temp);
                liveSensorHum = parseFloat(d.hum);
                document.getElementById('tempVal').innerText = d.temp + ' °C';
                document.getElementById('humVal').innerText = d.hum + ' %';
                document.getElementById('tempStatus').innerText = `S1: ${d.temp1}° | S2: ${d.temp2}° | S3: ${d.temp3}° (Avg: ${d.temp}°)`;
                document.getElementById('humStatus').innerText = `S1: ${d.hum1}% | S2: ${d.hum2}% | S3: ${d.hum3}% (Avg: ${d.hum}%)`;
            } else {
                document.getElementById('tempVal').innerText = '--.- °C';
                document.getElementById('humVal').innerText = '--.- %';
                document.getElementById('tempStatus').innerText = 'Sensor error (Check GPIO 8, 48, 42)';
                document.getElementById('humStatus').innerText = 'Sensor error (Check GPIO 8, 48, 42)';
            }

            if(d.flowRate !== undefined) {
                liveSensorFlow = parseFloat(d.flowRate);
                document.getElementById('flowVal').innerText = d.flowRate + ' L/m';
                document.getElementById('flowStatus').innerText = `Total: ${d.totalLiters} L | ${d.flowPulses} Pulses`;
            }

            if(d.nh3 !== undefined) {
                liveSensorNH3 = parseFloat(d.nh3);
                document.getElementById('nh3Val').innerText = d.nh3 + ' ppm';
                const elNh3 = document.getElementById('nh3Status');
                if(!d.nh3Warmed) {
                    elNh3.innerText = `Status: Warming Up | ${d.nh3V}V`;
                    elNh3.style.color = '#eab308';
                } else if(d.nh3Alert) {
                    elNh3.innerText = `Status: ⚠️ GAS ALERT! | ${d.nh3V}V`;
                    elNh3.style.color = '#ef4444';
                } else {
                    elNh3.innerText = `Status: ✅ Clean/Normal | ${d.nh3V}V`;
                    elNh3.style.color = '#22c55e';
                }
            }

            if(d.lcdStatus) {
                const el = document.getElementById('lcdDiag');
                el.innerText = d.lcdStatus;
                if(d.lcdStatus.includes('Connected')) {
                    el.style.color = '#22c55e';
                } else {
                    el.style.color = '#ef4444';
                }
            }

            d.relays.forEach(r => {
                const card = document.getElementById('rCard' + r.id);
                const badge = document.getElementById('rBadge' + r.id);
                const sw = document.getElementById('rSwitch' + r.id);
                const reason = document.getElementById('rReason' + r.id);
                const titleText = document.getElementById('rTitleText' + r.id);

                if(card && badge && sw) {
                    if(r.state === 1) {
                        card.classList.add('active');
                        badge.className = 'state-badge state-on';
                        badge.innerText = 'ON';
                        sw.checked = true;
                    } else {
                        card.classList.remove('active');
                        badge.className = 'state-badge state-off';
                        badge.innerText = 'OFF';
                        sw.checked = false;
                    }
                    if(reason) reason.innerText = r.reason || '';
                    if(titleText) titleText.innerText = (r.name && r.name.length > 0) ? r.name : ('Relay ' + r.id);

                    // NEVER overwrite user inputs while user is typing / active in that field!
                    const nameEl = document.getElementById('rName' + r.id);
                    const sensorEl = document.getElementById('rSensor' + r.id);
                    const modeEl = document.getElementById('rMode' + r.id);
                    const actionEl = document.getElementById('rAction' + r.id);
                    const minEl = document.getElementById('rMin' + r.id);
                    const maxEl = document.getElementById('rMax' + r.id);
                    const tonEl = document.getElementById('rTon' + r.id);
                    const toffEl = document.getElementById('rToff' + r.id);

                    if(nameEl && document.activeElement !== nameEl) nameEl.value = r.name || '';
                    if(sensorEl && document.activeElement !== sensorEl) {
                        sensorEl.value = (r.sensor !== undefined) ? r.sensor : 1;
                        updateSensorUnits(r.id);
                    }
                    if(modeEl && document.activeElement !== modeEl) {
                        modeEl.value = r.mode;
                        updateModeUnits(r.id);
                    }
                    if(actionEl && document.activeElement !== actionEl) actionEl.value = r.action;
                    if(minEl && document.activeElement !== minEl) minEl.value = r.min;
                    if(maxEl && document.activeElement !== maxEl) maxEl.value = r.max;
                    if(tonEl && document.activeElement !== tonEl) tonEl.value = r.ton || 30;
                    if(toffEl && document.activeElement !== toffEl) toffEl.value = r.toff || 120;
                }
            });

            if(d.calib) {
                const c = d.calib;
                const calT1 = document.getElementById('calT1');
                const calH1 = document.getElementById('calH1');
                const calT2 = document.getElementById('calT2');
                const calH2 = document.getElementById('calH2');
                const calT3 = document.getElementById('calT3');
                const calH3 = document.getElementById('calH3');
                const calNH3 = document.getElementById('calNH3');
                const calFlow = document.getElementById('calFlow');

                if(calT1 && document.activeElement !== calT1) calT1.value = c.t1;
                if(calH1 && document.activeElement !== calH1) calH1.value = c.h1;
                if(calT2 && document.activeElement !== calT2) calT2.value = c.t2;
                if(calH2 && document.activeElement !== calH2) calH2.value = c.h2;
                if(calT3 && document.activeElement !== calT3) calT3.value = c.t3;
                if(calH3 && document.activeElement !== calH3) calH3.value = c.h3;
                if(calNH3 && document.activeElement !== calNH3) calNH3.value = c.nh3;
                if(calFlow && document.activeElement !== calFlow) calFlow.value = c.flow;

                const lT1 = document.getElementById('calT1Live'); if(lT1) lT1.innerText = `Raw: ${c.rawT1}°C`;
                const lH1 = document.getElementById('calH1Live'); if(lH1) lH1.innerText = `Raw: ${c.rawH1}%`;
                const lT2 = document.getElementById('calT2Live'); if(lT2) lT2.innerText = `Raw: ${c.rawT2}°C`;
                const lH2 = document.getElementById('calH2Live'); if(lH2) lH2.innerText = `Raw: ${c.rawH2}%`;
                const lT3 = document.getElementById('calT3Live'); if(lT3) lT3.innerText = `Raw: ${c.rawT3}°C`;
                const lH3 = document.getElementById('calH3Live'); if(lH3) lH3.innerText = `Raw: ${c.rawH3}%`;
                const lNH3 = document.getElementById('calNH3Live'); if(lNH3) lNH3.innerText = `Raw: ${c.rawNH3} ppm`;
            }

            document.getElementById('staStatus').innerText = d.staStatus;
            document.getElementById('staIP').innerText = d.staIP;
        }

        function updateSensorUnits(id) {
            const sEl = document.getElementById('rSensor' + id);
            if(!sEl) return;
            const s = parseInt(sEl.value);
            let u = ' (°C)';
            let step = '0.5';
            if(s === 2 || s === 4 || s === 6 || s === 8) { u = ' (%)'; step = '1.0'; }
            else if(s === 9) { u = ' (ppm)'; step = '1.0'; }
            else if(s === 10) { u = ' (L/min)'; step = '0.5'; }
            else if(s === 11 || s === 0) { u = ''; }

            const minEl = document.getElementById('rMin' + id);
            const maxEl = document.getElementById('rMax' + id);
            if(minEl) minEl.step = step;
            if(maxEl) maxEl.step = step;
            const maxLbl = document.getElementById('rMaxLbl' + id);
            const minLbl = document.getElementById('rMinLbl' + id);
            if(maxLbl) maxLbl.innerText = '🔥 Max Threshold' + u + ' [ON if ≥]';
            if(minLbl) minLbl.innerText = '❄️ Min Threshold' + u + ' [ON if ≤]';
        }

        function updateModeUnits(id) {
            const mEl = document.getElementById('rMode' + id);
            if(!mEl) return;
            const m = parseInt(mEl.value);
            const tBox = document.getElementById('rTimerBox' + id);
            if(tBox) {
                if(m === 1 || m === 2) {
                    tBox.style.opacity = '1.0';
                    tBox.style.border = '1px solid #38bdf8';
                } else {
                    tBox.style.opacity = '0.45';
                    tBox.style.border = '1px dashed #334155';
                }
            }
        }

        function fetchStatus() {
            fetch('/status')
                .then(r => r.json())
                .then(d => updateUI(d))
                .catch(e => console.log(e));
        }

        function toggleRelay(id, state) {
            const card = document.getElementById('rCard' + id);
            const badge = document.getElementById('rBadge' + id);
            const sw = document.getElementById('rSwitch' + id);
            const reason = document.getElementById('rReason' + id);
            if(card && badge && sw) {
                if(state) {
                    card.classList.add('active');
                    badge.className = 'state-badge state-on';
                    badge.innerText = 'ON';
                    sw.checked = true;
                } else {
                    card.classList.remove('active');
                    badge.className = 'state-badge state-off';
                    badge.innerText = 'OFF';
                    sw.checked = false;
                }
                if(reason) reason.innerText = 'Manual Web Toggle';
            }
            fetch(`/toggle?id=${id}&state=${state ? 1 : 0}`).then(fetchStatus);
        }

        function setAllAuto() {
            fetch('/allauto').then(fetchStatus);
        }

        function setAll(state) {
            for(let i = 1; i <= 16; i++) {
                const card = document.getElementById('rCard' + i);
                const badge = document.getElementById('rBadge' + i);
                const sw = document.getElementById('rSwitch' + i);
                const reason = document.getElementById('rReason' + i);
                if(card && badge && sw) {
                    if(state === 1) {
                        card.classList.add('active');
                        badge.className = 'state-badge state-on';
                        badge.innerText = 'ON';
                        sw.checked = true;
                    } else {
                        card.classList.remove('active');
                        badge.className = 'state-badge state-off';
                        badge.innerText = 'OFF';
                        sw.checked = false;
                    }
                    if(reason) reason.innerText = (state === 1) ? 'Manual Master ON' : 'Manual Master OFF';
                }
            }
            fetch(`/all?state=${state}`).then(fetchStatus);
        }

        function quickSet(id, turnOn) {
            const sEl = document.getElementById('rSensor' + id);
            let s = sEl ? parseInt(sEl.value) : 1;
            let curVal = liveSensorTemp || 30.0;
            if(s === 2 || s === 4 || s === 6 || s === 8) curVal = liveSensorHum || 60.0;
            else if(s === 9) curVal = liveSensorNH3 || 10.0;
            else if(s === 10) curVal = liveSensorFlow || 1.0;

            const minEl = document.getElementById('rMin' + id);
            const maxEl = document.getElementById('rMax' + id);
            const actEl = document.getElementById('rAction' + id);
            if(actEl) actEl.value = '0'; // Outside Range ON

            if(turnOn) {
                if(maxEl) maxEl.value = (curVal - 0.5).toFixed(1);
                if(minEl) minEl.value = (curVal - 3.0).toFixed(1);
            } else {
                if(minEl) minEl.value = (curVal - 2.0).toFixed(1);
                if(maxEl) maxEl.value = (curVal + 2.0).toFixed(1);
            }
            saveConfig(id);
        }

        function saveConfig(id) {
            const btn = document.getElementById('rSaveBtn' + id);
            if(btn) { btn.innerText = '⏳ Saving...'; btn.style.background = '#eab308'; }

            const name = document.getElementById('rName' + id) ? document.getElementById('rName' + id).value : '';
            const sensor = document.getElementById('rSensor' + id) ? document.getElementById('rSensor' + id).value : '1';
            const mode = document.getElementById('rMode' + id).value;
            const action = document.getElementById('rAction' + id).value;
            const minVal = document.getElementById('rMin' + id).value;
            const maxVal = document.getElementById('rMax' + id).value;
            const tonVal = document.getElementById('rTon' + id) ? document.getElementById('rTon' + id).value : '30';
            const toffVal = document.getElementById('rToff' + id) ? document.getElementById('rToff' + id).value : '120';

            const body = new URLSearchParams();
            body.append('id', id);
            body.append('name', name);
            body.append('sensor', sensor);
            body.append('mode', mode);
            body.append('action', action);
            body.append('min', minVal);
            body.append('max', maxVal);
            body.append('ton', tonVal);
            body.append('toff', toffVal);

            fetch('/saveconfig', { method: 'POST', body: body })
                .then(r => r.text())
                .then(() => {
                    if(btn) {
                        btn.innerText = '✅ Saved!';
                        btn.style.background = '#16a34a';
                        setTimeout(() => {
                            btn.innerText = '💾 Save';
                            btn.style.background = '#2563eb';
                        }, 1200);
                    }
                    fetchStatus();
                })
                .catch(() => {
                    if(btn) { btn.innerText = '❌ Error'; btn.style.background = '#ef4444'; }
                });
        }

        function applyBatchRange() {
            const min = document.getElementById('batchMin').value;
            const max = document.getElementById('batchMax').value;
            const msg = document.getElementById('batchMsg');
            if(!min || !max) { alert('Enter Min and Max'); return; }
            msg.innerText = 'Applying to all 16 relays...';

            const body = new URLSearchParams();
            body.append('min', min);
            body.append('max', max);

            fetch('/batchrange', { method: 'POST', body: body })
                .then(r => r.text())
                .then(t => {
                    msg.innerText = '✅ ' + t + '! Relays updating live...';
                    setTimeout(() => { msg.innerText = ''; }, 3000);
                    fetchStatus();
                });
        }

        function scanWiFi() {
            const div = document.getElementById('scanResult');
            div.style.display = 'block';
            div.innerHTML = '<div style="padding:6px; color:var(--text-dim)">Scanning...</div>';
            fetch('/scan')
                .then(r => r.json())
                .then(list => {
                    div.innerHTML = '';
                    if(!list.length) { div.innerHTML = '<div style="padding:6px">No networks found</div>'; return; }
                    list.forEach(n => {
                        const item = document.createElement('div');
                        item.style = 'padding:6px; font-size:0.8rem; cursor:pointer; border-bottom:1px solid #1e293b;';
                        item.innerHTML = `${n.ssid} (${n.rssi} dBm)`;
                        item.onclick = () => {
                            document.getElementById('wifi_ssid').value = n.ssid;
                            div.style.display = 'none';
                        };
                        div.appendChild(item);
                    });
                });
        }

        function saveWiFi() {
            const s = document.getElementById('wifi_ssid').value.trim();
            const p = document.getElementById('wifi_pass').value;
            if(!s) { alert('Enter SSID'); return; }
            const msg = document.getElementById('wifiMsg');
            msg.innerText = 'Connecting...';

            const body = new URLSearchParams();
            body.append('ssid', s);
            body.append('pass', p);

            fetch('/savewifi', { method: 'POST', body: body })
                .then(r => r.text())
                .then(t => {
                    msg.innerText = t;
                    alert('Saved! ESP32 restarting...');
                });
        }

        function resetWiFi() {
            if(!confirm('Clear WiFi router credentials?')) return;
            fetch('/resetwifi', { method: 'POST' }).then(() => {
                alert('WiFi cleared.');
                fetchStatus();
            });
        }

        function saveCalibration() {
            const btn = document.getElementById('btnSaveCalib');
            const msg = document.getElementById('calibMsg');
            if(btn) { btn.innerText = '⏳ Saving...'; btn.style.background = '#eab308'; }

            const body = new URLSearchParams();
            body.append('t1', document.getElementById('calT1').value);
            body.append('h1', document.getElementById('calH1').value);
            body.append('t2', document.getElementById('calT2').value);
            body.append('h2', document.getElementById('calH2').value);
            body.append('t3', document.getElementById('calT3').value);
            body.append('h3', document.getElementById('calH3').value);
            body.append('nh3', document.getElementById('calNH3').value);
            body.append('flow', document.getElementById('calFlow').value);

            fetch('/savecalib', { method: 'POST', body: body })
                .then(r => r.text())
                .then(t => {
                    if(btn) {
                        btn.innerText = '✅ Saved!';
                        btn.style.background = '#16a34a';
                        setTimeout(() => {
                            btn.innerText = '💾 Save Calibration';
                            btn.style.background = '#2563eb';
                        }, 1200);
                    }
                    if(msg) {
                        msg.innerText = 'Calibration Saved!';
                        setTimeout(() => { msg.innerText = ''; }, 3000);
                    }
                    fetchStatus();
                })
                .catch(() => {
                    if(btn) { btn.innerText = '❌ Error'; btn.style.background = '#ef4444'; }
                });
        }

        function resetCalibration() {
            if(!confirm('Reset all sensor offsets to 0.0 and factor to 1.0?')) return;
            document.getElementById('calT1').value = '0.0';
            document.getElementById('calH1').value = '0.0';
            document.getElementById('calT2').value = '0.0';
            document.getElementById('calH2').value = '0.0';
            document.getElementById('calT3').value = '0.0';
            document.getElementById('calH3').value = '0.0';
            document.getElementById('calNH3').value = '1.0';
            document.getElementById('calFlow').value = '7.5';
            saveCalibration();
        }

        fetchStatus();
        setInterval(fetchStatus, 2000);
    </script>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
}
