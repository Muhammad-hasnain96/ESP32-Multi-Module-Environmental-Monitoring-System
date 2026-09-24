// =====================================================================
// ESP32-S3 — MODULE 1: Air Quality, Gas, Flow & 2004 LCD System
// =====================================================================
//  Sensor         ESP32-S3 Pin        Measurement / Wiring
//  ------------   -----------------   --------------------------------
//  DHT11          GPIO 4              Temperature, Humidity, HeatIndex (VCC=3.3V, GND=GND)
//  CCS811 (CO2)   Wire1: SDA=GPIO 17  eCO2 (ppm), TVOC (ppb)
//                        SCL=GPIO 16  (VCC=3.3V, GND=GND, WAKE=GND, RST=3.3V, ADDR=GND)
//  MQ-137         GPIO 1 (AO)         Ammonia NH3 (ppm) [ADC1_CH0, VCC=5V, GND=GND]
//                 GPIO 2 (DO)         Threshold Alert
//  FS200A         GPIO 5 (INT)        Air/Water Flow
//  2004 I2C LCD   Wire:  SDA=GPIO 15  20x4 Real-time Local Display
//                        SCL=GPIO 18  (VCC=5V, GND=GND)
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_CCS811.h>
#include <LiquidCrystal_I2C.h>

// =====================================================================
//  1. DHT11 — Temperature & Humidity (GPIO 4)
// =====================================================================
#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// =====================================================================
//  2. MQ-137 — Ammonia Gas Sensor (GPIO 1 AO / GPIO 2 DO)
// =====================================================================
#define MQ137_AO  1
#define MQ137_DO  2
float   mq137Baseline   = 0.0f;
bool    mq137Warmed     = false;
unsigned long mq137WarmStart = 0;
const unsigned long MQ137_WARMUP = 30000UL;

// =====================================================================
//  3. FS200A Flow Sensor (GPIO 5, Interrupt)
// =====================================================================
#define FLOW_PIN  5
const float PULSES_PER_L = 7.5f;
volatile unsigned long flowPulses = 0;
float totalVolume = 0.0f;
void IRAM_ATTR flowISR() { flowPulses++; }

// =====================================================================
//  4. CCS811 CO2 Sensor — I2C Bus 1 (Wire1: SDA=17, SCL=16)
// =====================================================================
#define CCS811_SDA_PIN  17
#define CCS811_SCL_PIN  16
Adafruit_CCS811 ccs;
bool    ccsOK    = false;
uint16_t ccsECO2 = 400, ccsTVOC = 0;

// =====================================================================
//  5. 2004 Character LCD (Wire: SDA=15, SCL=18, Auto-Detect 0x27 / 0x3F)
// =====================================================================
#define LCD_SDA_PIN  15
#define LCD_SCL_PIN  18
LiquidCrystal_I2C lcd(0x27, 20, 4);
bool lcd_available = false;

// =====================================================================
//  WiFi & ThingsBoard
// =====================================================================
const char* WIFI_SSID = "Meeting Room";
const char* WIFI_PASS = "Shahid786$$";
const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";

// =====================================================================
//  Timing & Rotating Screens
// =====================================================================
const unsigned long INTERVAL          = 3000UL; // 3 seconds sensor reading & serial telemetry
const unsigned long LCD_PAGE_INTERVAL = 5000UL; // 5 seconds LCD screen rotation
unsigned long lastLog = 0;
unsigned long lastLcdSwitch = 0;
unsigned long loopCount = 0;
int lcdScreenPage = 0; // 0 = Screen 1 (Temp/Hum/HeatIdx), 1 = Screen 2 (CO2/NH3/Flow)

// Global sensor values for LCD rendering
float currentTC = 25.0f, currentTF = 77.0f, currentHum = 50.0f, currentHI = 25.0f;
float currentNH3 = 0.0f, currentMQ137V = 0.0f;
float currentFlowHz = 0.0f;

// =====================================================================
//  Helpers
// =====================================================================
String uptime() {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600; s %= 3600;
    unsigned long m = s / 60;   s %= 60;
    char b[12]; snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", h, m, s);
    return String(b);
}

int avgADC(int pin, int n = 10) {
    long sum = 0;
    for (int i = 0; i < n; i++) { sum += analogRead(pin); delay(5); }
    return (int)(sum / n);
}

