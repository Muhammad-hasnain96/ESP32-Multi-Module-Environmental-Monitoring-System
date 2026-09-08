// =====================================================================
// ESP32 — MODULE 3: Light, Soil & Environmental System
// =====================================================================
// SENSOR WIRING:
//   1. DHT11 Sensor:
//      - DATA  -> GPIO 4
//      - VCC   -> 3.3V / 5V
//      - GND   -> GND
//
//   2. BH1750 Digital Light Sensor:
//      - SDA   -> GPIO 21
//      - SCL   -> GPIO 22
//      - ADDR  -> GND (Address: 0x23)
//      - VCC   -> 3.3V
//      - GND   -> GND
//
//   3. Capacitive Soil Moisture Sensor v2.0:
//      - AOUT  -> GPIO 34 (ADC1_CH6 - Analog Input)
//      - VCC   -> 3.3V
//      - GND   -> GND
//
//   [COMMENTED / OPTIONAL] 4. HX711 5kg Load Cell:
//      - DT    -> GPIO 14
//      - SCK   -> GPIO 12
//      - VCC   -> 5V (VIN)
//      - GND   -> GND
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
// #include "HX711.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// =====================================================================
//  1. DHT11 Configuration (GPIO 4)
// =====================================================================
#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// =====================================================================
//  2. BH1750 Configuration (I2C: SDA=21, SCL=22)
// =====================================================================
#define I2C_SDA_PIN  21
#define I2C_SCL_PIN  22
BH1750 lightMeter(0x23);
bool bh1750_available = false;
float currentLux = 0.0f;

// Calibration factor: Master sensor = 202.5 lx / BH1750 raw = 277.1 lx
const float LIGHT_CAL_FACTOR = 0.7308f;

// =====================================================================
//  3. Capacitive Soil Moisture Sensor Configuration (GPIO 34)
// =====================================================================
#define SOIL_PIN         34
#define VREF             3.3f
#define ADC_RESOLUTION   4095.0f

// Calibration reference values (ADC 0-4095)
const int AIR_VALUE   = 3000; // Dry air (0% moisture)
const int WATER_VALUE = 1350; // Pure water (100% moisture)

// =====================================================================
//  [COMMENTED] 4. HX711 Load Cell Configuration
// =====================================================================
// #define HX711_DOUT_PIN  14
// #define HX711_SCK_PIN   12
// HX711 scale;
// bool scale_available = false;
// float scale_calibration_factor = 420.0f;

// =====================================================================
//  WiFi & ThingsBoard Configuration
// =====================================================================
const char* WIFI_SSID = "Meeting Room";
const char* WIFI_PASS = "Shahid786$$";
const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";

// =====================================================================
//  Timing & State Variables
// =====================================================================
const unsigned long INTERVAL = 3000UL; // Read and send every 3 seconds
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

void printLine(char c = '-') {
    for (int i = 0; i < 62; i++) Serial.print(c);
    Serial.println();
}

// Light Status Classification
const char* getLightStatus(float lux) {
    if (lux < 1.0f)     return "DARK (Night)";
    if (lux < 50.0f)    return "DIM (Low Indoor)";
    if (lux < 200.0f)   return "MODERATE (Indoor Normal)";
    if (lux < 500.0f)   return "BRIGHT (Office / Well Lit)";
    if (lux < 1000.0f)  return "VERY BRIGHT (Near Window)";
    if (lux < 10000.0f) return "OUTDOOR SHADE";
    if (lux < 30000.0f) return "CLOUDY OUTDOOR";
    return                     "DIRECT SUNLIGHT";
}

// Soil Moisture Status Classification
const char* getSoilStatus(float pct) {
    if (pct < 15.0f)  return "VERY DRY";
    if (pct < 35.0f)  return "DRY";
    if (pct < 65.0f)  return "OPTIMAL";
    if (pct < 85.0f)  return "WET";
    return                   "WATERLOGGED";
}

