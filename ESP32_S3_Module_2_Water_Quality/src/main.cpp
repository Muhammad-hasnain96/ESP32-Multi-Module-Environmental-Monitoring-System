// =====================================================================
// ESP32-S3 — MODULE 2: Complete Water Quality, pH & Flow Monitoring System
// With Integrated Dual-Band AP+STA Wi-Fi Utility, Secure Login & Web Dashboard
// =====================================================================
// SENSOR WIRING FOR ESP32-S3:
//   1. DHT11 Sensor:
//      - DATA   -> GPIO 4
//      - VCC    -> 3.3V
//      - GND    -> GND
//
//   2. Analog TDS Meter:
//      - AOUT   -> GPIO 1   (ADC1_CH0 - 12-bit Analog Input)
//      - VCC    -> 3.3V
//      - GND    -> GND
//
//   3. Water Flow Sensor (FS200A / YF-S201):
//      - Signal -> GPIO 5   (Interrupt)
//      - VCC    -> 3.3V / 5V (VIN)
//      - GND    -> GND
//
//   4. E-201-C BNC pH Meter (pH-4502C Module):
//      - Po     -> GPIO 2   (ADC1_CH1 - 12-bit Analog Input)
//      - VCC    -> 5V (VIN) [Requires 5V for op-amp linear headroom]
//      - GND    -> GND (Both Power & Analog ground connected to GND)
//
//   5. 2004 Character LCD (I2C Backpack):
//      - SDA    -> GPIO 17  (Wire: Address 0x27 / 0x3F)
//      - SCL    -> GPIO 18  (Wire)
//      - VCC    -> 5V (VIN) [5V required for contrast]
//      - GND    -> GND
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LiquidCrystal_I2C.h>

// =====================================================================
//  1. DHT11 Configuration (GPIO 4)
// =====================================================================
#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// =====================================================================
//  2. Analog TDS Meter Configuration (GPIO 1 - ADC1_CH0)
// =====================================================================
#define TDS_PIN          1
#define VREF             3.3f   // ESP32-S3 ADC Reference Voltage
#define ADC_RESOLUTION   4095.0f

// In dry air, the module's op-amp output sits at a baseline leakage of ~0.18V - 0.25V.
// Any voltage at or below TDS_DRY_VOLTAGE_THRESHOLD is treated as dry air -> 0.0 ppm.
const float TDS_DRY_VOLTAGE_THRESHOLD = 0.26f;

// =====================================================================
//  3. Water Flow Sensor Configuration (GPIO 5 - Interrupt)
// =====================================================================
#define FLOW_PIN         5
const float FLOW_CAL_FACTOR  = 7.5f;   // Pulses per second per L/min
const float PULSES_PER_LITER = 450.0f; // 7.5 * 60 = 450 pulses per liter

volatile unsigned long flowPulseCount = 0;
float totalLiters = 0.0f;

void IRAM_ATTR flowPulseISR() {
    flowPulseCount++;
}

// =====================================================================
//  4. E-201-C BNC pH Sensor Configuration (GPIO 2 - ADC1_CH1)
// =====================================================================
#define PH_PIN           2

// 2-Point Calibrated Constants:
// Neutral Water: 1.160V -> pH 7.00
// Vinegar:       1.995V -> pH 2.80
const float PH_NEUTRAL_V = 1.160f;
const float PH_SLOPE     = 5.03f; // (7.0 - 2.8) / (1.995 - 1.160) = 5.03 pH/V

// =====================================================================
//  5. 2004 Character LCD (Dedicated Wire: SDA=17, SCL=18)
// =====================================================================
#define LCD_SDA_PIN      17
#define LCD_SCL_PIN      18
LiquidCrystal_I2C lcd(0x27, 20, 4);
bool lcd_available = false;
int lcdScreenPage = 0; // 0 = Env & pH (Page 1), 1 = TDS & Flow (Page 2)
const unsigned long LCD_PAGE_INTERVAL = 5000UL;
unsigned long lastLcdSwitch = 0;

// =====================================================================
//  WiFi & ThingsBoard Configuration
// =====================================================================
const char* AP_SSID = "ESP32-S3_Water_Quality";
const char* AP_PASS = "12345678";

Preferences prefWifi;
Preferences prefAuth;

String wifi_ssid = "Meeting Room";
String wifi_pass = "Shahid786$$";
String auth_user = "admin";
String auth_pass = "admin";

const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";

WebServer server(80);

// =====================================================================
//  Global Sensor State & Timing Variables
// =====================================================================
const unsigned long INTERVAL = 3000UL; // 3.0s interval between sensor reads & telemetry
unsigned long lastLog = 0;
unsigned long loopCount = 0;

int   lastTbHttpCode = 0;
unsigned long tbSendCount = 0;

// Live cached sensor states
bool  currentDhtOk     = false;
float currentTC        = 0.0f;
float currentTF        = 0.0f;
float currentHum       = 0.0f;
float currentHI        = 0.0f;

bool  currentTdsOk     = false;
float currentTdsPPM    = 0.0f;
float currentTdsV      = 0.0f;
int   currentTdsRaw    = 0;

bool  currentPhOk      = false;
float currentPhValue   = 7.0f;
float currentPhV       = 0.0f;
int   currentPhRaw     = 0;

float currentFlowHz    = 0.0f;
float currentFlowLMin  = 0.0f;
float currentFlowMLSec = 0.0f;

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
    if (ph <  3.0f)  return "STRONGLY ACIDIC (Hazardous / Acid)";
    if (ph <  6.5f)  return "ACIDIC (Low pH / Corrosive)";
    if (ph <= 7.5f)  return "NEUTRAL (Ideal Drinking / Potable Water)";
    if (ph <= 8.5f)  return "MILDLY ALKALINE (Safe / Mineral-Rich)";
    if (ph <= 11.0f) return "ALKALINE (Basic / Soapy)";
    return                  "STRONGLY ALKALINE (Hazardous / Caustic)";
}