// Full Status Labels (for Serial Monitor & ThingsBoard)
const char* nh3Label(float p, bool w) {
    if (!w)         return "WARMING UP";
    if (p <   5.0f) return "CLEAN";
    if (p <  25.0f) return "LOW";
    if (p <  50.0f) return "MODERATE";
    if (p < 100.0f) return "HIGH";
    return                  "DANGER";
}
const char* flowLabel(float hz) {
    if (hz == 0)    return "NO FLOW";
    if (hz < 5.0f)  return "VERY LOW";
    if (hz < 15.0f) return "LOW";
    if (hz < 35.0f) return "MODERATE";
    return                  "HIGH";
}
const char* co2Label(uint16_t v) {
    if (v <  600)   return "EXCELLENT";
    if (v < 1000)   return "GOOD";
    if (v < 1500)   return "MODERATE";
    if (v < 2500)   return "POOR";
    return                  "HAZARDOUS";
}
const char* tvocLabel(uint16_t v) {
    if (v <  220)   return "LOW";
    if (v <  660)   return "MODERATE";
    if (v < 2200)   return "HIGH";
    return                  "VERY HIGH";
}

// Concise Status Labels for 2004 Character LCD (20-column limit)
const char* shortCo2Label(uint16_t v) {
    if (v <  600)   return "EXCL";
    if (v < 1000)   return "GOOD";
    if (v < 1500)   return "MODR";
    if (v < 2500)   return "POOR";
    return                 "HAZD";
}

const char* shortNh3Label(float p, bool w) {
    if (!w)         return "WARM";
    if (p <   5.0f) return "CLEN";
    if (p <  25.0f) return "LOW ";
    if (p <  50.0f) return "MODR";
    if (p < 100.0f) return "HIGH";
    return                 "DNGR";
}

const char* shortFlowLabel(float hz) {
    if (hz == 0)    return "NONE ";
    if (hz < 5.0f)  return "V-LOW";
    if (hz < 15.0f) return "LOW  ";
    if (hz < 35.0f) return "MODR ";
    return                 "HIGH ";
}

// =====================================================================
//  WiFi
// =====================================================================
void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    WiFi.disconnect(true); delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int n = 0;
    while (WiFi.status() != WL_CONNECTED && n++ < 20) {
        delay(500); Serial.print('.');
    }
    Serial.println(WiFi.status() == WL_CONNECTED
        ? " Connected!" : " Pending...");
}

// =====================================================================
//  MQ-137 Read (0.0 ppm Baseline Calibration)
// =====================================================================
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

// =====================================================================
//  CCS811 Init
// =====================================================================
bool initCCS() {
    if (ccs.begin(0x5A, &Wire1)) { ccsOK = true; return true; }
    if (ccs.begin(0x5B, &Wire1)) { ccsOK = true; return true; }
    return false;
}

// =====================================================================
//  ThingsBoard — send all 16 keys
// =====================================================================
void sendTelemetry(float tC, float tF, float hum, float hi,
                   float nh3, const char* nh3Lv, float mq137V, bool alert,
                   unsigned long pulses, float flowHz, float vol, const char* flowLv,
                   uint16_t eco2, const char* co2Lv, uint16_t tvoc, const char* tvocLv) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F(" Skipped (WiFi offline)"));
        return;
    }
    String url = String(TB_HOST) + "/api/v1/" + TB_TOKEN + "/telemetry";
    WiFiClientSecure client; client.setInsecure();
    HTTPClient https; https.setTimeout(8000);
    if (!https.begin(client, url)) {
        Serial.println(F(" Error starting HTTPS"));
        return;
    }
    https.addHeader("Content-Type", "application/json");
    String p = "{";
    p += "\"temperature\":"    + String(tC, 1);
    p += ",\"temperatureF\":"  + String(tF, 1);
    p += ",\"humidity\":"      + String(hum, 1);
    p += ",\"heatIndex\":"     + String(hi, 1);
    p += ",\"nh3PPM\":"        + String(nh3, 1);
    p += ",\"nh3Level\":\""    + String(nh3Lv) + "\"";
    p += ",\"mq137V\":"        + String(mq137V, 3);
    p += ",\"nh3Alert\":"      + String(alert ? 1 : 0);
    p += ",\"flowPulses\":"    + String(pulses);
    p += ",\"flowHz\":"        + String(flowHz, 2);
    p += ",\"flowVolume\":"    + String(vol, 2);
    p += ",\"flowStatus\":\""  + String(flowLv) + "\"";
    p += ",\"eCO2\":"          + String(eco2);
    p += ",\"eco2Status\":\""  + String(co2Lv) + "\"";
    p += ",\"tvoc\":"          + String(tvoc);
    p += ",\"tvocStatus\":\""  + String(tvocLv) + "\"";
    p += "}";
    int code = https.POST(p);
    if (code > 0) {
        Serial.printf(" HTTP %d\n", code);
    } else {
        Serial.printf(" Failed: %s\n", https.errorToString(code).c_str());
    }
    https.end();
}

