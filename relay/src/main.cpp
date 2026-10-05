/**
 * ============================================================================
 * ESP32-S3 16-Channel Climate Hub with 2004 I2C LCD & 4 Physical Buttons
 * (Includes Full-Range Auto I2C Scanner & Web Status)
 * ============================================================================
 * 
 * Hardware:
 * - ESP32-S3 DevKitC-1
 * - Module 1 (8-ch Relay): GPIO 4, 5, 6, 7, 15, 16, 21, 47
 * - Module 2 (4-ch Relay): GPIO 1, 2, 9, 10
 * - Module 3 (4-ch Relay): GPIO 11, 12, 13, 14
 * - DHT11 Sensors (x3 Multi-Zone / Average):
 *   Sensor 1 (T1): GPIO 8
 *   Sensor 2 (T2): GPIO 3
 *   Sensor 3 (T3): GPIO 42
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
    4, 5, 6, 7, 15, 16, 21, 47,    // Module 1 (Relays 1-8: R7 on GPIO 21, R8 on GPIO 47)
    1, 2, 9, 10,                   // Module 2 (Relays 9-12)
    11, 12, 13, 14                 // Module 3 (Relays 13-16)
};

// 3x DHT11 Sensors (T1 on GPIO 8, T2 on GPIO 3, T3 on GPIO 42)
#define DHTPIN1 8
#define DHTPIN2 3
#define DHTPIN3 42
#define DHTTYPE DHT11

DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);
DHT dht3(DHTPIN3, DHTTYPE);

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
    int mode;        // 0: Manual, 1: Auto Temp, 2: Auto Hum
    float minVal;    // Min threshold
    float maxVal;    // Max threshold
    int action;      // 0: Above Max ON, 1: Below Min ON, 2: Inside Range ON
};

RelayConfig configs[NUM_RELAYS];
bool relayStates[NUM_RELAYS] = { false };
String relayReason[NUM_RELAYS];

// Sensor readings (3x DHT11 & Averages)
float temp1 = 0.0, temp2 = 0.0, temp3 = 0.0;
float hum1  = 0.0, hum2  = 0.0, hum3  = 0.0;
bool valid1 = false, valid2 = false, valid3 = false;
float avgTemp = 0.0;
float avgHum  = 0.0;
float currentTemp = 0.0; // Relay decisions are made on currentTemp = avgTemp
float currentHum  = 0.0; // Relay decisions are made on currentHum = avgHum
bool sensorValid = false;

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

    // Initialize I2C & LCD with Auto Scanner
    initLCD();

    // Start 3x DHT11 Sensors (GPIO 8, 3, 42)
    pinMode(DHTPIN1, INPUT_PULLUP);
    pinMode(DHTPIN2, INPUT_PULLUP);
    pinMode(DHTPIN3, INPUT_PULLUP);
    dht1.begin();
    dht2.begin();
    dht3.begin();

    // Load Relay Thresholds
    loadRelayConfigs();

    delay(1500);
    readDHTSensor();
    evaluateAutoRules();
    Serial.println("\n\n========================================================");
    Serial.println("  ⚡ ESP32-S3 16-RELAY SMART CLIMATE HUB (LCD + BUTTONS)");
    Serial.println("========================================================");

    // WiFi Setup
    prefWifi.begin("wifi-config", false);
    saved_ssid = prefWifi.getString("ssid", "");
    saved_pass = prefWifi.getString("pass", "");

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);

    if (saved_ssid.length() > 0) {
        sta_status_str = "Connecting...";
        WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
        int count = 0;
        while (WiFi.status() != WL_CONNECTED && count < 15) {
            delay(400);
            count++;
        }
        if (WiFi.status() == WL_CONNECTED) {
            sta_status_str = "Connected";
        } else {
            sta_status_str = "Not Connected";
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

    server.begin();
    Serial.println("🚀 Web Server started successfully!");
}

void loop() {
    server.handleClient();

    // Fast, non-blocking check for LCD (0x27 or 0x3F) every 4 seconds if not yet connected
    static unsigned long lastI2CRetry = 0;
    if (!lcdAvailable && millis() - lastI2CRetry > 4000) {
        lastI2CRetry = millis();
        initLCD();
    }

    // Read Sensor & Evaluate Rules every 2 seconds
    static unsigned long lastSensor = 0;
    if (millis() - lastSensor > 2000) {
        lastSensor = millis();
        readDHTSensor();
        evaluateAutoRules();
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
    delay(100);
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);
    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin(I2C_SDA, I2C_SCL, 50000); // 50kHz for rock-solid stability over jumper wires
    Wire.setTimeOut(50);

    byte lcdAddr = 0;
    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() == 0) {
        lcdAddr = 0x27;
    } else {
        Wire.beginTransmission(0x3F);
        if (Wire.endTransmission() == 0) lcdAddr = 0x3F;
    }

    if (lcdAddr == 0) {
        for (byte a = 0x20; a <= 0x3F; a++) {
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
        Wire.begin(I2C_SDA, I2C_SCL, 50000); // CRITICAL: Re-assert pins because library init() calls Wire.begin() with no args!
        pLcd->backlight();
        pLcd->display();
        pLcd->clear();

        pLcd->setCursor(0, 0);
        pLcd->print("====================");
        pLcd->setCursor(0, 1);
        pLcd->print("  16-CH CLIMATE HUB ");
        pLcd->setCursor(0, 2);
        pLcd->print("  ESP32-S3 + RELAYS ");
        pLcd->setCursor(0, 3);
        pLcd->print("====================");

        lcdAvailable = true;
        delay(1500);
        pLcd->clear();
    } else {
        lcdStatusStr = "Not Detected (Check SDA 17, SCL 18)";
        lcdAvailable = false;
    }
}

// --- BUTTONS LOGIC & NAVIGATION ---

bool checkButton(Button &btn) {
    int reading = digitalRead(btn.pin);
    bool triggered = false;

    if (reading != btn.lastState) {
        btn.lastDebounceTime = millis();
    }

    if ((millis() - btn.lastDebounceTime) > 35) {
        // Initial Press
        if (reading == LOW && !btn.pressed) {
            btn.pressed = true;
            btn.pressStartTime = millis();
            btn.lastRepeatTime = millis();
            triggered = true;
            Serial.printf("🔘 Button [%s] PRESSED (GPIO %d) -> Menu State: %d\n", btn.name, btn.pin, currentMenu);
        }
        // Auto-repeat when held down (after 400ms delay, repeat every 120ms)
        else if (reading == LOW && btn.pressed) {
            if ((millis() - btn.pressStartTime > 400) && (millis() - btn.lastRepeatTime > 120)) {
                btn.lastRepeatTime = millis();
                triggered = true;
            }
        }
        // Button Released
        else if (reading == HIGH && btn.pressed) {
            btn.pressed = false;
        }
    }

    btn.lastState = reading;
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
                relayMenuIndex = (relayMenuIndex - 1 + 3) % 3;
                updateLCD();
            } else if (bDown) {
                relayMenuIndex = (relayMenuIndex + 1) % 3;
                updateLCD();
            } else if (bBack) {
                currentMenu = STATE_SELECT_RELAY;
                if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                updateLCD();
            } else if (bMenu) {
                if (relayMenuIndex == 0) {
                    // Toggle Mode between Auto Temp (1) and Auto Hum (2)
                    configs[selectedRelay].mode = (configs[selectedRelay].mode == 1) ? 2 : 1;
                    if (configs[selectedRelay].mode == 2 && configs[selectedRelay].maxVal <= 60.0 && configs[selectedRelay].minVal <= 40.0) {
                        configs[selectedRelay].minVal = globalHumMin;
                        configs[selectedRelay].maxVal = globalHumMax;
                    } else if (configs[selectedRelay].mode == 1 && configs[selectedRelay].maxVal > 60.0) {
                        configs[selectedRelay].minVal = globalTempMin;
                        configs[selectedRelay].maxVal = globalTempMax;
                    }
                    saveRelayConfig(selectedRelay);
                    readDHTSensor();
                    evaluateSingleRelay(selectedRelay, true);

                    if (lcdAvailable && pLcd != nullptr) {
                        pLcd->clear();
                        pLcd->setCursor(3, 1);
                        pLcd->print("Mode Updated!");
                        pLcd->setCursor(1, 2);
                        pLcd->print(configs[selectedRelay].mode == 2 ? "-> Auto Humidity" : "-> Auto Temperature");
                        delay(600);
                        pLcd->clear();
                    }
                    updateLCD();
                } else {
                    currentMenu = STATE_EDIT_VALUE;
                    selectedTarget = (relayMenuIndex == 1) ? 0 : 1;
                    editingVal = (selectedTarget == 0) ? configs[selectedRelay].minVal : configs[selectedRelay].maxVal;
                    if (lcdAvailable && pLcd != nullptr) pLcd->clear();
                    updateLCD();
                }
            }
            break;

        case STATE_EDIT_VALUE:
            if (bUp) {
                if (configs[selectedRelay].mode == 2) {
                    editingVal += 1.0;
                    if (editingVal > 99.0) editingVal = 99.0;
                } else {
                    editingVal += 0.5;
                    if (editingVal > 60.0) editingVal = 60.0;
                }
                updateLCD();
            } else if (bDown) {
                if (configs[selectedRelay].mode == 2) {
                    editingVal -= 1.0;
                    if (editingVal < 10.0) editingVal = 10.0;
                } else {
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
                } else {
                    configs[selectedRelay].maxVal = editingVal;
                }
                if (configs[selectedRelay].minVal > configs[selectedRelay].maxVal) {
                    float t = configs[selectedRelay].minVal;
                    configs[selectedRelay].minVal = configs[selectedRelay].maxVal;
                    configs[selectedRelay].maxVal = t;
                }
                saveRelayConfig(selectedRelay);
                readDHTSensor();
                evaluateSingleRelay(selectedRelay, true);
                Serial.printf("⚡ LCD Saved Relay %d: Mode=%d, Min=%.1f, Max=%.1f -> State: %s (%s)\n",
                    selectedRelay + 1, configs[selectedRelay].mode, configs[selectedRelay].minVal, configs[selectedRelay].maxVal,
                    relayStates[selectedRelay] ? "ON" : "OFF", relayReason[selectedRelay].c_str());

                if (lcdAvailable && pLcd != nullptr) {
                    pLcd->clear();
                    pLcd->setCursor(3, 1);
                    pLcd->print("*** SAVED! ***");
                    pLcd->setCursor(1, 2);
                    char sb[21];
                    char u = (configs[selectedRelay].mode == 2) ? '%' : 'C';
                    snprintf(sb, sizeof(sb), "Mn:%4.1f%c  Mx:%4.1f%c", configs[selectedRelay].minVal, u, configs[selectedRelay].maxVal, u);
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
    pLcd->setCursor(0, 0);
    pLcd->print("====================");

    pLcd->setCursor(0, 1);
    if (sensorValid) {
        char s1[4], s2[4], s3[4], sAvg[4];
        if (valid1) snprintf(s1, sizeof(s1), "%2.0f", temp1); else strcpy(s1, "--");
        if (valid2) snprintf(s2, sizeof(s2), "%2.0f", temp2); else strcpy(s2, "--");
        if (valid3) snprintf(s3, sizeof(s3), "%2.0f", temp3); else strcpy(s3, "--");
        snprintf(sAvg, sizeof(sAvg), "%2.0f", avgTemp);
        char buf[21];
        snprintf(buf, sizeof(buf), "T: %s, %s, %s Avg.%s", s1, s2, s3, sAvg);
        pLcd->print(buf);
    } else {
        pLcd->print("T: --, --, -- Avg.--");
    }

    pLcd->setCursor(0, 2);
    if (sensorValid) {
        char h1s[4], h2s[4], h3s[4], hAvgs[4];
        if (valid1) snprintf(h1s, sizeof(h1s), "%2.0f", hum1); else strcpy(h1s, "--");
        if (valid2) snprintf(h2s, sizeof(h2s), "%2.0f", hum2); else strcpy(h2s, "--");
        if (valid3) snprintf(h3s, sizeof(h3s), "%2.0f", hum3); else strcpy(h3s, "--");
        snprintf(hAvgs, sizeof(hAvgs), "%2.0f", avgHum);
        char buf[21];
        snprintf(buf, sizeof(buf), "H: %s, %s, %s Avg.%s", h1s, h2s, h3s, hAvgs);
        pLcd->print(buf);
    } else {
        pLcd->print("H: --, --, -- Avg.--");
    }

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU] Main Settings");
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
    snprintf(buf1, sizeof(buf1), "> Relay %02d (GPIO %02d) ", selectedRelay + 1, RELAY_PINS[selectedRelay]);
    pLcd->print(buf1);

    pLcd->setCursor(0, 2);
    char buf2[21];
    if (configs[selectedRelay].mode == 2) {
        snprintf(buf2, sizeof(buf2), "[HUM ] Mn:%2.0f%% Mx:%2.0f%% ", configs[selectedRelay].minVal, configs[selectedRelay].maxVal);
    } else {
        snprintf(buf2, sizeof(buf2), "[TEMP] Mn:%4.1f Mx:%4.1f", configs[selectedRelay].minVal, configs[selectedRelay].maxVal);
    }
    pLcd->print(buf2);

    pLcd->setCursor(0, 3);
    pLcd->print("[MENU]Conf  [BCK]Bck");
}

void drawRelayMenuScreen() {
    pLcd->setCursor(0, 0);
    char b0[21];
    snprintf(b0, sizeof(b0), "-- RELAY %02d SETUP --", selectedRelay + 1);
    pLcd->print(b0);

    pLcd->setCursor(0, 1);
    char b1[21];
    snprintf(b1, sizeof(b1), "%s 1.Mode:%s", relayMenuIndex == 0 ? ">" : " ", configs[selectedRelay].mode == 2 ? "Auto Hum " : "Auto Temp");
    pLcd->print(b1);

    char u = (configs[selectedRelay].mode == 2) ? '%' : 'C';
    pLcd->setCursor(0, 2);
    char b2[21];
    snprintf(b2, sizeof(b2), "%s 2.Min : %4.1f %c    ", relayMenuIndex == 1 ? ">" : " ", configs[selectedRelay].minVal, u);
    pLcd->print(b2);

    pLcd->setCursor(0, 3);
    char b3[21];
    snprintf(b3, sizeof(b3), "%s 3.Max : %4.1f %c    ", relayMenuIndex == 2 ? ">" : " ", configs[selectedRelay].maxVal, u);
    pLcd->print(b3);
}

void drawEditValueScreen() {
    pLcd->setCursor(0, 0);
    char b0[21];
    const char* typeName = (configs[selectedRelay].mode == 2) ? "HUM " : "TEMP";
    snprintf(b0, sizeof(b0), "RELAY %02d: %s %s", selectedRelay + 1, selectedTarget == 0 ? "MIN" : "MAX", typeName);
    pLcd->print(b0);

    pLcd->setCursor(0, 1);
    pLcd->print("Press UP/DN to set: ");

    pLcd->setCursor(0, 2);
    char b2[21];
    if (configs[selectedRelay].mode == 2) {
        snprintf(b2, sizeof(b2), " >>> [ %4.1f %%  ] <<<", editingVal);
    } else {
        snprintf(b2, sizeof(b2), " >>> [ %4.1f %cC ] <<<", editingVal, 223);
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

    if (ok1) { temp1 = t1; hum1 = h1; valid1 = true; }
    if (ok2) { temp2 = t2; hum2 = h2; valid2 = true; }
    if (ok3) { temp3 = t3; hum3 = h3; valid3 = true; }

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
        Serial.printf("🌡️ DHT11s -> T1:%s(%.1fC) | T2:%s(%.1fC) | T3:%s(%.1fC) => AVG: %.1fC, %.1f%%\n",
            valid1 ? "OK" : "NC", temp1,
            valid2 ? "OK" : "NC", temp2,
            valid3 ? "OK" : "NC", temp3,
            avgTemp, avgHum);
    }
}

void evaluateSingleRelay(int i, bool isConfigUpdate) {
    if (!sensorValid || configs[i].mode == 0) return;

    float val = (configs[i].mode == 1) ? currentTemp : currentHum;
    String valUnit = (configs[i].mode == 1) ? "°C" : "%";
    String typeName = (configs[i].mode == 1) ? "Avg Temp" : "Avg Hum";

    float minVal = configs[i].minVal;
    float maxVal = configs[i].maxVal;

    bool shouldBeOn = relayStates[i];
    String reason = "";

    if (configs[i].action == 0) {
        // Outside Range ON (User Requirement):
        // 1. Relay ON when temp/hum is EQUAL OR ABOVE maximum threshold (val >= maxVal)
        // 2. Relay ON when temp/hum is EQUAL OR BELOW minimum threshold (val <= minVal)
        // 3. Relay automatically OFF between min and max thresholds (minVal < val < maxVal)
        if (minVal > maxVal) { float t = minVal; minVal = maxVal; maxVal = t; configs[i].minVal = minVal; configs[i].maxVal = maxVal; }
        if (val >= maxVal) {
            shouldBeOn = true;
            reason = "Auto ON: " + typeName + " " + String(val, 1) + valUnit + " >= Max " + String(maxVal, 1) + valUnit;
        } else if (val <= minVal) {
            shouldBeOn = true;
            reason = "Auto ON: " + typeName + " " + String(val, 1) + valUnit + " <= Min " + String(minVal, 1) + valUnit;
        } else {
            shouldBeOn = false;
            reason = "Auto OFF: In Safe Range (" + String(minVal, 1) + " - " + String(maxVal, 1) + valUnit + ") [Avg: " + String(val, 1) + valUnit + "]";
        }
    }
    else if (configs[i].action == 1) { // Inside Range ON
        if (minVal > maxVal) { float t = minVal; minVal = maxVal; maxVal = t; configs[i].minVal = minVal; configs[i].maxVal = maxVal; }
        if (val >= minVal && val <= maxVal) {
            shouldBeOn = true;
            reason = "Auto ON: In Range (" + String(minVal, 1) + " - " + String(maxVal, 1) + valUnit + ")";
        } else {
            shouldBeOn = false;
            reason = "Auto OFF: Out of Range";
        }
    }
    else if (configs[i].action == 2) { // Above Max ON (Cooling Only)
        if (minVal > maxVal) { float t = minVal; minVal = maxVal; maxVal = t; configs[i].minVal = minVal; configs[i].maxVal = maxVal; }
        if (val >= maxVal) {
            shouldBeOn = true;
            reason = "Auto ON: " + typeName + " " + String(val, 1) + valUnit + " >= " + String(maxVal, 1) + valUnit;
        } else if (val <= minVal) {
            shouldBeOn = false;
            reason = "Auto OFF: " + typeName + " " + String(val, 1) + valUnit + " <= " + String(minVal, 1) + valUnit;
        }
    }
    else if (configs[i].action == 3) { // Below Min ON (Heating Only)
        if (minVal > maxVal) { float t = minVal; minVal = maxVal; maxVal = t; configs[i].minVal = minVal; configs[i].maxVal = maxVal; }
        if (val <= minVal) {
            shouldBeOn = true;
            reason = "Auto ON: " + typeName + " " + String(val, 1) + valUnit + " <= " + String(minVal, 1) + valUnit;
        } else if (val >= maxVal) {
            shouldBeOn = false;
            reason = "Auto OFF: " + typeName + " " + String(val, 1) + valUnit + " >= " + String(maxVal, 1) + valUnit;
        }
    }

    if (shouldBeOn != relayStates[i]) {
        setRelay(i, shouldBeOn, reason);
    } else {
        relayReason[i] = reason;
    }
}

void evaluateAutoRules() {
    if (!sensorValid) return;
    for (int i = 0; i < NUM_RELAYS; i++) {
        evaluateSingleRelay(i, false);
    }
}

void setRelay(int index, bool state, String reason) {
    if (index < 0 || index >= NUM_RELAYS) return;
    relayStates[index] = state;
    relayReason[index] = reason;
    digitalWrite(RELAY_PINS[index], state ? RELAY_ON : RELAY_OFF);
    Serial.printf("⚡ Relay %02d (GPIO %02d) -> %s | %s\n", index + 1, RELAY_PINS[index], state ? "ON" : "OFF", reason.c_str());
}

void turnAll(bool state) {
    for (int i = 0; i < NUM_RELAYS; i++) {
        configs[i].mode = 0;
        setRelay(i, state, state ? "Manual Master ON" : "Manual Master OFF");
        delay(40);
    }
}

void setAllAuto() {
    for (int i = 0; i < NUM_RELAYS; i++) {
        configs[i].mode = 1; // 1 = Auto Temp
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
        String keyM = "m" + String(i);
        String keyMin = "min" + String(i);
        String keyMax = "max" + String(i);
        String keyA = "a" + String(i);

        int m = prefRelay.getInt(keyM.c_str(), 1);
        if (m == 0) m = 1; // Default/Reset to Auto Temp mode so conditions are active!
        configs[i].mode   = m;
        prefRelay.putInt(keyM.c_str(), m);

        configs[i].minVal = prefRelay.getFloat(keyMin.c_str(), defaultMins[i]);
        configs[i].maxVal = prefRelay.getFloat(keyMax.c_str(), defaultMaxs[i]);
        configs[i].action = prefRelay.getInt(keyA.c_str(), 0);
    }
}

void saveRelayConfig(int idx) {
    if (idx < 0 || idx >= NUM_RELAYS) return;
    String keyM = "m" + String(idx);
    String keyMin = "min" + String(idx);
    String keyMax = "max" + String(idx);
    String keyA = "a" + String(idx);

    prefRelay.putInt(keyM.c_str(), configs[idx].mode);
    prefRelay.putFloat(keyMin.c_str(), configs[idx].minVal);
    prefRelay.putFloat(keyMax.c_str(), configs[idx].maxVal);
    prefRelay.putInt(keyA.c_str(), configs[idx].action);
}

// --- WEB HANDLERS ---

void handleRelayToggle() {
    if (server.hasArg("id") && server.hasArg("state")) {
        int id = server.arg("id").toInt() - 1;
        bool state = (server.arg("state") == "1");
        if (id >= 0 && id < NUM_RELAYS) {
            configs[id].mode = 0;
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
    if (server.hasArg("id") && server.hasArg("mode") && server.hasArg("min") && server.hasArg("max") && server.hasArg("action")) {
        int id = server.arg("id").toInt() - 1;
        if (id >= 0 && id < NUM_RELAYS) {
            configs[id].mode   = server.arg("mode").toInt();
            configs[id].minVal = server.arg("min").toFloat();
            configs[id].maxVal = server.arg("max").toFloat();
            configs[id].action = server.arg("action").toInt();

            // Ensure minVal is lower than maxVal
            if (configs[id].minVal > configs[id].maxVal) {
                float t = configs[id].minVal;
                configs[id].minVal = configs[id].maxVal;
                configs[id].maxVal = t;
            }

            saveRelayConfig(id);
            readDHTSensor();
            evaluateSingleRelay(id, true);
            Serial.printf("⚡ Web Saved Relay %d: Mode=%d, Min=%.1f, Max=%.1f, Action=%d -> State: %s (%s)\n",
                id + 1, configs[id].mode, configs[id].minVal, configs[id].maxVal, configs[id].action,
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
            configs[i].mode = 1; // Auto Temp
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

    json += "\"relays\":[";
    for (int i = 0; i < NUM_RELAYS; i++) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"id\":" + String(i + 1) + ",";
        json += "\"pin\":" + String(RELAY_PINS[i]) + ",";
        json += "\"state\":" + String(relayStates[i] ? "1" : "0") + ",";
        json += "\"mode\":" + String(configs[i].mode) + ",";
        json += "\"min\":" + String(configs[i].minVal, 1) + ",";
        json += "\"max\":" + String(configs[i].maxVal, 1) + ",";
        json += "\"action\":" + String(configs[i].action) + ",";
        json += "\"reason\":\"" + relayReason[i] + "\"";
        json += "}";
    }
    json += "],";

    json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"staSSID\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : saved_ssid) + "\",";
    json += "\"staIP\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Not Connected") + "\",";
    json += "\"staStatus\":\"" + sta_status_str + "\",";
    json += "\"apIP\":\"" + WiFi.softAPIP().toString() + "\"";
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

        <div class="hero-grid">
            <div class="sensor-card">
                <div class="sensor-label">🌡️ Avg Temperature</div>
                <div class="sensor-val" id="tempVal" style="color:#38bdf8;">--.- °C</div>
                <span id="tempStatus" style="font-size:0.75rem; color:var(--text-dim);">T1: -- | T2: -- | T3: --</span>
            </div>
            <div class="sensor-card">
                <div class="sensor-label">💧 Avg Humidity</div>
                <div class="sensor-val" id="humVal" style="color:#06b6d4;">--.- %</div>
                <span id="humStatus" style="font-size:0.75rem; color:var(--text-dim);">H1: -- | H2: -- | H3: --</span>
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
            if(i === 9) container.innerHTML += `<div class="mod-header">📦 Module 2 (4-ch Relay Board: R9 - R12)</div>`;
            if(i === 13) container.innerHTML += `<div class="mod-header">📦 Module 3 (4-ch Relay Board: R13 - R16)</div>`;

            const card = document.createElement('div');
            card.className = 'relay-item';
            card.id = 'rCard' + i;
            card.innerHTML = `
                <div class="relay-header">
                    <div class="relay-title">
                        <span>Relay ${i}</span>
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

                <div class="config-grid">
                    <div class="field">
                        <label>Mode</label>
                        <select id="rMode${i}" class="input-sm">
                            <option value="1">🌡️ Auto Temp</option>
                            <option value="2">💧 Auto Hum</option>
                            <option value="0">✋ Manual</option>
                        </select>
                    </div>
                    <div class="field">
                        <label>Action</label>
                        <select id="rAction${i}" class="input-sm">
                            <option value="0">⚡ Outside Range ON (≤Min or ≥Max)</option>
                            <option value="1">🟢 Inside Range ON (Min to Max)</option>
                            <option value="2">❄️ Above Max ON (Cooling)</option>
                            <option value="3">🔥 Below Min ON (Heating)</option>
                        </select>
                    </div>
                    <div class="field">
                        <label>🔥 Max Threshold (°C) [ON if ≥]</label>
                        <input type="number" step="0.5" id="rMax${i}" class="input-sm">
                    </div>
                    <div class="field">
                        <label>❄️ Min Threshold (°C) [ON if ≤]</label>
                        <input type="number" step="0.5" id="rMin${i}" class="input-sm">
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px; flex-wrap:wrap; gap:6px;">
                    <div style="display:flex; gap:6px;">
                        <button class="btn-preset btn-preset-on" title="Auto-set thresholds to turn ON now at live temp" onclick="quickSet(${i}, true)">🎯 Set ON</button>
                        <button class="btn-preset btn-preset-off" title="Auto-set thresholds to turn OFF now at live temp" onclick="quickSet(${i}, false)">⭕ Set OFF</button>
                    </div>
                    <button class="btn-save" id="rSaveBtn${i}" onclick="saveConfig(${i})">💾 Save</button>
                </div>
                <div class="reason-text" id="rReason${i}">--</div>
            `;
            container.appendChild(card);
        }

        let liveSensorTemp = 30.0;

        function updateUI(d) {
            if(d.sensorOk) {
                liveSensorTemp = parseFloat(d.temp);
                document.getElementById('tempVal').innerText = d.temp + ' °C';
                document.getElementById('humVal').innerText = d.hum + ' %';
                document.getElementById('tempStatus').innerText = `T1: ${d.temp1}° | T2: ${d.temp2}° | T3: ${d.temp3}° (Avg: ${d.temp}°)`;
                document.getElementById('humStatus').innerText = `H1: ${d.hum1}% | H2: ${d.hum2}% | H3: ${d.hum3}% (Avg: ${d.hum}%)`;
            } else {
                document.getElementById('tempVal').innerText = '--.- °C';
                document.getElementById('humVal').innerText = '--.- %';
                document.getElementById('tempStatus').innerText = 'Sensor error (Check GPIO 8, 3, 42)';
                document.getElementById('humStatus').innerText = 'Sensor error (Check GPIO 8, 3, 42)';
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

                    // NEVER overwrite user inputs while user is typing / active in that field!
                    const modeEl = document.getElementById('rMode' + r.id);
                    const actionEl = document.getElementById('rAction' + r.id);
                    const minEl = document.getElementById('rMin' + r.id);
                    const maxEl = document.getElementById('rMax' + r.id);

                    if(modeEl && document.activeElement !== modeEl) modeEl.value = r.mode;
                    if(actionEl && document.activeElement !== actionEl) actionEl.value = r.action;
                    if(minEl && document.activeElement !== minEl) minEl.value = r.min;
                    if(maxEl && document.activeElement !== maxEl) maxEl.value = r.max;
                }
            });

            document.getElementById('staStatus').innerText = d.staStatus;
            document.getElementById('staIP').innerText = d.staIP;
        }

        function fetchStatus() {
            fetch('/status')
                .then(r => r.json())
                .then(d => updateUI(d))
                .catch(e => console.log(e));
        }

        function toggleRelay(id, state) {
            fetch(`/toggle?id=${id}&state=${state ? 1 : 0}`).then(fetchStatus);
        }

        function setAllAuto() {
            fetch('/allauto').then(fetchStatus);
        }

        function setAll(state) {
            fetch(`/all?state=${state}`).then(fetchStatus);
        }

        function quickSet(id, turnOn) {
            const curT = liveSensorTemp || 30.0;
            const modeEl = document.getElementById('rMode' + id);
            const actionEl = document.getElementById('rAction' + id);
            const minEl = document.getElementById('rMin' + id);
            const maxEl = document.getElementById('rMax' + id);

            if(modeEl) modeEl.value = '1'; // Auto Temp
            if(actionEl) actionEl.value = '0'; // Outside Range ON

            if(turnOn) {
                // To turn ON immediately: set max lower than live temp (live temp >= max)
                if(maxEl) maxEl.value = (curT - 0.5).toFixed(1);
                if(minEl) minEl.value = (curT - 3.0).toFixed(1);
            } else {
                // To turn OFF immediately: set live temp inside safe range (min < live temp < max)
                if(minEl) minEl.value = (curT - 2.0).toFixed(1);
                if(maxEl) maxEl.value = (curT + 2.0).toFixed(1);
            }
            saveConfig(id);
        }

        function saveConfig(id) {
            const btn = document.getElementById('rSaveBtn' + id);
            if(btn) { btn.innerText = '⏳ Saving...'; btn.style.background = '#eab308'; }

            const mode = document.getElementById('rMode' + id).value;
            const action = document.getElementById('rAction' + id).value;
            const minVal = document.getElementById('rMin' + id).value;
            const maxVal = document.getElementById('rMax' + id).value;

            const body = new URLSearchParams();
            body.append('id', id);
            body.append('mode', mode);
            body.append('action', action);
            body.append('min', minVal);
            body.append('max', maxVal);

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

        fetchStatus();
        setInterval(fetchStatus, 2000);
    </script>
</body>
</html>
)rawliteral";

    server.send(200, "text/html", html);
}