const char* shortPhLabel(float ph) {
    if (ph <  3.0f)  return "HI-ACID";
    if (ph <  6.5f)  return "ACIDIC ";
    if (ph <= 7.5f)  return "NEUTRAL";
    if (ph <= 8.5f)  return "SLT ALK";
    if (ph <= 11.0f) return "ALKALIN";
    return                  "HI-ALKA";
}

const char* shortTdsLabel(float tds) {
    if (tds <= 5.0f)   return "DRY ";
    if (tds < 50.0f)   return "PURE";
    if (tds < 150.0f)  return "EXCL";
    if (tds < 300.0f)  return "GOOD";
    if (tds < 500.0f)  return "FAIR";
    return                    "POOR";
}

// =====================================================================
//  Analog Pin Open-Circuit / Floating Pin Detector
// =====================================================================
bool isAnalogSensorConnected(int pin) {
    gpio_reset_pin((gpio_num_t)pin);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    delay(10);
    pinMode(pin, INPUT);
    delay(3);

    int discharged = analogRead(pin);

    if (discharged < 350) {
        return false; // Disconnected / Floating open pin
    }
    return true; // Actively driven by real sensor
}

// =====================================================================
//  Analog TDS Reader (30-Sample Median Noise Filter + Disconnect Check)
// =====================================================================
float readTDS(float currentTempC, float &outVoltage, int &outRawADC, bool &outConnected) {
    if (!isAnalogSensorConnected(TDS_PIN)) {
        outConnected = false;
        outVoltage = 0.0f;
        outRawADC = 0;
        return 0.0f;
    }

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
    outRawADC = (int)(sum / 10);
    outVoltage = (outRawADC / ADC_RESOLUTION) * VREF;

    if (outVoltage < 0.08f) {
        outConnected = false;
        return 0.0f;
    }

    outConnected = true;

    // Check for dry probe in air
    if (outVoltage <= TDS_DRY_VOLTAGE_THRESHOLD) {
        return 0.0f;
    }

    float temp = (currentTempC > 0 && currentTempC < 80) ? currentTempC : 25.0f;
    float tempCoeff = 1.0f + 0.02f * (temp - 25.0f);
    float compVoltage = outVoltage / tempCoeff;

    float tdsValue = (133.42f * compVoltage * compVoltage * compVoltage
                    - 255.86f * compVoltage * compVoltage
                    + 857.39f * compVoltage) * 0.5f;

    if (tdsValue < 5.0f) {
        tdsValue = 0.0f;
    }

    return max(tdsValue, 0.0f);
}

// =====================================================================
//  Analog pH Reader (30-Sample Median Noise Filter + Disconnect Check)
// =====================================================================
float readPH(float &outVoltage, int &outRawADC, bool &outConnected) {
    if (!isAnalogSensorConnected(PH_PIN)) {
        outConnected = false;
        outVoltage = 0.0f;
        outRawADC = 0;
        return 0.0f;
    }

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

    if (outVoltage < 0.25f || outVoltage > 3.05f) {
        outConnected = false;
        return 0.0f;
    }

    outConnected = true;
    float calculatedPH = 7.0f - (outVoltage - PH_NEUTRAL_V) * PH_SLOPE;
    return constrain(calculatedPH, 0.0f, 14.0f);
}

// =====================================================================
//  ThingsBoard Telemetry Dispatch (Module 2: Complete Telemetry Keys)
// =====================================================================
void sendTelemetry(bool dhtOK, float tC, float tF, float hum, float hi,
                   bool tdsOK, float tds, float tdsV, const char* quality,
                   float flowRateLMin, float flowRateMLSec, float flowHz,
                   float volTotal, unsigned long pulses, const char* flowStatus,
                   bool phOK, float ph, float phV, const char* phStatus) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(8000);

    String url = String(TB_HOST) + "/api/v1/" + TB_TOKEN + "/telemetry";
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");

    String p = "{";

    // 1. DHT11 Ambient Temperature & Humidity
    if (dhtOK) {
        p += "\"m2_temperature\":"    + String(tC, 1);
        p += ",\"m2_temperatureF\":"  + String(tF, 1);
        p += ",\"m2_humidity\":"      + String(hum, 1);
        p += ",\"m2_heatIndex\":"     + String(hi, 1);
        p += ",\"temperature\":"      + String(tC, 1);
        p += ",\"temperatureF\":"     + String(tF, 1);
        p += ",\"temp\":"             + String(tC, 1);
        p += ",\"tempF\":"            + String(tF, 1);
        p += ",\"humidity\":"         + String(hum, 1);
        p += ",\"heatIndex\":"        + String(hi, 1);
        p += ",\"heat_index\":"       + String(hi, 1);
        p += ",\"m2_temp\":"          + String(tC, 1);
        p += ",\"m2_hum\":"           + String(hum, 1);
        p += ",\"water_temp\":"       + String(tC, 1);
        p += ",\"water_humidity\":"   + String(hum, 1);
    } else {
        p += "\"m2_temperature\":\"OFF\"";
        p += ",\"m2_temperatureF\":\"OFF\"";
        p += ",\"m2_humidity\":\"OFF\"";
        p += ",\"m2_heatIndex\":\"OFF\"";
        p += ",\"temperature\":\"OFF\"";
        p += ",\"temperatureF\":\"OFF\"";
        p += ",\"temp\":\"OFF\"";
        p += ",\"tempF\":\"OFF\"";
        p += ",\"humidity\":\"OFF\"";
        p += ",\"heatIndex\":\"OFF\"";
        p += ",\"heat_index\":\"OFF\"";
        p += ",\"m2_temp\":\"OFF\"";
        p += ",\"m2_hum\":\"OFF\"";
        p += ",\"water_temp\":\"OFF\"";
        p += ",\"water_humidity\":\"OFF\"";
    }

    // 2. TDS Meter
    if (tdsOK) {
        p += ",\"tdsPPM\":"            + String(tds, 1);
        p += ",\"tdsValue\":"          + String(tds, 1);
        p += ",\"tdsVoltage\":"        + String(tdsV, 3);
        p += ",\"waterQuality\":\""     + String(quality) + "\"";
        p += ",\"tdsStatus\":\""       + String(quality) + "\"";
    } else {
        p += ",\"tdsPPM\":\"OFF\"";
        p += ",\"tdsValue\":\"OFF\"";
        p += ",\"tdsVoltage\":\"OFF\"";
        p += ",\"waterQuality\":\"OFF\"";
        p += ",\"tdsStatus\":\"OFF\"";
    }

    // 3. Water Flow Sensor (FS200A)
    p += ",\"flowRateLMin\":"      + String(flowRateLMin, 2);
    p += ",\"flowRate\":"          + String(flowRateLMin, 2);
    p += ",\"flowRateMLSec\":"     + String(flowRateMLSec, 1);
    p += ",\"flowFrequencyHz\":"   + String(flowHz, 2);
    p += ",\"totalVolumeLiters\":" + String(volTotal, 3);
    p += ",\"totalLitres\":"       + String(volTotal, 3);
    p += ",\"flowPulses\":"        + String(pulses);
    p += ",\"flowStatus\":\""       + String(flowStatus) + "\"";

    // 4. pH Sensor
    if (phOK) {
        p += ",\"phValue\":"           + String(ph, 2);
        p += ",\"phVoltage\":"         + String(phV, 3);
        p += ",\"phStatus\":\""        + String(phStatus) + "\"";
        p += ",\"m2_phValue\":"        + String(ph, 2);
        p += ",\"m2_phStatus\":\""     + String(phStatus) + "\"";
        p += ",\"water_status\":\""    + String(phStatus) + "\"";
        p += ",\"Water-status\":\""    + String(phStatus) + "\"";
        p += ",\"waterStatus\":\""     + String(phStatus) + "\"";
    } else {
        p += ",\"phValue\":\"OFF\"";
        p += ",\"phVoltage\":\"OFF\"";
        p += ",\"phStatus\":\"OFF\"";
        p += ",\"m2_phValue\":\"OFF\"";
        p += ",\"m2_phStatus\":\"OFF\"";
        p += ",\"water_status\":\"OFF\"";
        p += ",\"Water-status\":\"OFF\"";
        p += ",\"waterStatus\":\"OFF\"";
    }
    p += "}";

    int code = https.POST(p);
    lastTbHttpCode = code;
    if (code > 0) tbSendCount++;
    https.end();
}