// =====================================================================
//  Capacitive Soil Sensor Reading with Multi-Sample Filter
// =====================================================================
float readSoilMoisture(int &outRawADC, float &outVoltage) {
    const int SAMPLES = 30;
    int buffer[SAMPLES];

    for (int i = 0; i < SAMPLES; i++) {
        buffer[i] = analogRead(SOIL_PIN);
        delay(2);
    }

    for (int i = 0; i < SAMPLES - 1; i++) {
        for (int j = i + 1; j < SAMPLES; j++) {
            if (buffer[i] > buffer[j]) {
                int temp = buffer[i];
                buffer[i] = buffer[j];
                buffer[j] = temp;
            }
        }
    }

    long sum = 0;
    for (int i = 10; i < 20; i++) {
        sum += buffer[i];
    }
    outRawADC = sum / 10;
    outVoltage = (outRawADC / ADC_RESOLUTION) * VREF;

    float moisturePct = ((float)(AIR_VALUE - outRawADC) / (float)(AIR_VALUE - WATER_VALUE)) * 100.0f;
    return constrain(moisturePct, 0.0f, 100.0f);
}

// =====================================================================
//  WiFi Connection
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
    Serial.println(WiFi.status() == WL_CONNECTED ? " Connected!" : " Pending...");
}

// =====================================================================
//  ThingsBoard Telemetry Dispatch (Module 3: 10 Keys)
// =====================================================================
void sendTelemetry(float tC, float tF, float hum, float hi,
                   float lux, const char* lightLv,
                   float soilPct, float soilV, int soilADC, const char* soilLv) {
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

    // JSON Payload
    String p = "{";
    // DHT11 Ambient
    p += "\"temperature\":"    + String(tC, 1);
    p += ",\"temperatureF\":"  + String(tF, 1);
    p += ",\"humidity\":"      + String(hum, 1);
    p += ",\"heatIndex\":"     + String(hi, 1);
    p += ",\"m3_temperature\":" + String(tC, 1);
    p += ",\"m3_humidity\":"   + String(hum, 1);
    // BH1750 Light
    p += ",\"lux\":"           + String(lux, 1);
    p += ",\"lightLevel\":\""   + String(lightLv) + "\"";
    // Soil Moisture
    p += ",\"soilMoisture\":"  + String(soilPct, 1);
    p += ",\"moisture\":"      + String(soilPct, 1);
    p += ",\"soil_moisture\":" + String(soilPct, 1);
    p += ",\"soilVoltage\":"   + String(soilV, 3);
    p += ",\"soilRawADC\":"    + String(soilADC);
    p += ",\"soilStatus\":\""   + String(soilLv) + "\"";
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
//  Setup
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine('=');
    Serial.println(F("  ESP32 — MODULE 3: COMPLETE LIGHT, SOIL & ENVIRONMENT v1.1"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | BH1750 (GPIO 21/22) | Soil (GPIO 34)"));
    Serial.println(F("  Cloud  : ThingsBoard"));
    printLine('=');

    // 1. Initialize DHT11
    dht.begin();
    Serial.println(F("  [OK] DHT11 Initialized on GPIO 4"));

    // 2. Initialize Soil Sensor ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db); // 0 - 3.3V
    pinMode(SOIL_PIN, INPUT);
    Serial.println(F("  [OK] Soil Moisture Sensor on GPIO 34 (ADC1_CH6)"));

    // 3. Initialize I2C and BH1750
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Serial.print(F("  [..] BH1750 Light Sensor on I2C (0x23)... "));
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)) {
        bh1750_available = true;
        Serial.println(F("ONLINE [OK]"));
    } else {
        if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire)) {
            bh1750_available = true;
            Serial.println(F("ONLINE at 0x5C [OK]"));
        } else {
            Serial.println(F("OFFLINE! Check SDA=21, SCL=22, VCC=3.3V, ADDR=GND"));
        }
    }

    // [COMMENTED] 4. HX711 Load Cell Initialization
    // scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
    // if (scale.wait_ready_timeout(1000)) {
    //     scale_available = true;
    //     scale.set_scale(scale_calibration_factor);
    //     scale.tare(10);
    // }

    // 4. Connect WiFi
    Serial.print(F("  [..] WiFi Connecting"));
    connectWiFi();

    printLine('=');
    Serial.println();
}

