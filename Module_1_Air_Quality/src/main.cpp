// =====================================================================
// ESP32 — Multi-Sensor Environmental Monitoring System
// =====================================================================
//  Sensor         GPIO / Interface    Measurement
//  ------------   ----------------   --------------------------------
//  DHT11          GPIO 4             Temperature, Humidity, HeatIndex
//  MQ-137         GPIO 33 (AO)       Ammonia NH3 (ppm) [0.0 ppm baseline]
//                 GPIO 25 (DO)       Threshold Alert
//  FS200A         GPIO 27 (INT)      Air/Water Flow
//  CCS811         Wire1: SDA=17      eCO2 (ppm), TVOC (ppb)
//                        SCL=16
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_CCS811.h>

// =====================================================================
//  1. DHT11 — Temperature & Humidity (GPIO 4)
// =====================================================================
#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// =====================================================================
//  2. MQ-137 — Ammonia Gas Sensor (GPIO 33 AO / GPIO 25 DO)
// =====================================================================
#define MQ137_AO  33
#define MQ137_DO  25
float   mq137Baseline   = 0.0f;
bool    mq137Warmed     = false;
unsigned long mq137WarmStart = 0;
const unsigned long MQ137_WARMUP = 30000UL;

// =====================================================================
//  3. FS200A Flow Sensor (GPIO 27, Interrupt)
// =====================================================================
#define FLOW_PIN  27
const float PULSES_PER_L = 7.5f;
volatile unsigned long flowPulses = 0;
float totalVolume = 0.0f;
void IRAM_ATTR flowISR() { flowPulses++; }

// =====================================================================
//  4. CCS811 — I2C Bus 1 (Wire1: SDA=17, SCL=16)
// =====================================================================
Adafruit_CCS811 ccs;
bool    ccsOK    = false;
uint16_t ccsECO2 = 400, ccsTVOC = 0;

// =====================================================================
//  WiFi & ThingsBoard
// =====================================================================
const char* WIFI_SSID = "Meeting Room";
const char* WIFI_PASS = "Shahid786$$";
const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";

// =====================================================================
//  Timing
// =====================================================================
const unsigned long INTERVAL = 3000UL;
unsigned long lastLog = 0;
unsigned long loopCount = 0;

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

// Status Labels
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
//  Setup
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine('=');
    Serial.println(F("  ESP32  MULTI-SENSOR ENVIRONMENTAL MONITOR  v2.1"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 | MQ-137 | FS200A | CCS811"));
    Serial.println(F("  Cloud  : ThingsBoard  (16 telemetry keys)"));
    printLine('=');

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // DHT11
    dht.begin();
    Serial.println(F("  [OK] DHT11    GPIO 4"));

    // MQ-137
    pinMode(MQ137_AO, INPUT);
    pinMode(MQ137_DO, INPUT);
    mq137WarmStart = millis();
    Serial.println(F("  [OK] MQ-137   GPIO 33 (AO) / GPIO 25 (DO)  [warming up...]"));

    // FS200A
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, FALLING);
    Serial.println(F("  [OK] FS200A   GPIO 27 (Interrupt)"));

    // CCS811 on I2C Bus 1 (Wire1: SDA=17, SCL=16)
    Wire1.begin(17, 16, 50000);
    Wire1.setTimeOut(3000);
    Serial.print(F("  [..] CCS811   Wire1 SDA=17 SCL=16 ... "));
    if (initCCS()) {
        Serial.println(F("ONLINE"));
    } else {
        Serial.println(F("OFFLINE - Check WAK->GND, RST->3.3V"));
    }

    // WiFi
    Serial.print(F("  [..] WiFi     Connecting"));
    connectWiFi();

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
    if (now - lastLog < INTERVAL) { delay(50); return; }
    lastLog = now;
    loopCount++;

    // ---- 1. FS200A ----
    noInterrupts();
    unsigned long pulses = flowPulses; flowPulses = 0;
    interrupts();
    float flowHz  = pulses / (INTERVAL / 1000.0f);
    totalVolume  += pulses / PULSES_PER_L;

    // ---- 2. DHT11 ----
    float hum  = dht.readHumidity();
    float tC   = dht.readTemperature();
    float tF   = dht.readTemperature(true);
    if (isnan(hum) || isnan(tC)) { hum = 50; tC = 25; tF = 77; }
    float hi   = dht.computeHeatIndex(tC, hum, false);

    // ---- 3. MQ-137 ----
    float mq137V = 0;
    float nh3    = readMQ137(mq137V);
    bool  alert  = (digitalRead(MQ137_DO) == LOW);

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
    Serial.println(F("  AMMONIA GAS  [MQ-137 - GPIO 33/25]"));
    printLine();
    Serial.printf("    NH3 Level    :  %6.1f ppm   [%s]\n",
                  nh3, nh3Label(nh3, mq137Warmed));
    Serial.printf("    Voltage      :  %6.3f V\n", mq137V);
    Serial.printf("    DO Alert     :  %s\n", alert ? "YES - THRESHOLD EXCEEDED!" : "No");
    if (!mq137Warmed) {
        unsigned long remain = (MQ137_WARMUP - (now - mq137WarmStart)) / 1000;
        Serial.printf("    Warm-up      :  %lu seconds remaining\n", remain);
    }
    printLine();

    // FS200A
    Serial.println(F("  FLOW SENSOR  [FS200A - GPIO 27]"));
    printLine();
    Serial.printf("    Frequency    :  %6.2f Hz\n", flowHz);
    Serial.printf("    Total Volume :  %6.2f L-eq\n", totalVolume);
    Serial.printf("    Status       :  %s\n", flowLabel(flowHz));
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
                  pulses, flowHz, totalVolume, flowLabel(flowHz),
                  ccsECO2, co2Label(ccsECO2), ccsTVOC, tvocLabel(ccsTVOC));

    printLine('=');
}
