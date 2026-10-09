// =====================================================================
// ESP32-S3 — MODULE 3: Light, Soil Moisture & Environmental System
// With Dual AP+STA WiFi Utility, Mobile Min/Max Threshold Controls & Wireless OTA
// =====================================================================
// SENSOR WIRING FOR ESP32-S3:
//   1. DHT11 Sensor:
//      - DATA  -> GPIO 4
//      - VCC   -> 3.3V / 5V
//      - GND   -> GND
//
//   2. BH1750 Digital Light Sensor:
//      - SDA   -> GPIO 5   (Wire1: User connection) [Also auto-detects GPIO 15]
//      - SCL   -> GPIO 16  (Wire1)
//      - ADDR  -> GND (Address: 0x23) [or VCC for 0x5C]
//      - VCC   -> 5V / 3.3V
//      - GND   -> GND
//
//   3. Capacitive Soil Moisture Sensor v2.0:
//      - AOUT  -> GPIO 1   (ADC1_CH0 - 12-bit Analog Input)
//      - VCC   -> 3.3V
//      - GND   -> GND
//
//   4. 2004 Character LCD (I2C Backpack):
//      - SDA   -> GPIO 17  (Wire: Address 0x27 / 0x3F)
//      - SCL   -> GPIO 18  (Wire)
//      - VCC   -> 5V (VIN)
//      - GND   -> GND
//
//   * Access Point: "ESP32-S3_Light_Soil" (192.168.4.1)
//   * Web Login: admin / admin
//   * Wireless OTA: Accessible via /update or ArduinoOTA (port 3232)
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ArduinoOTA.h>

// =====================================================================
//  1. Hardware & Sensors Configuration
// =====================================================================

// DHT11 Configuration (GPIO 4)
#define DHTPIN   4
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

// BH1750 Configuration (Wire1: SDA=5 / 15, SCL=16)
#define BH1750_DEFAULT_SDA 5
#define BH1750_DEFAULT_SCL 16
BH1750 lightMeter(0x23);
bool bh1750_available = false;
int activeBH1750_SDA = BH1750_DEFAULT_SDA;
int activeBH1750_SCL = BH1750_DEFAULT_SCL;
uint8_t activeBH1750_ADDR = 0x23;
float currentLux = 0.0f;

// Capacitive Soil Moisture Sensor (GPIO 1 - ADC1_CH0)
#define SOIL_PIN         1
#define VREF             3.3f
#define ADC_RESOLUTION   4095.0f
const int AIR_VALUE   = 3000; // Dry air (0% moisture)
const int WATER_VALUE = 1350; // Pure water (100% moisture)

// 2004 Character LCD (Dedicated Wire: SDA=17, SCL=18)
#define LCD_SDA_PIN      17
#define LCD_SCL_PIN      18
LiquidCrystal_I2C lcd(0x27, 20, 4);
bool lcd_available = false;

// =====================================================================
//  2. WiFi, Preferences & ThingsBoard Configuration
// =====================================================================
const char* AP_SSID = "ESP32-S3_Light_Soil";
const char* AP_PASS = "12345678";

String wifi_ssid = "Meeting Room";
String wifi_pass = "Shahid786$$";

const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";
int lastTbHttpCode = 0;
unsigned long tbSendCount = 0;

WebServer server(80);
Preferences prefWifi;
Preferences prefAuth;
Preferences prefLimits;

// Authentication Credentials
String auth_user = "admin";
String auth_pass = "admin";

// =====================================================================
//  3. User-Configurable Min & Max Target Limits (Persistent in Flash)
// =====================================================================
float limitTempMin = 18.0f;
float limitTempMax = 32.0f;
float limitHumMin  = 35.0f;
float limitHumMax  = 75.0f;
float limitLuxMin  = 100.0f;
float limitLuxMax  = 5000.0f;
float limitSoilMin = 25.0f;
float limitSoilMax = 70.0f;

// Live Readings & States
float liveTempC = 0.0f;
float liveTempF = 0.0f;
float liveHum = 0.0f;
float liveHeatIndex = 0.0f;
bool  liveDhtOk = false;

float liveSoilMoisture = 0.0f;
float liveSoilVoltage = 0.0f;
int   liveSoilADC = 0;

const unsigned long SAMPLE_INTERVAL = 3000UL;
unsigned long lastSampleTime = 0;
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

const char* getLightStatus(float lux) {
    if (lux < 1.0f)     return "DARK";
    if (lux < 50.0f)    return "DIM";
    if (lux < 200.0f)   return "MODERATE";
    if (lux < 500.0f)   return "BRIGHT";
    if (lux < 1000.0f)  return "VERY BRIGHT";
    if (lux < 10000.0f) return "OUTDOOR SHADE";
    if (lux < 30000.0f) return "CLOUDY OUTDOOR";
    return                     "DIRECT SUNLIGHT";
}

const char* getSoilStatus(float pct) {
    if (pct < 15.0f)  return "VERY DRY";
    if (pct < 35.0f)  return "DRY";
    if (pct < 65.0f)  return "OPTIMAL";
    if (pct < 85.0f)  return "WET";
    return                   "WATERLOGGED";
}

const char* shortLightStatus(float lux) {
    if (lux < 1.0f)     return "DARK";
    if (lux < 50.0f)    return "DIM";
    if (lux < 200.0f)   return "MODER";
    if (lux < 500.0f)   return "BRIGHT";
    if (lux < 1000.0f)  return "V-BRT";
    if (lux < 10000.0f) return "SHADE";
    if (lux < 30000.0f) return "CLOUDY";
    return                     "SUNNY";
}