// =====================================================================
//  Print separator line (60 chars)
// =====================================================================
void printLine(char c = '-') {
    for (int i = 0; i < 60; i++) Serial.print(c);
    Serial.println();
}

// =====================================================================
//  2004 Character LCD Multi-Screen Renderer
// =====================================================================
void updateLCD() {
    if (!lcd_available) return;
    char buf[21];

    if (lcdScreenPage == 0) {
        // =============================================================
        //  SCREEN 1: Ambient Environment (Temp, Humidity, Heat Index)
        // =============================================================
        snprintf(buf, sizeof(buf), "-- AIR QUALITY [1/2]");
        lcd.setCursor(0, 0); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Temp    : %5.1f C   ", currentTC);
        lcd.setCursor(0, 1); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Humidity: %5.1f %%   ", currentHum);
        lcd.setCursor(0, 2); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Heat Idx: %5.1f C   ", currentHI);
        lcd.setCursor(0, 3); lcd.print(buf);
    } else {
        // =============================================================
        //  SCREEN 2: Gas & Flow (CO2, NH3, Air Flow)
        // =============================================================
        snprintf(buf, sizeof(buf), "-- GAS & FLOW  [2/2]");
        lcd.setCursor(0, 0); lcd.print(buf);

        if (ccsOK) {
            snprintf(buf, sizeof(buf), "CO2 : %4uppm [%-4s] ", ccsECO2, shortCo2Label(ccsECO2));
        } else {
            snprintf(buf, sizeof(buf), "CO2 : OFFLINE       ");
        }
        lcd.setCursor(0, 1); lcd.print(buf);

        if (!mq137Warmed) {
            unsigned long remain = (MQ137_WARMUP > (millis() - mq137WarmStart))
                                 ? (MQ137_WARMUP - (millis() - mq137WarmStart)) / 1000 : 0;
            snprintf(buf, sizeof(buf), "NH3 : WARMING (%2lus) ", remain);
        } else {
            snprintf(buf, sizeof(buf), "NH3 : %4.1fppm [%-4s] ", currentNH3, shortNh3Label(currentNH3, mq137Warmed));
        }
        lcd.setCursor(0, 2); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Flow: %4.1fHz [%-5s] ", currentFlowHz, shortFlowLabel(currentFlowHz));
        lcd.setCursor(0, 3); lcd.print(buf);
    }
}