// =====================================================================
//  2004 Character LCD Multi-Screen Renderer
// =====================================================================
void updateLCD() {
    if (!lcd_available) return;
    char buf[21];

    if (lcdScreenPage == 0) {
        // SCREEN 1: Ambient Environment & Water pH
        snprintf(buf, sizeof(buf), "-- ENV & pH  [1/2] -");
        lcd.setCursor(0, 0); lcd.print(buf);

        if (currentDhtOk) {
            snprintf(buf, sizeof(buf), "Temp    : %5.1f C   ", currentTC);
        } else {
            snprintf(buf, sizeof(buf), "Temp    :  OFF      ");
        }
        lcd.setCursor(0, 1); lcd.print(buf);

        if (currentDhtOk) {
            snprintf(buf, sizeof(buf), "Humidity: %5.1f %%   ", currentHum);
        } else {
            snprintf(buf, sizeof(buf), "Humidity:  OFF      ");
        }
        lcd.setCursor(0, 2); lcd.print(buf);

        if (currentPhOk) {
            snprintf(buf, sizeof(buf), "pH : %-4.2f [%-7s] ", currentPhValue, shortPhLabel(currentPhValue));
        } else {
            snprintf(buf, sizeof(buf), "pH : OFF / DISCONN  ");
        }
        lcd.setCursor(0, 3); lcd.print(buf);
    } else {
        // SCREEN 2: TDS Water Quality & Water Flow
        snprintf(buf, sizeof(buf), "-- TDS & FLOW [2/2]-");
        lcd.setCursor(0, 0); lcd.print(buf);

        if (currentTdsOk) {
            snprintf(buf, sizeof(buf), "TDS : %4.0fppm [%-4s]", currentTdsPPM, shortTdsLabel(currentTdsPPM));
        } else {
            snprintf(buf, sizeof(buf), "TDS : OFF / DISCONN ");
        }
        lcd.setCursor(0, 1); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Flow : %5.1f L/min  ", currentFlowLMin);
        lcd.setCursor(0, 2); lcd.print(buf);

        snprintf(buf, sizeof(buf), "Total: %6.2f Liters", totalLiters);
        lcd.setCursor(0, 3); lcd.print(buf);
    }
}

// =====================================================================
//  Authentication Management
// =====================================================================
const char* AUTH_COOKIE_NAME = "m2_auth";
const char* AUTH_TOKEN_VALUE = "valid_m2_session_786";

bool isClientAuthenticated() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");
        if (cookie.indexOf(String(AUTH_COOKIE_NAME) + "=" + String(AUTH_TOKEN_VALUE)) != -1) {
            return true;
        }
    }
    return false;
}

// =====================================================================
//  HTML Web Pages (Login Screen & Monitoring Dashboard)
// =====================================================================