const char* shortSoilStatus(float pct) {
    if (pct < 15.0f)  return "V-DRY";
    if (pct < 35.0f)  return "DRY";
    if (pct < 65.0f)  return "OPTIM";
    if (pct < 85.0f)  return "WET";
    return                   "FLOOD";
}

// =====================================================================
//  BH1750 Sensor Routines
// =====================================================================
bool tryBH1750Bus(int sda, int scl) {
    pinMode(sda, INPUT_PULLUP);
    pinMode(scl, INPUT_PULLUP);
    Wire1.end();
    delay(40);
    Wire1.begin(sda, scl, 50000);
    Wire1.setTimeOut(25);

    Wire1.beginTransmission(0x23);
    if (Wire1.endTransmission() == 0) {
        if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire1)) {
            activeBH1750_SDA = sda;
            activeBH1750_SCL = scl;
            activeBH1750_ADDR = 0x23;
            bh1750_available = true;
            Serial.printf("  [OK] BH1750 DETECTED at 0x23 on Wire1 (SDA=GPIO %d, SCL=GPIO %d)\n", sda, scl);
            return true;
        }
    }

    Wire1.beginTransmission(0x5C);
    if (Wire1.endTransmission() == 0) {
        if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire1)) {
            activeBH1750_SDA = sda;
            activeBH1750_SCL = scl;
            activeBH1750_ADDR = 0x5C;
            bh1750_available = true;
            Serial.printf("  [OK] BH1750 DETECTED at 0x5C on Wire1 (SDA=GPIO %d, SCL=GPIO %d)\n", sda, scl);
            return true;
        }
    }
    return false;
}

void initBH1750Sensor() {
    if (tryBH1750Bus(5, 16)) return;
    if (tryBH1750Bus(15, 16)) return;
    bh1750_available = false;
    Serial.println(F("  [ERR] BH1750 Light Sensor NOT DETECTED on SDA=5 or SDA=15 (SCL=16)!"));
}

// =====================================================================
//  Capacitive Soil Sensor Reading
// =====================================================================
float readSoilMoisture(int &outRawADC, float &outVoltage) {
    const int SAMPLES = 20;
    int buffer[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buffer[i] = analogRead(SOIL_PIN);
        delay(1);
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
    for (int i = 5; i < 15; i++) sum += buffer[i];
    outRawADC = sum / 10;
    outVoltage = (outRawADC / ADC_RESOLUTION) * VREF;

    float moisturePct = ((float)(AIR_VALUE - outRawADC) / (float)(AIR_VALUE - WATER_VALUE)) * 100.0f;
    return constrain(moisturePct, 0.0f, 100.0f);
}

// =====================================================================
//  ThingsBoard Telemetry Dispatch
// =====================================================================
void sendTelemetry(float tC, float tF, float hum, float hi,
                   float lux, const char* lightLv,
                   float soilPct, float soilV, int soilADC, const char* soilLv) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    String url = String(TB_HOST) + "/api/v1/" + String(TB_TOKEN) + "/telemetry";
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");

    String p = "{";
    if (liveDhtOk) {
        p += "\"m3_temperature\":"    + String(tC, 1);
        p += ",\"m3_temperatureF\":"  + String(tF, 1);
        p += ",\"m3_humidity\":"      + String(hum, 1);
        p += ",\"m3_heatIndex\":"     + String(hi, 1);
        p += ",\"temperature\":"      + String(tC, 1);
        p += ",\"temperatureF\":"     + String(tF, 1);
        p += ",\"humidity\":"         + String(hum, 1);
        p += ",\"heatIndex\":"        + String(hi, 1);
    } else {
        p += "\"temperatureStatus\":\"OFFLINE\"";
    }
    if (bh1750_available) {
        p += ",\"lux\":"              + String(lux, 1);
        p += ",\"lightLevel\":\""      + String(lightLv) + "\"";
    } else {
        p += ",\"lightStatus\":\"OFFLINE\"";
    }
    p += ",\"soilMoisture\":"     + String(soilPct, 1);
    p += ",\"soilVoltage\":"      + String(soilV, 3);
    p += ",\"soilRawADC\":"       + String(soilADC);
    p += ",\"soilStatus\":\""      + String(soilLv) + "\"";
    p += "}";

    int code = https.POST(p);
    lastTbHttpCode = code;
    if (code > 0) tbSendCount++;
    https.end();
}

// =====================================================================
//  Authentication Management
// =====================================================================
const char* AUTH_COOKIE_NAME = "m3_auth";
const char* AUTH_TOKEN_VALUE = "valid_m3_session_786";

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
//  HTML Web Pages (Login, Dashboard & Wireless OTA)
// =====================================================================

const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Module 3 Login | ESP32-S3 Environment Hub</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #10b981;
            --primary-hover: #059669;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #334155;
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
            background: #0f172a;
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
            color: #0f172a;
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
        <div class="icon">🌿</div>
        <h1>ESP32-S3 Module 3</h1>
        <p class="sub">Light, Soil & Climate Hub</p>

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
            data.append('user', u);
            data.append('pass', p);

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