// =====================================================================
//  Setup
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine('=');
    Serial.println(F("  ESP32-S3 MODULE 1: AIR QUALITY, GAS & 2004 LCD SYSTEM"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | MQ-137 (GPIO 1/2)"));
    Serial.println(F("           FS200A (GPIO 5) | CCS811 (Wire1: SDA=17, SCL=16)"));
    Serial.println(F("  Display: 2004 I2C LCD (Wire: SDA=15, SCL=18)"));
    Serial.println(F("  Cloud  : ThingsBoard"));
    printLine('=');

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // 1. DHT11
    dht.begin();
    Serial.println(F("  [OK] DHT11    -> GPIO 4"));

    // 2. MQ-137
    pinMode(MQ137_AO, INPUT);
    pinMode(MQ137_DO, INPUT);
    mq137WarmStart = millis();
    Serial.println(F("  [OK] MQ-137   -> GPIO 1 (AO) / GPIO 2 (DO) [warming up...]"));

    // 3. FS200A
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, FALLING);
    Serial.println(F("  [OK] FS200A   -> GPIO 5 (Interrupt)"));

    // 4. Initialize 2004 I2C LCD on Dedicated Wire (SDA=15, SCL=18)
    pinMode(LCD_SDA_PIN, INPUT_PULLUP);
    pinMode(LCD_SCL_PIN, INPUT_PULLUP);
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
    Wire.setTimeOut(25);
    Serial.print(F("  [..] 2004 I2C LCD (Dedicated SDA=15, SCL=18)... "));
    byte lcdAddr = 0;
    Wire.beginTransmission(0x27);
    if (Wire.endTransmission() == 0) lcdAddr = 0x27;
    else {
        Wire.beginTransmission(0x3F);
        if (Wire.endTransmission() == 0) lcdAddr = 0x3F;
    }
    if (lcdAddr != 0) {
        lcd = LiquidCrystal_I2C(lcdAddr, 20, 4);
        lcd.init();
        lcd.backlight();
        lcd.clear();

        // 🌟 Welcome Splash Screen
        lcd.setCursor(0, 0);
        lcd.print(F("===================="));
        lcd.setCursor(0, 1);
        lcd.print(F("    Welcome to      "));
        lcd.setCursor(0, 2);
        lcd.print(F("     Module-1       "));
        lcd.setCursor(0, 3);
        lcd.print(F("===================="));
        lcd_available = true;
        Serial.printf("ONLINE at 0x%02X [OK]\n", lcdAddr);
        delay(2500); // Show Welcome message clearly for 2.5 seconds

        // System Initialization Status
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("ESP32 MONITOR SYSTEM"));
        lcd.setCursor(0, 1);
        lcd.print(F("MODULE 1: AIR QUALITY"));
        lcd.setCursor(0, 2);
        lcd.print(F("WiFi Connecting...  "));
        lcd.setCursor(0, 3);
        lcd.print(F("Please wait...      "));
    } else {
        Serial.println(F("OFFLINE (Check SDA=15, SCL=18, VCC=5V, GND)"));
    }

    // 5. CCS811 on I2C Bus 1 (Wire1: SDA=17, SCL=16)
    pinMode(CCS811_SDA_PIN, INPUT_PULLUP);
    pinMode(CCS811_SCL_PIN, INPUT_PULLUP);
    Wire1.begin(CCS811_SDA_PIN, CCS811_SCL_PIN, 50000);
    Wire1.setTimeOut(500);
    Serial.print(F("  [..] CCS811   -> Wire1 SDA=17 SCL=16 ... "));
    if (initCCS()) {
        Serial.println(F("ONLINE [OK]"));
    } else {
        Serial.println(F("OFFLINE - Check WAK->GND, RST->3.3V, SDA=17, SCL=16"));
    }

    // 6. WiFi
    Serial.print(F("  [..] WiFi     Connecting"));
    connectWiFi();

    if (lcd_available) {
        lcd.setCursor(0, 2);
        if (WiFi.status() == WL_CONNECTED) {
            lcd.print(F("WiFi: Connected!    "));
        } else {
            lcd.print(F("WiFi: Offline       "));
        }
        lcd.setCursor(0, 3);
        lcd.print(F("Starting System...  "));
        delay(1200);
        lcd.clear();
        updateLCD();
    }

    lastLog = millis();
    lastLcdSwitch = millis();

    printLine('=');
    Serial.println();
}