// =====================================================================
//  Main Loop
// =====================================================================
void loop() {
    connectWiFi();

    unsigned long now = millis();
    if (now - lastLog < INTERVAL) { delay(50); return; }
    lastLog = now;
    loopCount++;

    // -------------------------------------------------------------
    // 1. Read DHT11 Temperature & Humidity
    // -------------------------------------------------------------
    float hum  = dht.readHumidity();
    float tC   = dht.readTemperature();
    float tF   = dht.readTemperature(true);
    bool  dhtOK = true;

    if (isnan(hum) || isnan(tC)) {
        hum = 50.0f; tC = 25.0f; tF = 77.0f;
        dhtOK = false;
    }
    float hi = dht.computeHeatIndex(tC, hum, false);

    // -------------------------------------------------------------
    // 2. Read BH1750 Light Sensor (with Master Calibration)
    // -------------------------------------------------------------
    float rawLux = 0.0f;
    float lux = 0.0f;
    if (bh1750_available) {
        float r = lightMeter.readLightLevel();
        if (r >= 0) {
            rawLux = r;
            lux = rawLux * LIGHT_CAL_FACTOR;
            currentLux = lux;
        }
    }
    const char* lightStatus = getLightStatus(lux);

    // -------------------------------------------------------------
    // 3. Read Capacitive Soil Moisture Sensor v2.0
    // -------------------------------------------------------------
    int soilADC = 0;
    float soilVoltage = 0.0f;
    float soilMoisturePct = readSoilMoisture(soilADC, soilVoltage);
    const char* soilStatus = getSoilStatus(soilMoisturePct);

    // [COMMENTED] 4. Read HX711 5kg Weight Sensor
    // long rawWeightADC = 0;
    // float weightGrams = readWeight(rawWeightADC);

    // =============================================================
    //  Professional Serial Dashboard
    // =============================================================
    Serial.println();
    printLine('=');
    Serial.printf("  MODULE 3 | READING #%-4lu | UPTIME: %s | WiFi: %s\n",
        loopCount, uptime().c_str(),
        WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "Disconnected");
    printLine('=');

    // Section 1: Temperature & Humidity
    Serial.println(F("  AMBIENT ENVIRONMENT  [DHT11 - GPIO 4]"));
    printLine();
    if (dhtOK) {
        Serial.printf("    Temperature  :  %5.1f °C   (%5.1f °F)\n", tC, tF);
        Serial.printf("    Humidity     :  %5.1f %%\n", hum);
        Serial.printf("    Heat Index   :  %5.1f °C\n", hi);
    } else {
        Serial.println(F("    [WARNING] DHT11 read failed, using 25.0°C fallback."));
    }
    printLine();

    // Section 2: Light Sensor
    Serial.printf("  LIGHT INTENSITY  [BH1750 - I2C SDA=21 SCL=22]  %s\n",
                  bh1750_available ? "[ONLINE]" : "[OFFLINE]");
    printLine();
    if (bh1750_available) {
        Serial.printf("    Illuminance  :  %8.1f lx   [%s]\n", lux, lightStatus);
        Serial.printf("    Raw Sensor   :  %8.1f lx   (Calibrated: x%.4f)\n", rawLux, LIGHT_CAL_FACTOR);
        int lightBars = min((int)(lux / 200.0f), 30);
        Serial.print(F("    Light Bar    :  ["));
        for (int i = 0; i < lightBars; i++) Serial.print('#');
        for (int i = lightBars; i < 30; i++) Serial.print(' ');
        Serial.printf("] %.0f lx\n", lux);
    } else {
        Serial.println(F("    [ERR] Sensor not detected. Check wiring: SDA=21, SCL=22, ADDR=GND"));
    }
    printLine();

    // Section 3: Soil Moisture Sensor
    Serial.println(F("  SOIL MOISTURE  [Capacitive v2.0 - GPIO 34]"));
    printLine();
    Serial.printf("    Moisture     :  %5.1f %%      [%s]\n", soilMoisturePct, soilStatus);
    Serial.printf("    Analog ADC   :  %5d / 4095  (Air ~%d, Water ~%d)\n", soilADC, AIR_VALUE, WATER_VALUE);
    Serial.printf("    Sensor Volt  :  %5.3f V\n", soilVoltage);

    int soilBars = min((int)(soilMoisturePct / 4.0f), 25);
    Serial.print(F("    Moisture Bar :  ["));
    for (int i = 0; i < soilBars; i++) Serial.print('#');
    for (int i = soilBars; i < 25; i++) Serial.print(' ');
    Serial.printf("] %.1f %%\n", soilMoisturePct);
    printLine();

    // Section 4: Cloud Telemetry
    Serial.print(F("  CLOUD -> ThingsBoard Telemetry (10 keys) ..."));
    sendTelemetry(tC, tF, hum, hi, lux, lightStatus, soilMoisturePct, soilVoltage, soilADC, soilStatus);

    printLine('=');
}
