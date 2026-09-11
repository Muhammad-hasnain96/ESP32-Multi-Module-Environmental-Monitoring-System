// =====================================================================
// ESP32 — MODULE 2: Complete Water Quality, pH & Flow Monitoring System
// =====================================================================
// SENSOR WIRING:
//   1. DHT11        : DATA   -> GPIO 4  (VCC -> 3.3V/5V, GND -> GND)
//   2. Analog TDS   : AOUT   -> GPIO 34 (VCC -> 3.3V/5V, GND -> GND)
//   3. Flow Sensor  : Signal -> GPIO 27 (VCC -> 5V VIN, GND -> GND)
//   4. E-201-C pH   : Po     -> GPIO 35 (VCC -> 5V VIN, GND -> GND)
// =====================================================================

#include <Arduino.h>
#include <DHT.h>
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
//  2. Analog TDS Meter Configuration (GPIO 34 - ADC1_CH6)
// =====================================================================
#define TDS_PIN          34
#define VREF             3.3f   // ESP32 ADC Reference Voltage
#define ADC_RESOLUTION   4095.0f

// =====================================================================
//  3. Water Flow Sensor Configuration (GPIO 27 - Interrupt)
// =====================================================================
#define FLOW_PIN         27
const float FLOW_CAL_FACTOR  = 7.5f;   // Pulses per second per L/min
const float PULSES_PER_LITER = 450.0f; // 7.5 * 60 = 450 pulses per liter

volatile unsigned long flowPulseCount = 0;
float totalLiters = 0.0f;

void IRAM_ATTR flowPulseISR() {
    flowPulseCount++;
}

// =====================================================================
//  4. E-201-C BNC pH Sensor Configuration (GPIO 35 - ADC1_CH7)
// =====================================================================
#define PH_PIN           35

// 2-Point Calibrated Constants:
// Neutral Water: 1.160V -> pH 7.00
// Vinegar:       1.995V -> pH 2.80
const float PH_NEUTRAL_V = 1.160f;
const float PH_SLOPE     = 5.03f; // (7.0 - 2.8) / (1.995 - 1.160) = 5.03 pH/V

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
const unsigned long INTERVAL = 3000UL; // 3 seconds interval
unsigned long lastLog = 0;
unsigned long loopCount = 0;

// =====================================================================
//  Helpers — Formatting & Timing
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

// =====================================================================
//  Status Labels
// =====================================================================
const char* getWaterQuality(float tds) {
    if (tds <= 5.0f)   return "DRY PROBE / AIR (0 ppm)";
    if (tds < 50.0f)   return "RO / PURE WATER (Very Low Minerals)";
    if (tds < 150.0f)  return "EXCELLENT (Ideal Drinking Water)";
    if (tds < 300.0f)  return "GOOD (Normal Tap / Filtered Water)";
    if (tds < 500.0f)  return "FAIR (Hard Water / High Minerals)";
    return                    "POOR (Not Recommended for Drinking)";
}

const char* getFlowStatus(float flowRate) {
    if (flowRate <= 0.05f) return "NO FLOW (Idle)";
    if (flowRate < 2.0f)   return "LOW FLOW (Trickle)";
    if (flowRate < 10.0f)  return "NORMAL FLOW (Active)";
    return                        "HIGH FLOW (Strong Stream)";
}

const char* getPhStatus(float ph) {
    if (ph <  3.0f) return "STRONGLY ACIDIC (Hazardous / Acid)";
    if (ph <  6.5f) return "ACIDIC (Low pH / Corrosive)";
    if (ph <= 7.5f) return "NEUTRAL (Ideal Drinking / Potable Water)";
    if (ph <= 8.5f) return "MILDLY ALKALINE (Safe / Mineral-Rich)";
    if (ph <= 11.0f) return "ALKALINE (Basic / Soapy)";
    return                 "STRONGLY ALKALINE (Hazardous / Caustic)";
}