const char OTA_UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Wireless OTA Update | ESP32-S3 Module 3</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #10b981;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #334155;
            --danger: #ef4444;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg); color: var(--text-main); min-height: 100vh; display: flex; align-items: center; justify-content: center; padding: 16px; }
        .ota-card {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 16px;
            padding: 32px 24px;
            width: 100%;
            max-width: 440px;
            box-shadow: 0 20px 25px -5px rgba(0, 0, 0, 0.5);
            text-align: center;
        }
        .icon { font-size: 3rem; margin-bottom: 12px; }
        h1 { font-size: 1.4rem; font-weight: 700; margin-bottom: 6px; }
        p.sub { font-size: 0.85rem; color: var(--text-muted); margin-bottom: 24px; }
        .file-box {
            border: 2px dashed var(--border);
            border-radius: 12px;
            padding: 24px 16px;
            margin-bottom: 20px;
            background: rgba(15, 23, 42, 0.6);
        }
        input[type="file"] { display: block; width: 100%; color: var(--text-muted); font-size: 0.9rem; margin-top: 8px; }
        .btn {
            width: 100%;
            padding: 12px;
            background: var(--primary);
            color: #0f172a;
            border: none;
            border-radius: 8px;
            font-size: 1rem;
            font-weight: 700;
            cursor: pointer;
            transition: background 0.2s;
        }
        .btn:hover { background: #059669; color: #fff; }
        .progress-box { display: none; margin-top: 20px; text-align: left; }
        .progress-bar { width: 100%; height: 8px; background: #0f172a; border-radius: 4px; overflow: hidden; margin-top: 8px; }
        .progress-fill { height: 100%; width: 0%; background: var(--primary); transition: width 0.3s; }
        .status-txt { font-size: 0.85rem; color: var(--text-muted); margin-top: 8px; }
        .back-link { display: inline-block; margin-top: 20px; color: var(--primary); font-size: 0.85rem; text-decoration: none; }
        .back-link:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <div class="ota-card">
        <div class="icon">⚡</div>
        <h1>Wireless Firmware Update (OTA)</h1>
        <p class="sub">Upload new compiled <b>firmware.bin</b> over Wi-Fi without any cables!</p>

        <form id="uploadForm" enctype="multipart/form-data">
            <div class="file-box">
                <span style="font-size:1.8rem;">📦</span>
                <div style="font-size:0.9rem; font-weight:600; margin-top:6px;">Choose Firmware Binary</div>
                <input type="file" id="firmwareFile" accept=".bin" required>
            </div>
            <button type="submit" class="btn" id="uploadBtn">Flash Wirelessly</button>
        </form>

        <div class="progress-box" id="progressBox">
            <div style="display:flex; justify-content:space-between; font-size:0.8rem; font-weight:600;">
                <span>Uploading to ESP32-S3...</span>
                <span id="pctTxt">0%</span>
            </div>
            <div class="progress-bar">
                <div class="progress-fill" id="progressFill"></div>
            </div>
            <div class="status-txt" id="statusTxt">Transmitting binary chunks over Wi-Fi...</div>
        </div>

        <a href="/" class="back-link">← Return to Sensor Dashboard</a>
    </div>

    <script>
        const form = document.getElementById('uploadForm');
        form.onsubmit = function(e) {
            e.preventDefault();
            const fileInput = document.getElementById('firmwareFile');
            if (!fileInput.files.length) return;
            const file = fileInput.files[0];
            const btn = document.getElementById('uploadBtn');
            const pBox = document.getElementById('progressBox');
            const pFill = document.getElementById('progressFill');
            const pctTxt = document.getElementById('pctTxt');
            const statusTxt = document.getElementById('statusTxt');

            btn.disabled = true;
            btn.innerText = 'Flashing...';
            pBox.style.display = 'block';

            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/update', true);

            xhr.upload.onprogress = function(e) {
                if (e.lengthComputable) {
                    const pct = Math.round((e.loaded / e.total) * 100);
                    pFill.style.width = pct + '%';
                    pctTxt.innerText = pct + '%';
                }
            };

            xhr.onload = function() {
                if (xhr.status === 200) {
                    pFill.style.width = '100%';
                    pctTxt.innerText = '100%';
                    statusTxt.innerHTML = '✅ <b>Success!</b> ESP32-S3 is rebooting into the new firmware. Reconnect in 8 seconds...';
                    setTimeout(() => { window.location.href = '/'; }, 8000);
                } else {
                    statusTxt.innerHTML = '❌ <b>Failed!</b> ' + xhr.responseText;
                    btn.disabled = false;
                    btn.innerText = 'Try Again';
                }
            };

            xhr.onerror = function() {
                statusTxt.innerHTML = '❌ Upload interrupted. ESP32 may be rebooting or connection lost.';
                btn.disabled = false;
                btn.innerText = 'Try Again';
            };

            const formData = new FormData();
            formData.append('update', file);
            xhr.send(formData);
        };
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
    <title>ESP32-S3 Module 3 | Climate, Light & Soil Dashboard</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #10b981;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #334155;
            --emerald: #10b981;
            --amber: #f59e0b;
            --blue: #38bdf8;
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
        .nav-actions { display: flex; gap: 8px; align-items: center; }
        .btn-nav {
            padding: 7px 13px;
            background: #0f172a;
            color: #e2e8f0;
            border: 1px solid var(--border);
            border-radius: 6px;
            font-size: 0.82rem;
            font-weight: 600;
            cursor: pointer;
            text-decoration: none;
            transition: all 0.2s;
        }
        .btn-nav:hover { background: var(--primary); color: #0f172a; border-color: var(--primary); }
        .btn-logout {
            padding: 7px 13px;
            background: #1e293b;
            color: #e2e8f0;
            border: 1px solid var(--border);
            border-radius: 6px;
            font-size: 0.82rem;
            cursor: pointer;
            transition: all 0.2s;
        }
        .btn-logout:hover { background: var(--danger); color: #fff; border-color: var(--danger); }

        /* Banner */
        .banner {
            display: flex;
            flex-wrap: wrap;
            gap: 12px;
            background: rgba(16, 185, 129, 0.08);
            border: 1px solid rgba(16, 185, 129, 0.3);
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
        .status-info { background: rgba(56, 189, 248, 0.2); color: var(--blue); }
        .status-err { background: rgba(239, 68, 68, 0.2); color: var(--danger); }

        .progress-bar {
            width: 100%;
            height: 6px;
            background: #0f172a;
            border-radius: 3px;
            margin-top: 10px;
            overflow: hidden;
        }
        .progress-fill { height: 100%; border-radius: 3px; transition: width 0.5s ease; }
        .fill-blue { background: var(--blue); }
        .fill-emerald { background: var(--emerald); }
        .fill-amber { background: var(--amber); }

        /* Configuration Sections */
        .section-box {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 14px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .section-header { font-size: 1.1rem; font-weight: 700; margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }

        /* Limit Rows */
        .limits-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(230px, 1fr));
            gap: 14px;
            margin-bottom: 16px;
        }
        .limit-card {
            background: #0f172a;
            border: 1px solid var(--border);
            border-radius: 10px;
            padding: 14px;
        }
        .limit-card-title { font-size: 0.85rem; font-weight: 700; color: #fff; margin-bottom: 10px; display: flex; align-items: center; justify-content: space-between; }
        .limit-inputs { display: flex; gap: 8px; align-items: center; }
        .limit-field { flex: 1; }
        .limit-field label { display: block; font-size: 0.7rem; color: var(--text-muted); font-weight: 600; text-transform: uppercase; margin-bottom: 4px; }
        .limit-field input {
            width: 100%;
            padding: 8px 10px;
            background: #1e293b;
            border: 1px solid var(--border);
            border-radius: 6px;
            color: #fff;
            font-size: 0.9rem;
            outline: none;
        }
        .limit-field input:focus { border-color: var(--primary); }

        /* Buttons */
        .btn-action {
            padding: 10px 18px;
            background: var(--primary);
            color: #0f172a;
            border: none;
            border-radius: 8px;
            font-weight: 700;
            font-size: 0.9rem;
            cursor: pointer;
            white-space: nowrap;
        }
        .btn-action:hover { background: #059669; color: #fff; }

        .wifi-grid { display: grid; grid-template-columns: 1fr 1fr auto; gap: 10px; align-items: end; }
        @media (max-width: 650px) { .wifi-grid { grid-template-columns: 1fr; } }
        .input-group label { display: block; font-size: 0.75rem; font-weight: 600; color: var(--text-muted); margin-bottom: 5px; text-transform: uppercase; }
        .input-group input, .input-group select {
            width: 100%;
            padding: 10px 12px;
            background: #0f172a;
            border: 1px solid var(--border);
            border-radius: 8px;
            color: #fff;
            font-size: 0.9rem;
            outline: none;
        }
        .input-group input:focus { border-color: var(--primary); }
    </style>
</head>
<body>
    <div class="container">
        <!-- Navbar -->
        <div class="navbar">
            <div class="nav-brand">🌿 <span>Module 3</span> Climate, Light & Soil Hub</div>
            <div class="nav-actions">
                <a href="/update" class="btn-nav">⚡ Wireless OTA</a>
                <button class="btn-logout" onclick="window.location.href='/logout'">Sign Out</button>
            </div>
        </div>

        <!-- Status Banner -->
        <div class="banner">
            <div class="banner-pill">
                <span class="dot pulse"></span>
                <span><b>AP Mode:</b> ESP32-S3_Light_Soil (192.168.4.1)</span>
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
            <!-- 1. Temperature -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Temperature</span>
                    <span class="card-icon">🌡️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valTemp">--.-</span><span class="card-unit">°C</span></div>
                    <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:8px;">Target: <span id="lblTempRange">-- - -- °C</span></div>
                </div>
                <div class="card-status status-optimal" id="statusTemp">DHT11 (GPIO 4)</div>
            </div>

            <!-- 2. Humidity -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Relative Humidity</span>
                    <span class="card-icon">💧</span>
                </div>
                <div>
                    <div class="card-value"><span id="valHum">--.-</span><span class="card-unit">%</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-blue" id="barHum" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-info" id="statusHum">Ambient Air</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblHumRange">Target: -- - --%</span>
                </div>
            </div>

            <!-- 3. Light Intensity -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Light Intensity</span>
                    <span class="card-icon">☀️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valLux">---</span><span class="card-unit">lx</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-amber" id="barLux" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="statusLux">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblLuxRange">Target: -- - -- lx</span>
                </div>
            </div>

            <!-- 4. Soil Moisture -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Soil Moisture</span>
                    <span class="card-icon">🌱</span>
                </div>
                <div>
                    <div class="card-value"><span id="valSoil">--.-</span><span class="card-unit">%</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-emerald" id="barSoil" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="statusSoil">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblSoilRange">Target: -- - --%</span>
                </div>
            </div>
        </div>

        <!-- Min & Max Target Thresholds Configurator -->
        <div class="section-box">
            <div class="section-header">🎯 Mobile Sensor Min & Max Target Thresholds</div>
            <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:16px;">
                Define your ideal environmental ranges. Cards will automatically highlight alerts in real-time when sensor values breach these limits.
            </div>

            <div class="limits-grid">
                <!-- Temp Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">🌡️ Temperature Limits (°C)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min Temp</label>
                            <input type="number" step="0.5" id="inpTempMin" placeholder="18.0">
                        </div>
                        <div class="limit-field">
                            <label>Max Temp</label>
                            <input type="number" step="0.5" id="inpTempMax" placeholder="32.0">
                        </div>
                    </div>
                </div>

                <!-- Humidity Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">💧 Humidity Limits (%)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min Hum</label>
                            <input type="number" step="1" id="inpHumMin" placeholder="35">
                        </div>
                        <div class="limit-field">
                            <label>Max Hum</label>
                            <input type="number" step="1" id="inpHumMax" placeholder="75">
                        </div>
                    </div>
                </div>

                <!-- Light Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">☀️ Light Limits (Lux)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min Lux</label>
                            <input type="number" step="50" id="inpLuxMin" placeholder="100">
                        </div>
                        <div class="limit-field">
                            <label>Max Lux</label>
                            <input type="number" step="100" id="inpLuxMax" placeholder="5000">
                        </div>
                    </div>
                </div>

                <!-- Soil Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">🌱 Soil Moisture Limits (%)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min Soil</label>
                            <input type="number" step="1" id="inpSoilMin" placeholder="25">
                        </div>
                        <div class="limit-field">
                            <label>Max Soil</label>
                            <input type="number" step="1" id="inpSoilMax" placeholder="70">
                        </div>
                    </div>
                </div>
            </div>

            <div style="display:flex; align-items:center; gap:12px;">
                <button class="btn-action" onclick="saveLimits()">💾 Save Limits to ESP32</button>
                <span id="limitFeedback" style="font-size:0.85rem; color:var(--text-muted);">Saved in non-volatile flash memory.</span>
            </div>
        </div>

        <!-- Wi-Fi Station Utility & Configuration -->
        <div class="section-box">
            <div class="section-header">📶 Wi-Fi Station Utility & Configuration</div>
            <div style="margin-bottom:14px; display:flex; gap:10px; align-items:center;">
                <button class="btn-action" style="background:#6366f1; color:#fff;" id="btnScan" onclick="scanNetworks()">🔍 Scan Available Wi-Fi</button>
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
            <div style="margin-top:12px; font-size:0.82rem; color:var(--text-muted);" id="wifiFeedback">ESP32-S3 will auto-reconnect to saved router credentials upon boot.</div>
        </div>
    </div>

    <script>
        let limitsLoaded = false;

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

                    // Populate Limits input fields once on first load
                    if (!limitsLoaded) {
                        document.getElementById('inpTempMin').value = d.tempMin;
                        document.getElementById('inpTempMax').value = d.tempMax;
                        document.getElementById('inpHumMin').value = d.humMin;
                        document.getElementById('inpHumMax').value = d.humMax;
                        document.getElementById('inpLuxMin').value = d.luxMin;
                        document.getElementById('inpLuxMax').value = d.luxMax;
                        document.getElementById('inpSoilMin').value = d.soilMin;
                        document.getElementById('inpSoilMax').value = d.soilMax;
                        limitsLoaded = true;
                    }

                    // Display Target Ranges
                    document.getElementById('lblTempRange').innerText = d.tempMin + ' - ' + d.tempMax + ' °C';
                    document.getElementById('lblHumRange').innerText = 'Target: ' + d.humMin + ' - ' + d.humMax + '%';
                    document.getElementById('lblLuxRange').innerText = 'Target: ' + d.luxMin + ' - ' + d.luxMax + ' lx';
                    document.getElementById('lblSoilRange').innerText = 'Target: ' + d.soilMin + ' - ' + d.soilMax + '%';

                    // 1. Temperature
                    if (d.dhtOk) {
                        document.getElementById('valTemp').innerText = d.temp.toFixed(1);
                        const sTemp = document.getElementById('statusTemp');
                        if (d.temp < d.tempMin) {
                            sTemp.innerText = '❄️ LOW (< ' + d.tempMin + '°C)';
                            sTemp.className = 'card-status status-warn';
                        } else if (d.temp > d.tempMax) {
                            sTemp.innerText = '🔥 HIGH (> ' + d.tempMax + '°C)';
                            sTemp.className = 'card-status status-err';
                        } else {
                            sTemp.innerText = '✅ OPTIMAL';
                            sTemp.className = 'card-status status-optimal';
                        }
                    } else {
                        document.getElementById('valTemp').innerText = '--.-';
                        document.getElementById('statusTemp').innerText = 'OFFLINE';
                        document.getElementById('statusTemp').className = 'card-status status-err';
                    }

                    // 2. Humidity
                    if (d.dhtOk) {
                        document.getElementById('valHum').innerText = d.hum.toFixed(1);
                        document.getElementById('barHum').style.width = Math.min(d.hum, 100) + '%';
                        const sHum = document.getElementById('statusHum');
                        if (d.hum < d.humMin) {
                            sHum.innerText = '🌵 DRY (< ' + d.humMin + '%)';
                            sHum.className = 'card-status status-warn';
                        } else if (d.hum > d.humMax) {
                            sHum.innerText = '💧 HUMID (> ' + d.humMax + '%)';
                            sHum.className = 'card-status status-err';
                        } else {
                            sHum.innerText = '✅ OPTIMAL';
                            sHum.className = 'card-status status-optimal';
                        }
                    } else {
                        document.getElementById('valHum').innerText = '--.-';
                        document.getElementById('barHum').style.width = '0%';
                        document.getElementById('statusHum').innerText = 'OFFLINE';
                        document.getElementById('statusHum').className = 'card-status status-err';
                    }

                    // 3. Light Intensity (Lux)
                    if (d.bhOnline && d.lux !== null) {
                        document.getElementById('valLux').innerText = d.lux.toFixed(0);
                        document.getElementById('barLux').style.width = Math.min((d.lux / 1000) * 100, 100) + '%';
                        const sLux = document.getElementById('statusLux');
                        if (d.lux < d.luxMin) {
                            sLux.innerText = '🌑 LOW LIGHT (< ' + d.luxMin + ' lx)';
                            sLux.className = 'card-status status-warn';
                        } else if (d.lux > d.luxMax) {
                            sLux.innerText = '☀️ BRIGHT (> ' + d.luxMax + ' lx)';
                            sLux.className = 'card-status status-err';
                        } else {
                            sLux.innerText = '✅ OPTIMAL';
                            sLux.className = 'card-status status-optimal';
                        }
                    } else {
                        document.getElementById('valLux').innerText = '---';
                        document.getElementById('barLux').style.width = '0%';
                        document.getElementById('statusLux').innerText = 'OFFLINE';
                        document.getElementById('statusLux').className = 'card-status status-err';
                    }

                    // 4. Soil Moisture
                    document.getElementById('valSoil').innerText = d.soil.toFixed(1);
                    document.getElementById('barSoil').style.width = Math.min(d.soil, 100) + '%';
                    const sSoil = document.getElementById('statusSoil');
                    if (d.soil < d.soilMin) {
                        sSoil.innerText = '🏜️ DRY (< ' + d.soilMin + '%)';
                        sSoil.className = 'card-status status-warn';
                    } else if (d.soil > d.soilMax) {
                        sSoil.innerText = '🌊 WET (> ' + d.soilMax + '%)';
                        sSoil.className = 'card-status status-err';
                    } else {
                        sSoil.innerText = '✅ OPTIMAL';
                        sSoil.className = 'card-status status-optimal';
                    }
                })
                .catch(e => console.log('Poll error:', e));
        }

        function saveLimits() {
            const body = new URLSearchParams();
            body.append('tempMin', document.getElementById('inpTempMin').value);
            body.append('tempMax', document.getElementById('inpTempMax').value);
            body.append('humMin',  document.getElementById('inpHumMin').value);
            body.append('humMax',  document.getElementById('inpHumMax').value);
            body.append('luxMin',  document.getElementById('inpLuxMin').value);
            body.append('luxMax',  document.getElementById('inpLuxMax').value);
            body.append('soilMin', document.getElementById('inpSoilMin').value);
            body.append('soilMax', document.getElementById('inpSoilMax').value);

            const fb = document.getElementById('limitFeedback');
            fb.innerText = 'Saving limits...';

            fetch('/savelimits', { method: 'POST', body: body })
                .then(r => r.text())
                .then(txt => {
                    fb.innerText = '✅ ' + txt;
                    updateDashboard();
                })
                .catch(() => { fb.innerText = '❌ Error saving limits'; });
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
                    msg.innerHTML = 'Found ' + list.length + ' networks. Select: <select id="netSelect" onchange="document.getElementById(\'wifiSsid\').value=this.value" style="padding:4px 8px; background:#0f172a; color:#fff; border:1px solid #334155; border-radius:6px; margin-left:6px;"><option value="">-- Choose SSID --</option>' + 
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
    String u = server.arg("user");
    String p = server.arg("pass");

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
    
    // Live sensor values
    json += "\"dhtOk\":"          + String(liveDhtOk ? "true" : "false") + ",";
    json += "\"temp\":"           + String(liveTempC, 1) + ",";
    json += "\"tempF\":"          + String(liveTempF, 1) + ",";
    json += "\"hum\":"            + String(liveHum, 1) + ",";
    json += "\"hi\":"             + String(liveHeatIndex, 1) + ",";
    
    json += "\"bhOnline\":"       + String(bh1750_available ? "true" : "false") + ",";
    if (bh1750_available) {
        json += "\"lux\":"        + String(currentLux, 1) + ",";
        json += "\"lightStatus\":\"" + String(getLightStatus(currentLux)) + "\",";
    } else {
        json += "\"lux\":null,";
        json += "\"lightStatus\":\"OFFLINE\",";
    }
    json += "\"bhSda\":"          + String(activeBH1750_SDA) + ",";

    json += "\"soil\":"           + String(liveSoilMoisture, 1) + ",";
    json += "\"soilStatus\":\""   + String(getSoilStatus(liveSoilMoisture)) + "\",";
    json += "\"soilAdc\":"        + String(liveSoilADC) + ",";
    json += "\"soilV\":"          + String(liveSoilVoltage, 3) + ",";

    // Target Limits
    json += "\"tempMin\":"        + String(limitTempMin, 1) + ",";
    json += "\"tempMax\":"        + String(limitTempMax, 1) + ",";
    json += "\"humMin\":"         + String(limitHumMin, 1) + ",";
    json += "\"humMax\":"         + String(limitHumMax, 1) + ",";
    json += "\"luxMin\":"         + String(limitLuxMin, 0) + ",";
    json += "\"luxMax\":"         + String(limitLuxMax, 0) + ",";
    json += "\"soilMin\":"        + String(limitSoilMin, 1) + ",";
    json += "\"soilMax\":"        + String(limitSoilMax, 1) + ",";

    json += "\"tbHttp\":"         + String(lastTbHttpCode) + ",";
    json += "\"tbSent\":"         + String(tbSendCount);
    json += "}";

    server.send(200, "application/json", json);
}

void handleSaveLimits() {
    if (!isClientAuthenticated()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    if (server.hasArg("tempMin")) limitTempMin = server.arg("tempMin").toFloat();
    if (server.hasArg("tempMax")) limitTempMax = server.arg("tempMax").toFloat();
    if (server.hasArg("humMin"))  limitHumMin  = server.arg("humMin").toFloat();
    if (server.hasArg("humMax"))  limitHumMax  = server.arg("humMax").toFloat();
    if (server.hasArg("luxMin"))  limitLuxMin  = server.arg("luxMin").toFloat();
    if (server.hasArg("luxMax"))  limitLuxMax  = server.arg("luxMax").toFloat();
    if (server.hasArg("soilMin")) limitSoilMin = server.arg("soilMin").toFloat();
    if (server.hasArg("soilMax")) limitSoilMax = server.arg("soilMax").toFloat();

    prefLimits.putFloat("tMin", limitTempMin);
    prefLimits.putFloat("tMax", limitTempMax);
    prefLimits.putFloat("hMin", limitHumMin);
    prefLimits.putFloat("hMax", limitHumMax);
    prefLimits.putFloat("lMin", limitLuxMin);
    prefLimits.putFloat("lMax", limitLuxMax);
    prefLimits.putFloat("sMin", limitSoilMin);
    prefLimits.putFloat("sMax", limitSoilMax);

    server.send(200, "text/plain", "Limits saved successfully!");
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

// Wireless Web OTA Handlers
void handleOtaPage() {
    if (!isClientAuthenticated()) {
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
        return;
    }
    server.send(200, "text/html", OTA_UPDATE_HTML);
}

void handleOtaUploadDone() {
    if (!isClientAuthenticated()) {
        server.send(401, "text/plain", "Unauthorized");
        return;
    }
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "OTA Update Failed!" : "OTA Update Success! Rebooting...");
    delay(1000);
    ESP.restart();
}

void handleOtaChunk() {
    if (!isClientAuthenticated()) return;
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("OTA Update Started: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("OTA Update Success: %u bytes\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

// =====================================================================
//  Setup
// =====================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    printLine('=');
    Serial.println(F("  ESP32-S3 — MODULE 3: LIGHT & SOIL ENVIRONMENT HUB"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | BH1750 (Wire1: SDA=5/15, SCL=16) | Soil (GPIO 1)"));
    Serial.println(F("  Display: 2004 I2C LCD (Wire: SDA=17, SCL=18)"));
    Serial.println(F("  Utility: WebServer AP (192.168.4.1) + Mobile Limits + Wireless OTA"));
    printLine('=');

    // 1. Initialize DHT11
    dht.begin();
    Serial.println(F("  [OK] DHT11 Initialized on GPIO 4"));

    // 2. Initialize Soil Sensor ADC
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(SOIL_PIN, INPUT);
    Serial.println(F("  [OK] Soil Moisture Sensor on GPIO 1 (ADC1_CH0)"));

    // 3. Initialize 2004 I2C LCD
    pinMode(LCD_SDA_PIN, INPUT_PULLUP);
    pinMode(LCD_SCL_PIN, INPUT_PULLUP);
    Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN, 50000);
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
        Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN, 50000);
        lcd.backlight();
        lcd.display();
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print(F("===================="));
        lcd.setCursor(0, 1); lcd.print(F("    Welcome to      "));
        lcd.setCursor(0, 2); lcd.print(F("  Module-3 Monitor  "));
        lcd.setCursor(0, 3); lcd.print(F("===================="));
        lcd_available = true;
        Serial.printf("  [OK] 2004 LCD Online at 0x%02X\n", lcdAddr);
    } else {
        Serial.println(F("  [..] 2004 LCD Not Detected on Wire (SDA=17, SCL=18)"));
    }

    // 4. Initialize BH1750
    initBH1750Sensor();

    // 5. Load Stored WiFi, Auth and Target Limits
    prefWifi.begin("m3-wifi", false);
    wifi_ssid = prefWifi.getString("ssid", "Meeting Room");
    wifi_pass = prefWifi.getString("pass", "Shahid786$$");

    prefAuth.begin("m3-auth", false);
    auth_user = prefAuth.getString("user", "admin");
    auth_pass = prefAuth.getString("pass", "admin");

    prefLimits.begin("m3-limits", false);
    limitTempMin = prefLimits.getFloat("tMin", 18.0f);
    limitTempMax = prefLimits.getFloat("tMax", 32.0f);
    limitHumMin  = prefLimits.getFloat("hMin", 35.0f);
    limitHumMax  = prefLimits.getFloat("hMax", 75.0f);
    limitLuxMin  = prefLimits.getFloat("lMin", 100.0f);
    limitLuxMax  = prefLimits.getFloat("lMax", 5000.0f);
    limitSoilMin = prefLimits.getFloat("sMin", 25.0f);
    limitSoilMax = prefLimits.getFloat("sMax", 70.0f);

    // 6. Start Dual WiFi Mode (AP + STA)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("  [OK] Access Point Active: SSID '%s' (IP: 192.168.4.1)\n", AP_SSID);

    if (wifi_ssid.length() > 0) {
        Serial.printf("  [..] Connecting to Station SSID '%s'...\n", wifi_ssid.c_str());
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }

    if (MDNS.begin("esp32lightsoil")) {
        MDNS.addService("http", "tcp", 80);
    }

    // 7. ArduinoOTA Wireless Flashing Setup
    ArduinoOTA.setHostname("esp32-s3-module3");
    ArduinoOTA.setPassword("admin");
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) type = "sketch";
        else type = "filesystem";
        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
    });
    ArduinoOTA.begin();
    Serial.println(F("  [OK] ArduinoOTA Initialized (Hostname: esp32-s3-module3)"));

    // 8. Configure Web Server Headers & Routes
    const char* headerkeys[] = {"Cookie"};
    size_t headerkeyssize = sizeof(headerkeys) / sizeof(char*);
    server.collectHeaders(headerkeys, headerkeyssize);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/login", HTTP_POST, handleLogin);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/data", HTTP_GET, handleData);
    server.on("/savelimits", HTTP_POST, handleSaveLimits);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/savewifi", HTTP_POST, handleSaveWiFi);
    server.on("/resetwifi", HTTP_POST, handleResetWiFi);

    // OTA Update Routes
    server.on("/update", HTTP_GET, handleOtaPage);
    server.on("/update", HTTP_POST, handleOtaUploadDone, handleOtaChunk);

    server.begin();
    Serial.println(F("  [OK] WebServer Started on Port 80 (Login Protected)"));
    printLine('=');

    if (lcd_available) {
        delay(1200);
        lcd.clear();
    }
}