// =====================================================================
//  Loop
// =====================================================================
void loop() {
    connectWiFi();
    if (!ccsOK) initCCS();

    unsigned long now = millis();

    // 1. Read sensors & send telemetry every 3 seconds (INTERVAL)
    if (now - lastLog >= INTERVAL) {
        lastLog = now;
        loopCount++;

        // ---- 1. FS200A ----
        noInterrupts();
        unsigned long pulses = flowPulses; flowPulses = 0;
        interrupts();
        currentFlowHz = pulses / (INTERVAL / 1000.0f);
        totalVolume  += pulses / PULSES_PER_L;

        // ---- 2. DHT11 ----
        float hum  = dht.readHumidity();
        float tC   = dht.readTemperature();
        float tF   = dht.readTemperature(true);
        if (isnan(hum) || isnan(tC)) { hum = 50; tC = 25; tF = 77; }
        float hi   = dht.computeHeatIndex(tC, hum, false);
        currentHum = hum;
        currentTC  = tC;
        currentTF  = tF;
        currentHI  = hi;

        // ---- 3. MQ-137 ----
        float mq137V = 0;
        float nh3    = readMQ137(mq137V);
        bool  alert  = (digitalRead(MQ137_DO) == LOW);
        currentNH3   = nh3;
        currentMQ137V= mq137V;

        // ---- 4. CCS811 ----
        if (ccsOK) {
            ccs.setEnvironmentalData(hum, tC);
            if (ccs.available() && !ccs.readData()) {
                ccsECO2 = ccs.geteCO2();
                ccsTVOC = ccs.getTVOC();
            }
        }

        // =====================================================================
        //  Professional Serial Output
        // =====================================================================
        Serial.println();
        printLine('=');
        Serial.printf("  READING #%-4lu  |  UPTIME: %s  |  WiFi: %s\n",
            loopCount, uptime().c_str(),
            WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Connecting...");
        printLine('=');

        // DHT11
        Serial.println(F("  TEMPERATURE & HUMIDITY  [DHT11 - GPIO 4]"));
        printLine();
        Serial.printf("    Temperature  :  %5.1f C   (%5.1f F)\n", tC, tF);
        Serial.printf("    Humidity     :  %5.1f %%\n", hum);
        Serial.printf("    Heat Index   :  %5.1f C\n", hi);
        printLine();

        // MQ-137
        Serial.println(F("  AMMONIA GAS  [MQ-137 - GPIO 1/2]"));
        printLine();
        Serial.printf("    NH3 Level    :  %6.1f ppm   [%s]\n",
                      nh3, nh3Label(nh3, mq137Warmed));
        Serial.printf("    Voltage      :  %6.3f V\n", mq137V);
        Serial.printf("    DO Alert     :  %s\n", alert ? "YES - THRESHOLD EXCEEDED!" : "No");
        if (!mq137Warmed) {
            unsigned long remain = (MQ137_WARMUP > (now - mq137WarmStart))
                                 ? (MQ137_WARMUP - (now - mq137WarmStart)) / 1000 : 0;
            Serial.printf("    Warm-up      :  %lu seconds remaining\n", remain);
        }
        printLine();

        // FS200A
        Serial.println(F("  FLOW SENSOR  [FS200A - GPIO 5]"));
        printLine();
        Serial.printf("    Frequency    :  %6.2f Hz\n", currentFlowHz);
        Serial.printf("    Total Volume :  %6.2f L-eq\n", totalVolume);
        Serial.printf("    Status       :  %s\n", flowLabel(currentFlowHz));
        printLine();

        // CCS811
        Serial.printf("  CO2 & TVOC  [CCS811 - Wire1 SDA=17 SCL=16]  %s\n",
                      ccsOK ? "" : "  [OFFLINE]");
        printLine();
        if (ccsOK) {
            Serial.printf("    eCO2         :  %5u ppm   [%s]\n",
                          ccsECO2, co2Label(ccsECO2));
            Serial.printf("    TVOC         :  %5u ppb   [%s]\n",
                          ccsTVOC, tvocLabel(ccsTVOC));
        } else {
            Serial.println(F("    Check: WAK->GND, RST->3.3V, SDA=17, SCL=16"));
        }
        printLine();

        // ThingsBoard
        Serial.print(F("  CLOUD -> ThingsBoard  (16 keys) ..."));
        sendTelemetry(tC, tF, hum, hi,
                      nh3, nh3Label(nh3, mq137Warmed), mq137V, alert,
                      pulses, currentFlowHz, totalVolume, flowLabel(currentFlowHz),
                      ccsECO2, co2Label(ccsECO2), ccsTVOC, tvocLabel(ccsTVOC));

        printLine('=');

        // Update live screen numbers
        if (lcd_available) {
            updateLCD();
        }
    }

    // 2. Rotate LCD screen page every 5 seconds (5000ms)
    if (lcd_available) {
        if (now - lastLcdSwitch >= LCD_PAGE_INTERVAL) {
            lastLcdSwitch = now;
            lcdScreenPage = (lcdScreenPage + 1) % 2;
            lcd.clear();
            updateLCD();
        }
    }

    delay(50);
}