// =====================================================================
//  Analog TDS Reader (30-Sample Median Noise Filter + Temp Compensation)
// =====================================================================
float readTDS(float currentTempC, float &outVoltage) {
    const int SAMPLES = 30;
    int buffer[SAMPLES];

    for (int i = 0; i < SAMPLES; i++) {
        buffer[i] = analogRead(TDS_PIN);
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
    float avgAdc = sum / 10.0f;
    outVoltage = (avgAdc / ADC_RESOLUTION) * VREF;

    if (outVoltage < 0.03f) {
        return 0.0f;
    }

    float temp = (currentTempC > 0 && currentTempC < 80) ? currentTempC : 25.0f;
    float tempCoeff = 1.0f + 0.02f * (temp - 25.0f);
    float compVoltage = outVoltage / tempCoeff;

    float tdsValue = (133.42f * compVoltage * compVoltage * compVoltage
                    - 255.86f * compVoltage * compVoltage
                    + 857.39f * compVoltage) * 0.5f;

    return max(tdsValue, 0.0f);
}

// =====================================================================
//  Analog pH Reader (30-Sample Median Noise Filter)
// =====================================================================
float readPH(float &outVoltage, int &outRawADC) {
    const int SAMPLES = 30;
    int buffer[SAMPLES];

    for (int i = 0; i < SAMPLES; i++) {
        buffer[i] = analogRead(PH_PIN);
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
    outRawADC = (int)(sum / 10);
    outVoltage = (outRawADC / ADC_RESOLUTION) * VREF;

    float calculatedPH = 7.0f - (outVoltage - PH_NEUTRAL_V) * PH_SLOPE;
    return constrain(calculatedPH, 0.0f, 14.0f);
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
//  ThingsBoard Telemetry Dispatch (Module 2: Complete Telemetry Keys)
// =====================================================================
void sendTelemetry(float tC, float tF, float hum, float hi,
                   float tds, float tdsV, const char* quality,
                   float flowRateLMin, float flowRateMLSec, float flowHz,
                   float volTotal, unsigned long pulses, const char* flowStatus,
                   float ph, float phV, const char* phStatus) {
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
    
    // JSON Payload (With all aliases for versatile widget compatibility)
    String p = "{";
    // DHT11
    p += "\"temperature\":"        + String(tC, 1);
    p += ",\"temperatureF\":"      + String(tF, 1);
    p += ",\"humidity\":"          + String(hum, 1);
    p += ",\"heatIndex\":"         + String(hi, 1);
    p += ",\"m2_temperature\":"    + String(tC, 1);
    p += ",\"m2_humidity\":"       + String(hum, 1);
    // TDS Meter
    p += ",\"tdsPPM\":"            + String(tds, 1);
    p += ",\"tdsValue\":"          + String(tds, 1);
    p += ",\"tdsVoltage\":"        + String(tdsV, 3);
    p += ",\"waterQuality\":\""     + String(quality) + "\"";
    p += ",\"tdsStatus\":\""       + String(quality) + "\"";
    // Flow Sensor
    p += ",\"flowRateLMin\":"      + String(flowRateLMin, 2);
    p += ",\"flowRate\":"          + String(flowRateLMin, 2);
    p += ",\"flowRateMLSec\":"     + String(flowRateMLSec, 1);
    p += ",\"flowFrequencyHz\":"   + String(flowHz, 2);
    p += ",\"totalVolumeLiters\":" + String(volTotal, 3);
    p += ",\"totalLitres\":"       + String(volTotal, 3);
    p += ",\"flowPulses\":"        + String(pulses);
    p += ",\"flowStatus\":\""       + String(flowStatus) + "\"";
    // pH Sensor
    p += ",\"phValue\":"           + String(ph, 2);
    p += ",\"phVoltage\":"         + String(phV, 3);
    p += ",\"phStatus\":\""        + String(phStatus) + "\"";
    p += ",\"m2_phValue\":"        + String(ph, 2);
    p += ",\"m2_phStatus\":\""     + String(phStatus) + "\"";
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

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db); // Full range 0 - 3.3V

    printLine('=');
    Serial.println(F("  ESP32 — MODULE 2: WATER QUALITY, pH & FLOW MONITOR"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | TDS (GPIO 34)"));
    Serial.println(F("           Flow (GPIO 27) | E-201-C pH (GPIO 35)"));
    Serial.println(F("  Cloud  : ThingsBoard"));
    printLine('=');

    // 1. TDS ADC
    pinMode(TDS_PIN, INPUT);
    Serial.println(F("  [OK] TDS Meter        -> GPIO 34 (ADC1_CH6)"));

    // 2. pH ADC
    pinMode(PH_PIN, INPUT);
    Serial.println(F("  [OK] E-201-C pH Meter -> GPIO 35 (ADC1_CH7)"));

    // 3. DHT11
    dht.begin();
    Serial.println(F("  [OK] DHT11            -> GPIO 4"));

    // 4. Flow Sensor Interrupt
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);
    Serial.println(F("  [OK] Water Flow Sensor -> GPIO 27 (Interrupt)"));

    // 5. WiFi
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
    float elapsedSec = (now - lastLog) / 1000.0f;
    lastLog = now;
    loopCount++;

    // -------------------------------------------------------------
    // 1. Water Flow Sensor (Atomic Read)
    // -------------------------------------------------------------
    noInterrupts();
    unsigned long pulses = flowPulseCount;
    flowPulseCount = 0;
    interrupts();

    float flowHz = pulses / elapsedSec;
    float flowRateLMin = flowHz / FLOW_CAL_FACTOR;
    float flowRateMLSec = (flowRateLMin * 1000.0f) / 60.0f;
    float addedVolume = pulses / PULSES_PER_LITER;
    totalLiters += addedVolume;
    const char* flowStatus = getFlowStatus(flowRateLMin);

    // -------------------------------------------------------------
    // 2. DHT11 Read
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
    // 3. TDS Sensor (with live DHT11 temperature compensation)
    // -------------------------------------------------------------
    float tdsVoltage = 0.0f;
    float tdsPPM = readTDS(tC, tdsVoltage);
    const char* qualityStr = getWaterQuality(tdsPPM);

    // -------------------------------------------------------------
    // 4. E-201-C pH Sensor
    // -------------------------------------------------------------
    float phVoltage = 0.0f;
    int   phRawADC = 0;
    float phValue = readPH(phVoltage, phRawADC);
    const char* phStatus = getPhStatus(phValue);

    // =============================================================
    //  Professional Serial Dashboard
    // =============================================================
    Serial.println();
    printLine('=');
    Serial.printf("  MODULE 2 | READING #%-4lu | UPTIME: %s | WiFi: %s\n",
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

    // Section 2: TDS Water Quality
    Serial.println(F("  WATER QUALITY  [Analog TDS Meter - GPIO 34]"));
    printLine();
    Serial.printf("    TDS Value    :  %6.1f ppm   [%s]\n", tdsPPM, qualityStr);
    Serial.printf("    Sensor Volt  :  %6.3f V\n", tdsVoltage);
    printLine();

    // Section 3: pH Measurement
    Serial.println(F("  WATER pH LEVEL  [E-201-C BNC - GPIO 35]"));
    printLine();
    Serial.printf("    pH Value     :  %6.2f       [%s]\n", phValue, phStatus);
    Serial.printf("    Sensor Volt  :  %6.3f V    (Raw ADC: %4d)\n", phVoltage, phRawADC);
    printLine();

    // Section 4: Water Flow Sensor
    Serial.println(F("  WATER FLOW MONITOR  [FS200A / YF-S201 - GPIO 27]"));
    printLine();
    Serial.printf("    Flow Rate    :  %6.2f L/min   (%5.1f mL/sec)\n", flowRateLMin, flowRateMLSec);
    Serial.printf("    Frequency    :  %6.2f Hz      (%lu pulses in 3s)\n", flowHz, pulses);
    Serial.printf("    Total Volume :  %6.3f Liters\n", totalLiters);
    Serial.printf("    Flow Status  :  %s\n", flowStatus);
    printLine();

    // Section 5: Cloud Telemetry
    Serial.print(F("  CLOUD -> ThingsBoard Telemetry (All Keys) ..."));
    sendTelemetry(tC, tF, hum, hi,
                  tdsPPM, tdsVoltage, qualityStr,
                  flowRateLMin, flowRateMLSec, flowHz,
                  totalLiters, pulses, flowStatus,
                  phValue, phVoltage, phStatus);

    printLine('=');
}