// =====================================================================
//  Loop
// =====================================================================
void loop() {
    // 1. Process Incoming HTTP Clients Immediately
    server.handleClient();

    // 2. Handle ArduinoOTA Wireless Packets
    ArduinoOTA.handle();

    // 3. Periodic Sensor Sampling & Telemetry Dispatch (Non-blocking)
    unsigned long now = millis();
    if (now - lastSampleTime >= SAMPLE_INTERVAL) {
        lastSampleTime = now;
        loopCount++;

        // Auto-reconnect BH1750 if offline
        static unsigned long lastBhRetry = 0;
        if (!bh1750_available && (millis() - lastBhRetry > 5000)) {
            lastBhRetry = millis();
            initBH1750Sensor();
        }

        // Read DHT11
        float hum = dht.readHumidity();
        float tC  = dht.readTemperature();
        float tF  = dht.readTemperature(true);
        if (isnan(hum) || isnan(tC)) {
            hum = 50.0f; tC = 25.0f; tF = 77.0f;
            liveDhtOk = false;
        } else {
            liveDhtOk = true;
        }
        liveTempC = tC;
        liveTempF = tF;
        liveHum   = hum;
        liveHeatIndex = dht.computeHeatIndex(tC, hum, false);

        // Read BH1750 Light
        if (bh1750_available) {
            float r = lightMeter.readLightLevel();
            if (r >= 0) {
                currentLux = r;
            } else {
                bh1750_available = false;
            }
        }

        // Read Soil Moisture
        liveSoilMoisture = readSoilMoisture(liveSoilADC, liveSoilVoltage);

        const char* lightStatus = getLightStatus(currentLux);
        const char* soilStatus  = getSoilStatus(liveSoilMoisture);

        // Send ThingsBoard Telemetry
        sendTelemetry(liveTempC, liveTempF, liveHum, liveHeatIndex,
                      currentLux, lightStatus,
                      liveSoilMoisture, liveSoilVoltage, liveSoilADC, soilStatus);

        // Serial Logging
        Serial.printf("[M3 #%lu] T:%.1fC | H:%.1f%% | Lux:%.0f (%s) | Soil:%.1f%% (%s) | WiFi:%s\n",
                      loopCount, liveTempC, liveHum, currentLux, lightStatus,
                      liveSoilMoisture, soilStatus,
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "AP-Only");

        // 2004 LCD Display Update
        if (lcd_available) {
            char buf[21];
            // Row 0: Temperature & Humidity
            if (liveDhtOk) {
                snprintf(buf, sizeof(buf), "Temp: %4.1fC Hum:%2.0f%%", liveTempC, liveHum);
            } else {
                snprintf(buf, sizeof(buf), "Temp: --.-C  Hum:--%% ");
            }
            lcd.setCursor(0, 0); lcd.print(buf);

            // Row 1: Light
            if (bh1750_available) {
                snprintf(buf, sizeof(buf), "Lux : %5.0f [%-6s] ", currentLux, shortLightStatus(currentLux));
            } else {
                snprintf(buf, sizeof(buf), "Lux : OFFLINE       ");
            }
            lcd.setCursor(0, 1); lcd.print(buf);

            // Row 2: Soil
            snprintf(buf, sizeof(buf), "Soil: %4.1f%% [%-5s] ", liveSoilMoisture, shortSoilStatus(liveSoilMoisture));
            lcd.setCursor(0, 2); lcd.print(buf);

            // Row 3: WiFi IP or AP IP
            if (WiFi.status() == WL_CONNECTED) {
                snprintf(buf, sizeof(buf), "IP:%-15s", WiFi.localIP().toString().c_str());
            } else {
                snprintf(buf, sizeof(buf), "AP: 192.168.4.1     ");
            }
            lcd.setCursor(0, 3); lcd.print(buf);
        }
    }
}