const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Module 2 Login | ESP32-S3 Water Quality Hub</title>
    <style>
        :root {
            --bg: #0b1329;
            --card-bg: #152238;
            --primary: #06b6d4;
            --primary-hover: #0891b2;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #1e3a5f;
            --danger: #ef4444;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg); color: var(--text-main); min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 16px; }
        .login-card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 16px;
            padding: 32px 24px;
            width: 100%;
            max-width: 380px;
            box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.5), 0 8px 10px -6px rgba(0, 0, 0, 0.5);
            text-align: center;
        }
        .icon { font-size: 3rem; margin-bottom: 12px; }
        h1 { font-size: 1.4rem; font-weight: 700; color: var(--text-main); margin-bottom: 6px; }
        p.sub { font-size: 0.85rem; color: var(--text-muted); margin-bottom: 24px; }
        .form-group { text-align: left; margin-bottom: 16px; }
        label { display: block; font-size: 0.8rem; font-weight: 600; color: var(--text-muted); margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px; }
        input[type="text"], input[type="password"] {
            width: 100%;
            padding: 12px 14px;
            background: #0b1329;
            border: 1px solid var(--border);
            border-radius: 8px;
            color: var(--text-main);
            font-size: 0.95rem;
            outline: none;
            transition: border-color 0.2s;
        }
        input:focus { border-color: var(--primary); }
        .btn {
            width: 100%;
            padding: 12px;
            background: var(--primary);
            color: #0b1329;
            border: none;
            border-radius: 8px;
            font-size: 1rem;
            font-weight: 700;
            cursor: pointer;
            margin-top: 8px;
            transition: background 0.2s;
        }
        .btn:hover { background: var(--primary-hover); color: #fff; }
        .error-msg {
            color: var(--danger);
            font-size: 0.82rem;
            margin-top: 12px;
            display: none;
            font-weight: 600;
        }
        .hint {
            margin-top: 20px;
            font-size: 0.75rem;
            color: #64748b;
        }
    </style>
</head>
<body>
    <div class="login-card">
        <div class="icon">🌊</div>
        <h1>ESP32-S3 Module 2</h1>
        <p class="sub">Water Quality, pH & Flow Hub</p>

        <form id="loginForm" onsubmit="handleLogin(event)">
            <div class="form-group">
                <label for="username">Username</label>
                <input type="text" id="username" placeholder="admin" required autofocus>
            </div>
            <div class="form-group">
                <label for="password">Password</label>
                <input type="password" id="password" placeholder="••••••••" required>
            </div>
            <button type="submit" class="btn" id="submitBtn">Sign In</button>
            <div class="error-msg" id="errMsg">❌ Invalid username or password</div>
        </form>
        <div class="hint">Default Credentials: <b>admin</b> / <b>admin</b></div>
    </div>

    <script>
        function handleLogin(e) {
            e.preventDefault();
            const u = document.getElementById('username').value.trim();
            const p = document.getElementById('password').value;
            const err = document.getElementById('errMsg');
            const btn = document.getElementById('submitBtn');

            btn.disabled = true;
            btn.innerText = 'Verifying...';
            err.style.display = 'none';

            const data = new URLSearchParams();
            data.append('username', u);
            data.append('password', p);

            fetch('/login', { method: 'POST', body: data })
                .then(r => {
                    if (r.ok) {
                        window.location.href = '/';
                    } else {
                        err.style.display = 'block';
                        btn.disabled = false;
                        btn.innerText = 'Sign In';
                    }
                })
                .catch(() => {
                    err.innerText = 'Network error. Try again.';
                    err.style.display = 'block';
                    btn.disabled = false;
                    btn.innerText = 'Sign In';
                });
        }
    </script>
</body>
</html>
)rawliteral";

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-S3 Module 2 | Water Quality & Flow Monitor</title>
    <style>
        :root {
            --bg: #090e1d;
            --card-bg: #131e33;
            --primary: #06b6d4;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #1e3a5f;
            --emerald: #10b981;
            --amber: #f59e0b;
            --purple: #a855f7;
            --danger: #ef4444;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg); color: var(--text-main); padding: 16px; min-height: 100vh; }
        .container { max-width: 1100px; margin: 0 auto; }

        /* Navbar */
        .navbar {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 12px 18px;
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 12px;
            margin-bottom: 20px;
        }
        .nav-brand { font-size: 1.15rem; font-weight: 700; display: flex; align-items: center; gap: 8px; }
        .nav-brand span { color: var(--primary); }
        .btn-logout {
            padding: 7px 14px;
            background: #1e293b;
            color: #e2e8f0;
            border: 1px solid var(--border);
            border-radius: 6px;
            font-size: 0.85rem;
            cursor: pointer;
            transition: all 0.2s;
        }
        .btn-logout:hover { background: var(--danger); color: #fff; border-color: var(--danger); }

        /* Banner */
        .banner {
            display: flex;
            flex-wrap: wrap;
            gap: 12px;
            background: rgba(6, 182, 212, 0.08);
            border: 1px solid rgba(6, 182, 212, 0.3);
            border-radius: 12px;
            padding: 12px 16px;
            margin-bottom: 20px;
            font-size: 0.85rem;
        }
        .banner-pill { display: flex; align-items: center; gap: 6px; }
        .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--primary); display: inline-block; }
        .dot.pulse { box-shadow: 0 0 8px var(--primary); animation: pulse 2s infinite; }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }

        /* Grid */
        .grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }
        .card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 14px;
            padding: 18px;
            display: flex;
            flex-direction: column;
            justify-content: space-between;
            position: relative;
            overflow: hidden;
            box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.2);
        }
        .card-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; }
        .card-title { font-size: 0.85rem; text-transform: uppercase; font-weight: 700; color: var(--text-muted); letter-spacing: 0.5px; }
        .card-icon { font-size: 1.4rem; }
        .card-value { font-size: 2.2rem; font-weight: 800; color: #ffffff; line-height: 1.1; margin-bottom: 6px; }
        .card-unit { font-size: 1rem; font-weight: 500; color: var(--text-muted); margin-left: 4px; }
        .card-status { font-size: 0.78rem; font-weight: 600; padding: 3px 8px; border-radius: 6px; width: fit-content; }
        .status-optimal { background: rgba(16, 185, 129, 0.2); color: var(--emerald); }
        .status-warn { background: rgba(245, 158, 11, 0.2); color: var(--amber); }
        .status-info { background: rgba(6, 182, 212, 0.2); color: var(--primary); }
        .status-purple { background: rgba(168, 85, 247, 0.2); color: var(--purple); }
        .status-err { background: rgba(239, 68, 68, 0.2); color: var(--danger); }

        .progress-bar {
            width: 100%;
            height: 6px;
            background: #090e1d;
            border-radius: 3px;
            margin-top: 10px;
            overflow: hidden;
        }
        .progress-fill { height: 100%; border-radius: 3px; transition: width 0.5s ease; }
        .fill-blue { background: var(--primary); }
        .fill-emerald { background: var(--emerald); }
        .fill-amber { background: var(--amber); }
        .fill-purple { background: var(--purple); }

        /* Wi-Fi Utility Card */
        .wifi-section {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 14px;
            padding: 20px;
        }
        .wifi-header { font-size: 1.1rem; font-weight: 700; margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }
        .wifi-grid { display: grid; grid-template-columns: 1fr 1fr auto; gap: 10px; align-items: end; }
        @media (max-width: 650px) { .wifi-grid { grid-template-columns: 1fr; } }
        .input-group label { display: block; font-size: 0.75rem; font-weight: 600; color: var(--text-muted); margin-bottom: 5px; text-transform: uppercase; }
        .input-group input, .input-group select {
            width: 100%;
            padding: 10px 12px;
            background: #090e1d;
            border: 1px solid var(--border);
            border-radius: 8px;
            color: #fff;
            font-size: 0.9rem;
            outline: none;
        }
        .input-group input:focus { border-color: var(--primary); }
        .btn-action {
            padding: 10px 18px;
            background: var(--primary);
            color: #090e1d;
            border: none;
            border-radius: 8px;
            font-weight: 700;
            font-size: 0.9rem;
            cursor: pointer;
            white-space: nowrap;
            height: 41px;
        }
        .btn-action:hover { background: #0891b2; color: #fff; }
        .btn-scan { background: #6366f1; color:#fff; }
        .btn-scan:hover { background: #4f46e5; }
        .wifi-status-text { margin-top: 12px; font-size: 0.82rem; color: var(--text-muted); }
    </style>
</head>
<body>
    <div class="container">
        <!-- Navbar -->
        <div class="navbar">
            <div class="nav-brand">🌊 <span>Module 2</span> Water Quality & Flow Hub</div>
            <button class="btn-logout" onclick="window.location.href='/logout'">Sign Out</button>
        </div>

        <!-- Status Banner -->
        <div class="banner">
            <div class="banner-pill">
                <span class="dot pulse"></span>
                <span><b>AP Mode:</b> ESP32-S3_Water_Quality (192.168.4.1)</span>
            </div>
            <div class="banner-pill">
                <span><b>WiFi STA:</b> <span id="staStatus">Connecting...</span></span>
            </div>
            <div class="banner-pill">
                <span><b>Uptime:</b> <span id="sysUptime">--:--:--</span></span>
            </div>
        </div>

        <!-- Live Sensor Grid -->
        <div class="grid">
            <!-- 1. Ambient Temperature & Humidity -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Ambient Temp & Hum</span>
                    <span class="card-icon">🌡️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valTemp">--.-</span><span class="card-unit">°C</span></div>
                    <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:8px;">
                        Humidity: <b style="color:#fff;" id="valHum">--.-</b> % | HeatIdx: <b style="color:#fff;" id="valHi">--.-</b> °C
                    </div>
                </div>
                <div class="card-status status-info" id="statusDht">DHT11 (GPIO 4)</div>
            </div>

            <!-- 2. Water TDS (Total Dissolved Solids) -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">TDS Water Quality</span>
                    <span class="card-icon">💧</span>
                </div>
                <div>
                    <div class="card-value"><span id="valTds">---</span><span class="card-unit">ppm</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-emerald" id="barTds" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="statusTds">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="tdsVoltTxt">GPIO 1 (0.00 V)</span>
                </div>
            </div>

            <!-- 3. Water pH Level -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Water pH Level</span>
                    <span class="card-icon">🧪</span>
                </div>
                <div>
                    <div class="card-value"><span id="valPh">--.-</span><span class="card-unit">pH</span></div>
                    <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:8px;">Probe: <span id="valPhV">--.-</span> V</div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center;">
                    <div class="card-status status-optimal" id="statusPh">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="phAlertTxt">E-201-C (GPIO 2)</span>
                </div>
            </div>

            <!-- 4. Water Flow Rate & Volume -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Water Flow & Liters</span>
                    <span class="card-icon">🌊</span>
                </div>
                <div>
                    <div class="card-value"><span id="valFlow">--.-</span><span class="card-unit">L/min</span></div>
                    <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:8px;">
                        Total Volume: <b style="color:#fff;" id="valVol">--.-</b> L | Freq: <span id="valFlowHz">--.-</span> Hz
                    </div>
                </div>
                <div class="card-status status-info" id="statusFlow">FS200A (GPIO 5)</div>
            </div>

            <!-- 5. Cloud Telemetry -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">ThingsBoard Cloud</span>
                    <span class="card-icon">☁️</span>
                </div>
                <div>
                    <div style="font-size:1.1rem; font-weight:700; color:#fff; margin-bottom:4px;">DigitalM Cloud</div>
                    <div style="font-size:0.8rem; color:var(--text-muted);">Token: 52kqr3ax...</div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="tbStatus">HTTP --</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="tbCount">Sent: 0</span>
                </div>
            </div>
        </div>

        <!-- Wi-Fi Utility Management -->
        <div class="wifi-section">
            <div class="wifi-header">📶 Wi-Fi Station Utility & Configuration</div>
            <div style="margin-bottom:14px; display:flex; gap:10px; align-items:center;">
                <button class="btn-action btn-scan" id="btnScan" onclick="scanNetworks()">🔍 Scan Available Wi-Fi</button>
                <span id="scanMsg" style="font-size:0.8rem; color:var(--text-muted);">Click to discover nearby routers</span>
            </div>
            
            <div class="wifi-grid">
                <div class="input-group">
                    <label for="wifiSsid">Network SSID</label>
                    <input type="text" id="wifiSsid" placeholder="e.g. Home_WiFi">
                </div>
                <div class="input-group">
                    <label for="wifiPass">Password</label>
                    <input type="password" id="wifiPass" placeholder="••••••••">
                </div>
                <div style="display:flex; gap:8px;">
                    <button class="btn-action" onclick="saveWiFi()">💾 Connect & Save</button>
                    <button class="btn-action" style="background:#475569; color:#fff;" onclick="resetWiFi()">🗑️ Reset</button>
                </div>
            </div>
            <div class="wifi-status-text" id="wifiFeedback">ESP32-S3 will auto-reconnect to saved router credentials upon boot.</div>
        </div>
    </div>

    <script>
        function updateDashboard() {
            fetch('/data')
                .then(r => {
                    if (r.status === 401) { window.location.href = '/'; return; }
                    return r.json();
                })
                .then(d => {
                    if (!d) return;
                    document.getElementById('sysUptime').innerText = d.uptime;
                    document.getElementById('staStatus').innerText = d.wifiSta + (d.ip ? ' (' + d.ip + ')' : '');

                    // Temp & Hum
                    if (d.dhtOk) {
                        document.getElementById('valTemp').innerText = d.temp.toFixed(1);
                        document.getElementById('valHum').innerText = d.hum.toFixed(1);
                        document.getElementById('valHi').innerText = d.hi.toFixed(1);
                        document.getElementById('statusDht').innerText = 'DHT11 (GPIO 4)';
                        document.getElementById('statusDht').className = 'card-status status-info';
                    } else {
                        document.getElementById('valTemp').innerText = '--.-';
                        document.getElementById('valHum').innerText = '--.-';
                        document.getElementById('valHi').innerText = '--.-';
                        document.getElementById('statusDht').innerText = 'OFFLINE';
                        document.getElementById('statusDht').className = 'card-status status-err';
                    }

                    // TDS Water Quality
                    if (d.tdsOk && d.tds !== null) {
                        document.getElementById('valTds').innerText = Math.round(d.tds);
                        document.getElementById('statusTds').innerText = d.tdsStatus;
                        document.getElementById('statusTds').className = 'card-status status-optimal';
                        document.getElementById('barTds').style.width = Math.min((d.tds / 500) * 100, 100) + '%';
                        document.getElementById('tdsVoltTxt').innerText = 'GPIO 1 (' + d.tdsV.toFixed(2) + ' V)';
                    } else {
                        document.getElementById('valTds').innerText = '--';
                        document.getElementById('statusTds').innerText = 'OFFLINE';
                        document.getElementById('statusTds').className = 'card-status status-err';
                        document.getElementById('barTds').style.width = '0%';
                        document.getElementById('tdsVoltTxt').innerText = 'GPIO 1 (Disconnected)';
                    }

                    // pH Sensor
                    if (d.phOk && d.ph !== null) {
                        document.getElementById('valPh').innerText = d.ph.toFixed(2);
                        document.getElementById('valPhV').innerText = d.phV.toFixed(2);
                        document.getElementById('statusPh').innerText = d.phStatus;
                        if (d.ph < 6.5) {
                            document.getElementById('statusPh').className = 'card-status status-warn';
                        } else if (d.ph > 8.5) {
                            document.getElementById('statusPh').className = 'card-status status-purple';
                        } else {
                            document.getElementById('statusPh').className = 'card-status status-optimal';
                        }
                        document.getElementById('phAlertTxt').innerText = 'E-201-C (Online)';
                    } else {
                        document.getElementById('valPh').innerText = '--.-';
                        document.getElementById('valPhV').innerText = '--.-';
                        document.getElementById('statusPh').innerText = 'OFFLINE';
                        document.getElementById('statusPh').className = 'card-status status-err';
                        document.getElementById('phAlertTxt').innerText = 'E-201-C (Disconnected)';
                    }

                    // Water Flow
                    document.getElementById('valFlow').innerText = d.flowRate.toFixed(2);
                    document.getElementById('valVol').innerText = d.vol.toFixed(2);
                    document.getElementById('valFlowHz').innerText = d.flowHz.toFixed(1);
                    document.getElementById('statusFlow').innerText = d.flowStatus;

                    // ThingsBoard
                    document.getElementById('tbStatus').innerText = d.tbHttp > 0 ? 'HTTP ' + d.tbHttp : 'Pending';
                    document.getElementById('tbStatus').className = (d.tbHttp === 200) ? 'card-status status-optimal' : 'card-status status-warn';
                    document.getElementById('tbCount').innerText = 'Sent: ' + d.tbSent;
                })
                .catch(e => console.log('Poll error:', e));
        }

        function scanNetworks() {
            const btn = document.getElementById('btnScan');
            const msg = document.getElementById('scanMsg');
            btn.disabled = true;
            btn.innerText = 'Scanning...';
            msg.innerText = 'Searching 2.4GHz spectrum...';

            fetch('/scan')
                .then(r => r.json())
                .then(list => {
                    btn.disabled = false;
                    btn.innerText = '🔍 Scan Available Wi-Fi';
                    if (!list || list.length === 0) {
                        msg.innerText = 'No networks found.';
                        return;
                    }
                    msg.innerHTML = 'Found ' + list.length + ' networks. Select: <select id="netSelect" onchange="document.getElementById(\'wifiSsid\').value=this.value" style="padding:4px 8px; background:#090e1d; color:#fff; border:1px solid #1e3a5f; border-radius:6px; margin-left:6px;"><option value="">-- Choose SSID --</option>' + 
                        list.map(n => '<option value="' + n.ssid + '">' + n.ssid + ' (' + n.rssi + ' dBm)</option>').join('') + '</select>';
                })
                .catch(() => {
                    btn.disabled = false;
                    btn.innerText = '🔍 Scan Available Wi-Fi';
                    msg.innerText = 'Scan failed.';
                });
        }

        function saveWiFi() {
            const s = document.getElementById('wifiSsid').value.trim();
            const p = document.getElementById('wifiPass').value;
            const fb = document.getElementById('wifiFeedback');
            if (!s) { alert('Please enter an SSID'); return; }
            fb.innerText = 'Connecting to ' + s + '...';

            const body = new URLSearchParams();
            body.append('ssid', s);
            body.append('pass', p);

            fetch('/savewifi', { method: 'POST', body: body })
                .then(r => r.text())
                .then(txt => {
                    fb.innerText = '✅ ' + txt;
                })
                .catch(() => { fb.innerText = '❌ Error saving Wi-Fi'; });
        }

        function resetWiFi() {
            if (!confirm('Clear saved Wi-Fi credentials?')) return;
            fetch('/resetwifi', { method: 'POST' })
                .then(r => r.text())
                .then(txt => {
                    document.getElementById('wifiFeedback').innerText = '⚠️ ' + txt;
                });
        }

        // Auto update every 2 seconds
        updateDashboard();
        setInterval(updateDashboard, 2000);
    </script>
</body>
</html>
)rawliteral";

// =====================================================================
//  WebServer Routes & Handlers
// =====================================================================

void handleRoot() {
    if (!isClientAuthenticated()) {
        server.send(200, "text/html", LOGIN_HTML);
    } else {
        server.send(200, "text/html", DASHBOARD_HTML);
    }
}

void handleLogin() {
    if (!server.hasArg("username") || !server.hasArg("password")) {
        server.send(400, "text/plain", "Missing Parameters");
        return;
    }
    String u = server.arg("username");
    String p = server.arg("password");

    if (u == auth_user && p == auth_pass) {
        server.sendHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=" + String(AUTH_TOKEN_VALUE) + "; Path=/; Max-Age=86400");
        server.send(200, "text/plain", "OK");
    } else {
        server.send(401, "text/plain", "Invalid Credentials");
    }
}

void handleLogout() {
    server.sendHeader("Set-Cookie", String(AUTH_COOKIE_NAME) + "=logged_out; Path=/; Max-Age=0");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

void handleData() {
    if (!isClientAuthenticated()) {
        server.send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }

    String json = "{";
    json += "\"uptime\":\""       + uptime() + "\",";
    json += "\"wifiSta\":\""      + String(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected") + "\",";
    json += "\"ip\":\""           + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
    
    // DHT11
    json += "\"dhtOk\":"          + String(currentDhtOk ? "true" : "false") + ",";
    json += "\"temp\":"           + String(currentTC, 1) + ",";
    json += "\"tempF\":"          + String(currentTF, 1) + ",";
    json += "\"hum\":"            + String(currentHum, 1) + ",";
    json += "\"hi\":"             + String(currentHI, 1) + ",";

    // TDS
    json += "\"tdsOk\":"          + String(currentTdsOk ? "true" : "false") + ",";
    if (currentTdsOk) {
        json += "\"tds\":"         + String(currentTdsPPM, 1) + ",";
        json += "\"tdsV\":"        + String(currentTdsV, 3) + ",";
        json += "\"tdsStatus\":\"" + String(getWaterQuality(currentTdsPPM)) + "\",";
    } else {
        json += "\"tds\":null,";
        json += "\"tdsV\":0,";
        json += "\"tdsStatus\":\"OFFLINE\",";
    }

    // pH
    json += "\"phOk\":"           + String(currentPhOk ? "true" : "false") + ",";
    if (currentPhOk) {
        json += "\"ph\":"          + String(currentPhValue, 2) + ",";
        json += "\"phV\":"         + String(currentPhV, 3) + ",";
        json += "\"phStatus\":\""  + String(getPhStatus(currentPhValue)) + "\",";
    } else {
        json += "\"ph\":null,";
        json += "\"phV\":0,";
        json += "\"phStatus\":\"OFFLINE\",";
    }

    // Water Flow
    json += "\"flowRate\":"       + String(currentFlowLMin, 2) + ",";
    json += "\"flowMLSec\":"      + String(currentFlowMLSec, 1) + ",";
    json += "\"flowHz\":"         + String(currentFlowHz, 2) + ",";
    json += "\"vol\":"            + String(totalLiters, 2) + ",";
    json += "\"flowStatus\":\""   + String(getFlowStatus(currentFlowLMin)) + "\",";

    // ThingsBoard
    json += "\"tbHttp\":"         + String(lastTbHttpCode) + ",";
    json += "\"tbSent\":"         + String(tbSendCount);
    json += "}";

    server.send(200, "application/json", json);
}

void handleScan() {
    if (!isClientAuthenticated()) {
        server.send(401, "application/json", "[]");
        return;
    }
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleSaveWiFi() {
    if (!isClientAuthenticated()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    if (server.hasArg("ssid")) {
        String s = server.arg("ssid");
        String p = server.arg("pass");
        prefWifi.putString("ssid", s);
        prefWifi.putString("pass", p);
        wifi_ssid = s;
        wifi_pass = p;

        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        server.send(200, "text/plain", "Credentials saved! Connecting to " + s);
        return;
    }
    server.send(400, "text/plain", "Missing SSID");
}

void handleResetWiFi() {
    if (!isClientAuthenticated()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    prefWifi.remove("ssid");
    prefWifi.remove("pass");
    WiFi.disconnect();
    server.send(200, "text/plain", "Wi-Fi credentials cleared.");
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
    Serial.println(F("  ESP32-S3 MODULE 2: WATER QUALITY, pH & FLOW MONITOR"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | TDS (GPIO 1 - ADC1_CH0)"));
    Serial.println(F("           Flow (GPIO 5)  | E-201-C pH (GPIO 2 - ADC1_CH1)"));
    Serial.println(F("  Display: 2004 I2C LCD (Wire: SDA=17, SCL=18)"));
    Serial.println(F("  Utility: WebServer AP (192.168.4.1) + Secure Login Dashboard"));
    printLine('=');

    // 1. TDS ADC (GPIO 1 - ADC1_CH0)
    pinMode(TDS_PIN, INPUT);
    Serial.println(F("  [OK] TDS Meter        -> GPIO 1 (ADC1_CH0)"));

    // 2. pH ADC (GPIO 2 - ADC1_CH1)
    pinMode(PH_PIN, INPUT);
    Serial.println(F("  [OK] E-201-C pH Meter -> GPIO 2 (ADC1_CH1)"));

    // 3. DHT11 (GPIO 4)
    dht.begin();
    Serial.println(F("  [OK] DHT11            -> GPIO 4"));

    // 4. Flow Sensor Interrupt (GPIO 5)
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowPulseISR, FALLING);
    Serial.println(F("  [OK] Water Flow Sensor -> GPIO 5 (Interrupt)"));

    // 5. Initialize 2004 I2C LCD on Dedicated Wire (SDA=17, SCL=18)
    pinMode(LCD_SDA_PIN, INPUT_PULLUP);
    pinMode(LCD_SCL_PIN, INPUT_PULLUP);
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN, 50000); // 50kHz for rock-solid stability
    Wire.setTimeOut(25);
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
        lcd.setCursor(0, 0); lcd.print(F("===================="));
        lcd.setCursor(0, 1); lcd.print(F("    Welcome to      "));
        lcd.setCursor(0, 2); lcd.print(F("  Module-2 Monitor  "));
        lcd.setCursor(0, 3); lcd.print(F("===================="));
        lcd_available = true;
        Serial.printf("  [OK] 2004 LCD Online at 0x%02X\n", lcdAddr);
    } else {
        Serial.println(F("  [..] 2004 LCD Not Detected on Wire (SDA=17, SCL=18)"));
    }

    // 6. Preferences for WiFi & Auth
    prefWifi.begin("m2-wifi", false);
    wifi_ssid = prefWifi.getString("ssid", "Meeting Room");
    wifi_pass = prefWifi.getString("pass", "Shahid786$$");

    prefAuth.begin("m2-auth", false);
    auth_user = prefAuth.getString("user", "admin");
    auth_pass = prefAuth.getString("pass", "admin");

    // 7. Dual WiFi Mode (AP + STA)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("  [OK] Access Point Active: SSID '%s' (IP: 192.168.4.1)\n", AP_SSID);

    if (wifi_ssid.length() > 0) {
        Serial.printf("  [..] Connecting to Station SSID '%s'...\n", wifi_ssid.c_str());
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }

    if (MDNS.begin("esp32waterquality")) {
        MDNS.addService("http", "tcp", 80);
    }

    // 8. Configure Web Server Headers & Routes
    const char* headerkeys[] = {"Cookie"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/data", HTTP_GET, handleData);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/savewifi", HTTP_POST, handleSaveWiFi);
    server.on("/resetwifi", HTTP_POST, handleResetWiFi);

    server.begin();
    Serial.println(F("  [OK] WebServer Started on Port 80 (Login Protected)"));
    printLine('=');

    if (lcd_available) {
        delay(1200);
        lcd.clear();
        updateLCD();
    }

    lastLog = millis();
    lastLcdSwitch = millis();
}

// =====================================================================
//  Main Loop
// =====================================================================
void loop() {
    // 1. Process Incoming HTTP Clients Immediately
    server.handleClient();

    unsigned long now = millis();

    // 2. Read sensors & send telemetry every 3 seconds (INTERVAL)
    if (now - lastLog >= INTERVAL) {
        float elapsedSec = (now - lastLog) / 1000.0f;
        lastLog = now;
        loopCount++;

        // ---- 1. DHT11 ----
        float hum  = dht.readHumidity();
        float tC   = dht.readTemperature();
        float tF   = dht.readTemperature(true);
        currentDhtOk = (!isnan(hum) && !isnan(tC) && hum > 0.05f);
        if (currentDhtOk) {
            currentTC  = tC;
            currentTF  = tF;
            currentHum = hum;
            currentHI  = dht.computeHeatIndex(tC, hum, false);
        } else {
            currentTC  = 0.0f;
            currentTF  = 0.0f;
            currentHum = 0.0f;
            currentHI  = 0.0f;
        }

        // ---- 2. Water Flow Sensor (FS200A) ----
        noInterrupts();
        unsigned long pulses = flowPulseCount;
        flowPulseCount = 0;
        interrupts();

        currentFlowHz = pulses / elapsedSec;
        currentFlowLMin  = currentFlowHz / FLOW_CAL_FACTOR;
        currentFlowMLSec = (currentFlowLMin * 1000.0f) / 60.0f;
        float litersInPeriod = (float)pulses / PULSES_PER_LITER;
        totalLiters += litersInPeriod;
        const char* flowStatus = getFlowStatus(currentFlowLMin);

        // ---- 3. Analog TDS Meter ----
        float tdsVoltage = 0.0f;
        int   tdsRawADC   = 0;
        bool  tdsOK       = false;
        float tdsPPM     = readTDS(currentTC, tdsVoltage, tdsRawADC, tdsOK);
        currentTdsOk     = tdsOK;
        currentTdsPPM    = tdsPPM;
        currentTdsV      = tdsVoltage;
        currentTdsRaw    = tdsRawADC;
        const char* qualityStr = tdsOK ? getWaterQuality(tdsPPM) : "OFF";

        // ---- 4. E-201-C BNC pH Sensor ----
        float phVoltage = 0.0f;
        int   phRawADC = 0;
        bool  phOK     = false;
        float phValue  = readPH(phVoltage, phRawADC, phOK);
        currentPhOk    = phOK;
        currentPhValue = phValue;
        currentPhV     = phVoltage;
        currentPhRaw   = phRawADC;
        const char* phStatus = phOK ? getPhStatus(phValue) : "OFF";

        // ---- Serial Output ----
        Serial.printf("[M2 #%lu] T:%.1fC | TDS:%.1fppm (%s) | pH:%.2f (%s) | Flow:%.2fL/min | WiFi:%s\n",
                      loopCount, currentDhtOk ? currentTC : 0.0f,
                      currentTdsOk ? currentTdsPPM : 0.0f, qualityStr,
                      currentPhOk ? currentPhValue : 0.0f, phStatus,
                      currentFlowLMin,
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "AP-Only");

        // ---- Send ThingsBoard Telemetry ----
        sendTelemetry(currentDhtOk, currentTC, currentTF, currentHum, currentHI,
                      currentTdsOk, currentTdsPPM, currentTdsV, qualityStr,
                      currentFlowLMin, currentFlowMLSec, currentFlowHz,
                      totalLiters, pulses, flowStatus,
                      currentPhOk, currentPhValue, currentPhV, phStatus);

        // Update live screen numbers
        if (lcd_available) {
            updateLCD();
        }
    }

    // 3. Rotate LCD screen page every 5 seconds (5000ms)
    if (lcd_available) {
        if (now - lastLcdSwitch >= LCD_PAGE_INTERVAL) {
            lastLcdSwitch = now;
            lcdScreenPage = (lcdScreenPage + 1) % 2;
            lcd.clear();
            updateLCD();
        }
    }
}
