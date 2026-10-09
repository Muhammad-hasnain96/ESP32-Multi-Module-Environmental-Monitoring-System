// =====================================================================
// ESP32-S3 — MODULE 1: Air Quality, Gas, Flow & 2004 LCD System
// With Dual AP+STA WiFi Utility, Mobile Min/Max Threshold Controls & Wireless OTA
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
//
//  * Access Point: "ESP32-S3_Air_Quality" (192.168.4.1)
//  * Web Login: admin / admin
//  * Wireless OTA: Accessible via /update or ArduinoOTA (port 3232)
// =====================================================================

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Adafruit_CCS811.h>
#include <LiquidCrystal_I2C.h>
#include <Update.h>
#include <ArduinoOTA.h>

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
//  6. WiFi, WebServer, Preferences & ThingsBoard Configuration
// =====================================================================
const char* AP_SSID = "ESP32-S3_Air_Quality";
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

String auth_user = "admin";
String auth_pass = "admin";

// =====================================================================
//  7. User-Configurable Min & Max Target Limits (Persistent in Flash)
// =====================================================================
float limitTempMin = 18.0f;
float limitTempMax = 32.0f;
float limitHumMin  = 35.0f;
float limitHumMax  = 75.0f;
float limitCo2Min  = 400.0f;
float limitCo2Max  = 1000.0f;
float limitNh3Min  = 0.0f;
float limitNh3Max  = 25.0f;

// Live Readings & States
float currentTC = 25.0f, currentTF = 77.0f, currentHum = 50.0f, currentHI = 25.0f;
bool  currentDhtOk = false;
float currentNH3 = 0.0f, currentMQ137V = 0.0f;
bool  currentNH3Alert = false;
float currentFlowHz = 0.0f;
unsigned long totalFlowPulses = 0;

const unsigned long INTERVAL          = 3000UL;
const unsigned long LCD_PAGE_INTERVAL = 5000UL;
unsigned long lastLog = 0;
unsigned long lastLcdSwitch = 0;
unsigned long loopCount = 0;
int lcdScreenPage = 0;

// =====================================================================
//  Helpers & Status Classifications
// =====================================================================
String uptime() {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600; s %= 3600;
    unsigned long m = s / 60;   s %= 60;
    char b[12]; snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", h, m, s);
    return String(b);
}

void printLine(char c = '-') {
    for (int i = 0; i < 60; i++) Serial.print(c);
    Serial.println();
}

int avgADC(int pin, int n = 10) {
    long sum = 0;
    for (int i = 0; i < n; i++) { sum += analogRead(pin); delay(2); }
    return (int)(sum / n);
}

const char* nh3Label(float p, bool w) {
    if (!w)         return "WARMING UP";
    if (p <   5.0f) return "CLEAN";
    if (p <  25.0f) return "LOW";
    if (p <  50.0f) return "MODERATE";
    if (p < 100.0f) return "HIGH";
    return                  "DANGER";
}

const char* co2Label(uint16_t v) {
    if (v <  600) return "EXCELLENT";
    if (v <  800) return "GOOD";
    if (v < 1000) return "FAIR";
    if (v < 1500) return "MEDIOCRE";
    if (v < 2000) return "BAD";
    return               "HAZARDOUS";
}

const char* tvocLabel(uint16_t v) {
    if (v <  50)   return "CLEAN";
    if (v < 150)   return "GOOD";
    if (v < 300)   return "MODERATE";
    if (v < 500)   return "POOR";
    return                "HAZARDOUS";
}

const char* flowLabel(float hz) {
    if (hz == 0)   return "NONE";
    if (hz < 5.0f) return "VERY LOW";
    if (hz < 15.0f)return "LOW";
    if (hz < 35.0f)return "MODERATE";
    return                "HIGH";
}

const char* shortCo2Label(uint16_t v) {
    if (v <  600) return "EXCL";
    if (v < 1000) return "GOOD";
    if (v < 1500) return "FAIR";
    if (v < 2500) return "POOR";
    return               "HAZD";
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
//  ThingsBoard Telemetry Dispatch
// =====================================================================
void sendTelemetry(float tC, float tF, float hum, float hi, bool dhtOk,
                   float nh3, const char* nh3Lv, float mq137V, bool alert,
                   unsigned long pulses, float flowHz, float vol, const char* flowLv,
                   uint16_t eco2, const char* co2Lv, uint16_t tvoc, const char* tvocLv, bool ccsOk) {
    if (WiFi.status() != WL_CONNECTED) return;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    https.setTimeout(8000);

    String url = String(TB_HOST) + "/api/v1/" + TB_TOKEN + "/telemetry";
    https.begin(client, url);
    https.addHeader("Content-Type", "application/json");

    String p = "{";
    if (dhtOk) {
        p += "\"temperature\":"    + String(tC, 1);
        p += ",\"temperatureF\":"  + String(tF, 1);
        p += ",\"humidity\":"      + String(hum, 1);
        p += ",\"heatIndex\":"     + String(hi, 1);
    } else {
        p += "\"temperatureStatus\":\"OFFLINE\"";
    }
    p += ",\"nh3PPM\":"        + String(nh3, 1);
    p += ",\"nh3Level\":\""    + String(nh3Lv) + "\"";
    p += ",\"mq137V\":"        + String(mq137V, 3);
    p += ",\"nh3Alert\":"      + String(alert ? 1 : 0);
    p += ",\"flowPulses\":"    + String(pulses);
    p += ",\"flowHz\":"        + String(flowHz, 2);
    p += ",\"flowVolume\":"    + String(vol, 2);
    p += ",\"flowStatus\":\""  + String(flowLv) + "\"";
    if (ccsOk) {
        p += ",\"eCO2\":"          + String(eco2);
        p += ",\"eco2Status\":\""  + String(co2Lv) + "\"";
        p += ",\"tvoc\":"          + String(tvoc);
        p += ",\"tvocStatus\":\""  + String(tvocLv) + "\"";
    } else {
        p += ",\"eco2Status\":\"OFFLINE\"";
        p += ",\"tvocStatus\":\"OFFLINE\"";
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
        // SCREEN 1: Ambient Environment (Temp, Humidity, Heat Index)
        snprintf(buf, sizeof(buf), "-- AIR QUALITY [1/2]");
        lcd.setCursor(0, 0); lcd.print(buf);

        if (currentDhtOk) {
            snprintf(buf, sizeof(buf), "Temp    : %5.1f C   ", currentTC);
        } else {
            snprintf(buf, sizeof(buf), "Temp    : --.- C   ");
        }
        lcd.setCursor(0, 1); lcd.print(buf);

        if (currentDhtOk) {
            snprintf(buf, sizeof(buf), "Humidity: %5.1f %%   ", currentHum);
        } else {
            snprintf(buf, sizeof(buf), "Humidity: --.- %%   ");
        }
        lcd.setCursor(0, 2); lcd.print(buf);

        if (currentDhtOk) {
            snprintf(buf, sizeof(buf), "Heat Idx: %5.1f C   ", currentHI);
        } else {
            snprintf(buf, sizeof(buf), "Heat Idx: --.- C   ");
        }
        lcd.setCursor(0, 3); lcd.print(buf);
    } else {
        // SCREEN 2: Gas & Flow (CO2, NH3, Air Flow)
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
//  Authentication Management
// =====================================================================
const char* AUTH_COOKIE_NAME = "m1_auth";
const char* AUTH_TOKEN_VALUE = "valid_m1_session_786";

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
    <title>Module 1 Login | ESP32-S3 Air Quality Hub</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #38bdf8;
            --primary-hover: #0284c7;
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
        <div class="icon">💨</div>
        <h1>ESP32-S3 Module 1</h1>
        <p class="sub">Air Quality, Gas & Flow Monitor</p>

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

const char OTA_UPDATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Wireless OTA Update | ESP32-S3 Module 1</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #38bdf8;
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
        .btn:hover { background: #0284c7; color: #fff; }
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
        <p class="sub">Upload compiled <b>firmware.bin</b> over Wi-Fi without any cables!</p>

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

        <a href="/" class="back-link">← Return to Air Quality Dashboard</a>
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
    <title>ESP32-S3 Module 1 | Air Quality & Gas Monitor</title>
    <style>
        :root {
            --bg: #0f172a;
            --card-bg: #1e293b;
            --primary: #38bdf8;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --border: #334155;
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
            background: rgba(56, 189, 248, 0.08);
            border: 1px solid rgba(56, 189, 248, 0.3);
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
        .status-info { background: rgba(56, 189, 248, 0.2); color: var(--primary); }
        .status-purple { background: rgba(168, 85, 247, 0.2); color: var(--purple); }
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
        .fill-blue { background: var(--primary); }
        .fill-emerald { background: var(--emerald); }
        .fill-amber { background: var(--amber); }
        .fill-purple { background: var(--purple); }

        /* Configuration Box */
        .section-box {
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 14px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .section-header { font-size: 1.1rem; font-weight: 700; margin-bottom: 14px; display: flex; align-items: center; gap: 8px; }

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
        .btn-action:hover { background: #0284c7; color: #fff; }

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
            <div class="nav-brand">💨 <span>Module 1</span> Air Quality & Gas Hub</div>
            <div class="nav-actions">
                <a href="/update" class="btn-nav">⚡ Wireless OTA</a>
                <button class="btn-logout" onclick="window.location.href='/logout'">Sign Out</button>
            </div>
        </div>

        <!-- Status Banner -->
        <div class="banner">
            <div class="banner-pill">
                <span class="dot pulse"></span>
                <span><b>AP Mode:</b> ESP32-S3_Air_Quality (192.168.4.1)</span>
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
            <!-- 1. Temperature & Humidity -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Temperature & Hum</span>
                    <span class="card-icon">🌡️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valTemp">--.-</span><span class="card-unit">°C</span></div>
                    <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:8px;">
                        Humidity: <b style="color:#fff;" id="valHum">--.-</b> % | HeatIdx: <b style="color:#fff;" id="valHi">--.-</b> °C
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center;">
                    <div class="card-status status-info" id="statusDht">DHT11 (GPIO 4)</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblTempRange">Target: -- - -- °C</span>
                </div>
            </div>

            <!-- 2. Carbon Dioxide eCO2 -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Carbon Dioxide (eCO2)</span>
                    <span class="card-icon">💨</span>
                </div>
                <div>
                    <div class="card-value"><span id="valEco2">---</span><span class="card-unit">ppm</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-emerald" id="barEco2" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="statusEco2">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblCo2Range">Target: -- - -- ppm</span>
                </div>
            </div>

            <!-- 3. Total VOCs (TVOC) -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Volatile Organics (TVOC)</span>
                    <span class="card-icon">🧪</span>
                </div>
                <div>
                    <div class="card-value"><span id="valTvoc">---</span><span class="card-unit">ppb</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-purple" id="barTvoc" style="width: 0%;"></div>
                    </div>
                </div>
                <div class="card-status status-purple" id="statusTvoc" style="margin-top:10px;">Air Purity</div>
            </div>

            <!-- 4. Ammonia Gas NH3 -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Ammonia Gas (NH3)</span>
                    <span class="card-icon">☣️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valNh3">--.-</span><span class="card-unit">ppm</span></div>
                    <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:8px;">Sensor: <span id="valNh3V">--.-</span> V</div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center;">
                    <div class="card-status status-optimal" id="statusNh3">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="lblNh3Range">Target: -- - -- ppm</span>
                </div>
            </div>

            <!-- 5. Air / Water Flow -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Flow Rate & Volume</span>
                    <span class="card-icon">🌊</span>
                </div>
                <div>
                    <div class="card-value"><span id="valFlow">--.-</span><span class="card-unit">Hz</span></div>
                    <div style="font-size:0.85rem; color:var(--text-muted); margin-bottom:8px;">
                        Total Volume: <b style="color:#fff;" id="valVol">--.-</b> L
                    </div>
                </div>
                <div class="card-status status-info" id="statusFlow">FS200A (GPIO 5)</div>
            </div>

            <!-- 6. Cloud Telemetry -->
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

                <!-- CO2 Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">💨 CO2 Limits (ppm)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min CO2</label>
                            <input type="number" step="50" id="inpCo2Min" placeholder="400">
                        </div>
                        <div class="limit-field">
                            <label>Max CO2</label>
                            <input type="number" step="50" id="inpCo2Max" placeholder="1000">
                        </div>
                    </div>
                </div>

                <!-- Ammonia Limits -->
                <div class="limit-card">
                    <div class="limit-card-title">☣️ Ammonia NH3 Limits (ppm)</div>
                    <div class="limit-inputs">
                        <div class="limit-field">
                            <label>Min NH3</label>
                            <input type="number" step="1" id="inpNh3Min" placeholder="0">
                        </div>
                        <div class="limit-field">
                            <label>Max NH3</label>
                            <input type="number" step="1" id="inpNh3Max" placeholder="25">
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

                    // Populate limits on initial load
                    if (!limitsLoaded && d.tempMin !== undefined) {
                        document.getElementById('inpTempMin').value = d.tempMin;
                        document.getElementById('inpTempMax').value = d.tempMax;
                        document.getElementById('inpHumMin').value = d.humMin;
                        document.getElementById('inpHumMax').value = d.humMax;
                        document.getElementById('inpCo2Min').value = d.co2Min;
                        document.getElementById('inpCo2Max').value = d.co2Max;
                        document.getElementById('inpNh3Min').value = d.nh3Min;
                        document.getElementById('inpNh3Max').value = d.nh3Max;
                        limitsLoaded = true;
                    }

                    // Target labels
                    document.getElementById('lblTempRange').innerText = d.tempMin + ' - ' + d.tempMax + ' °C';
                    document.getElementById('lblCo2Range').innerText = 'Target: ' + d.co2Min + ' - ' + d.co2Max + ' ppm';
                    document.getElementById('lblNh3Range').innerText = 'Target: ' + d.nh3Min + ' - ' + d.nh3Max + ' ppm';

                    // 1. Temp & Hum
                    if (d.dhtOk) {
                        document.getElementById('valTemp').innerText = d.temp.toFixed(1);
                        document.getElementById('valHum').innerText = d.hum.toFixed(1);
                        document.getElementById('valHi').innerText = d.hi.toFixed(1);
                        const sTemp = document.getElementById('statusDht');
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
                        document.getElementById('valHum').innerText = '--.-';
                        document.getElementById('valHi').innerText = '--.-';
                        document.getElementById('statusDht').innerText = 'OFFLINE';
                        document.getElementById('statusDht').className = 'card-status status-err';
                    }

                    // 2. eCO2
                    if (d.ccsOk && d.eco2 !== null) {
                        document.getElementById('valEco2').innerText = d.eco2;
                        document.getElementById('barEco2').style.width = Math.min((d.eco2 / 2000) * 100, 100) + '%';
                        const sEco2 = document.getElementById('statusEco2');
                        if (d.eco2 > d.co2Max) {
                            sEco2.innerText = '⚠️ HIGH CO2 (> ' + d.co2Max + ' ppm)';
                            sEco2.className = 'card-status status-err';
                        } else {
                            sEco2.innerText = d.eco2Status;
                            sEco2.className = 'card-status status-optimal';
                        }
                    } else {
                        document.getElementById('valEco2').innerText = '--';
                        document.getElementById('statusEco2').innerText = 'OFFLINE';
                        document.getElementById('statusEco2').className = 'card-status status-err';
                        document.getElementById('barEco2').style.width = '0%';
                    }

                    // 3. TVOC
                    if (d.ccsOk && d.tvoc !== null) {
                        document.getElementById('valTvoc').innerText = d.tvoc;
                        document.getElementById('statusTvoc').innerText = d.tvocStatus;
                        document.getElementById('statusTvoc').className = 'card-status status-purple';
                        document.getElementById('barTvoc').style.width = Math.min((d.tvoc / 1000) * 100, 100) + '%';
                    } else {
                        document.getElementById('valTvoc').innerText = '--';
                        document.getElementById('statusTvoc').innerText = 'OFFLINE';
                        document.getElementById('statusTvoc').className = 'card-status status-err';
                        document.getElementById('barTvoc').style.width = '0%';
                    }

                    // 4. NH3
                    document.getElementById('valNh3').innerText = d.nh3.toFixed(1);
                    document.getElementById('valNh3V').innerText = d.nh3V.toFixed(2);
                    const sNh3 = document.getElementById('statusNh3');
                    if (d.nh3 > d.nh3Max) {
                        sNh3.innerText = '⚠️ HIGH NH3 (> ' + d.nh3Max + ' ppm)';
                        sNh3.className = 'card-status status-err';
                    } else {
                        sNh3.innerText = d.nh3Status;
                        sNh3.className = 'card-status status-optimal';
                    }

                    // 5. Flow
                    document.getElementById('valFlow').innerText = d.flowHz.toFixed(1);
                    document.getElementById('valVol').innerText = d.vol.toFixed(2);
                    document.getElementById('statusFlow').innerText = d.flowStatus;

                    // 6. ThingsBoard
                    document.getElementById('tbStatus').innerText = d.tbHttp > 0 ? 'HTTP ' + d.tbHttp : 'Pending';
                    document.getElementById('tbStatus').className = (d.tbHttp === 200) ? 'card-status status-optimal' : 'card-status status-warn';
                    document.getElementById('tbCount').innerText = 'Sent: ' + d.tbSent;
                })
                .catch(e => console.log('Poll error:', e));
        }

        function saveLimits() {
            const body = new URLSearchParams();
            body.append('tempMin', document.getElementById('inpTempMin').value);
            body.append('tempMax', document.getElementById('inpTempMax').value);
            body.append('humMin',  document.getElementById('inpHumMin').value);
            body.append('humMax',  document.getElementById('inpHumMax').value);
            body.append('co2Min',  document.getElementById('inpCo2Min').value);
            body.append('co2Max',  document.getElementById('inpCo2Max').value);
            body.append('nh3Min',  document.getElementById('inpNh3Min').value);
            body.append('nh3Max',  document.getElementById('inpNh3Max').value);

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
    json += "\"dhtOk\":"          + String(currentDhtOk ? "true" : "false") + ",";
    json += "\"temp\":"           + String(currentTC, 1) + ",";
    json += "\"tempF\":"          + String(currentTF, 1) + ",";
    json += "\"hum\":"            + String(currentHum, 1) + ",";
    json += "\"hi\":"             + String(currentHI, 1) + ",";
    json += "\"ccsOk\":"          + String(ccsOK ? "true" : "false") + ",";
    if (ccsOK) {
        json += "\"eco2\":"         + String(ccsECO2) + ",";
        json += "\"eco2Status\":\"" + String(co2Label(ccsECO2)) + "\",";
        json += "\"tvoc\":"         + String(ccsTVOC) + ",";
        json += "\"tvocStatus\":\"" + String(tvocLabel(ccsTVOC)) + "\",";
    } else {
        json += "\"eco2\":null,";
        json += "\"eco2Status\":\"OFFLINE\",";
        json += "\"tvoc\":null,";
        json += "\"tvocStatus\":\"OFFLINE\",";
    }
    json += "\"nh3\":"            + String(currentNH3, 1) + ",";
    json += "\"nh3V\":"           + String(currentMQ137V, 3) + ",";
    json += "\"nh3Status\":\""    + String(nh3Label(currentNH3, mq137Warmed)) + "\",";
    json += "\"nh3Alert\":"       + String(currentNH3Alert ? "true" : "false") + ",";
    json += "\"flowHz\":"         + String(currentFlowHz, 2) + ",";
    json += "\"vol\":"            + String(totalVolume, 2) + ",";
    json += "\"flowStatus\":\""   + String(flowLabel(currentFlowHz)) + "\",";

    // Target Limits
    json += "\"tempMin\":"        + String(limitTempMin, 1) + ",";
    json += "\"tempMax\":"        + String(limitTempMax, 1) + ",";
    json += "\"humMin\":"         + String(limitHumMin, 1) + ",";
    json += "\"humMax\":"         + String(limitHumMax, 1) + ",";
    json += "\"co2Min\":"         + String(limitCo2Min, 0) + ",";
    json += "\"co2Max\":"         + String(limitCo2Max, 0) + ",";
    json += "\"nh3Min\":"         + String(limitNh3Min, 1) + ",";
    json += "\"nh3Max\":"         + String(limitNh3Max, 1) + ",";

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
    if (server.hasArg("co2Min"))  limitCo2Min  = server.arg("co2Min").toFloat();
    if (server.hasArg("co2Max"))  limitCo2Max  = server.arg("co2Max").toFloat();
    if (server.hasArg("nh3Min"))  limitNh3Min  = server.arg("nh3Min").toFloat();
    if (server.hasArg("nh3Max"))  limitNh3Max  = server.arg("nh3Max").toFloat();

    prefLimits.putFloat("tMin", limitTempMin);
    prefLimits.putFloat("tMax", limitTempMax);
    prefLimits.putFloat("hMin", limitHumMin);
    prefLimits.putFloat("hMax", limitHumMax);
    prefLimits.putFloat("cMin", limitCo2Min);
    prefLimits.putFloat("cMax", limitCo2Max);
    prefLimits.putFloat("nMin", limitNh3Min);
    prefLimits.putFloat("nMax", limitNh3Max);

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
    Serial.println(F("  ESP32-S3 MODULE 1: AIR QUALITY, GAS & FLOW SYSTEM"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | MQ-137 (GPIO 1/2)"));
    Serial.println(F("           FS200A (GPIO 5) | CCS811 (Wire1: SDA=17, SCL=16)"));
    Serial.println(F("  Display: 2004 I2C LCD (Wire: SDA=15, SCL=18)"));
    Serial.println(F("  Utility: WebServer AP (192.168.4.1) + Mobile Limits + Wireless OTA"));
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
        lcd.setCursor(0, 2); lcd.print(F("  Module-1 Monitor  "));
        lcd.setCursor(0, 3); lcd.print(F("===================="));
        lcd_available = true;
        Serial.printf("  [OK] 2004 LCD Online at 0x%02X\n", lcdAddr);
    } else {
        Serial.println(F("  [..] 2004 LCD Not Detected on Wire (SDA=15, SCL=18)"));
    }

    // 5. CCS811 on I2C Bus 1 (Wire1: SDA=17, SCL=16)
    pinMode(CCS811_SDA_PIN, INPUT_PULLUP);
    pinMode(CCS811_SCL_PIN, INPUT_PULLUP);
    Wire1.begin(CCS811_SDA_PIN, CCS811_SCL_PIN, 50000);
    Wire1.setTimeOut(500);
    if (initCCS()) {
        Serial.println(F("  [OK] CCS811   -> Wire1 SDA=17 SCL=16 [ONLINE]"));
    } else {
        Serial.println(F("  [..] CCS811   -> OFFLINE (Check WAK->GND, RST->3.3V, SDA=17, SCL=16)"));
    }

    // 6. Preferences for WiFi, Auth & Limits
    prefWifi.begin("m1-wifi", false);
    wifi_ssid = prefWifi.getString("ssid", "Meeting Room");
    wifi_pass = prefWifi.getString("pass", "Shahid786$$");

    prefAuth.begin("m1-auth", false);
    auth_user = prefAuth.getString("user", "admin");
    auth_pass = prefAuth.getString("pass", "admin");

    prefLimits.begin("m1-limits", false);
    limitTempMin = prefLimits.getFloat("tMin", 18.0f);
    limitTempMax = prefLimits.getFloat("tMax", 32.0f);
    limitHumMin  = prefLimits.getFloat("hMin", 35.0f);
    limitHumMax  = prefLimits.getFloat("hMax", 75.0f);
    limitCo2Min  = prefLimits.getFloat("cMin", 400.0f);
    limitCo2Max  = prefLimits.getFloat("cMax", 1000.0f);
    limitNh3Min  = prefLimits.getFloat("nMin", 0.0f);
    limitNh3Max  = prefLimits.getFloat("nMax", 25.0f);

    // 7. Dual WiFi Mode (AP + STA)
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("  [OK] Access Point Active: SSID '%s' (IP: 192.168.4.1)\n", AP_SSID);

    if (wifi_ssid.length() > 0) {
        Serial.printf("  [..] Connecting to Station SSID '%s'...\n", wifi_ssid.c_str());
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }

    if (MDNS.begin("esp32airquality")) {
        MDNS.addService("http", "tcp", 80);
    }

    // 8. ArduinoOTA Wireless Flashing Setup
    ArduinoOTA.setHostname("esp32-s3-module1");
    ArduinoOTA.setPassword("admin");
    ArduinoOTA.begin();
    Serial.println(F("  [OK] ArduinoOTA Initialized (Hostname: esp32-s3-module1)"));

    // 9. Configure Web Server Headers & Routes
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
        updateLCD();
    }

    lastLog = millis();
    lastLcdSwitch = millis();
}

// =====================================================================
//  Loop
// =====================================================================
void loop() {
    // 1. Process Incoming HTTP Clients Immediately
    server.handleClient();

    // 2. Handle ArduinoOTA Wireless Packets
    ArduinoOTA.handle();

    // 3. Throttled CCS811 retry every 5s if offline
    static unsigned long lastCcsRetry = 0;
    if (!ccsOK && (millis() - lastCcsRetry >= 5000)) {
        lastCcsRetry = millis();
        initCCS();
    }

    unsigned long now = millis();

    // 4. Read sensors & send telemetry every 3 seconds (INTERVAL)
    if (now - lastLog >= INTERVAL) {
        lastLog = now;
        loopCount++;

        // ---- 1. FS200A ----
        noInterrupts();
        unsigned long pulses = flowPulses; flowPulses = 0;
        interrupts();
        totalFlowPulses += pulses;
        currentFlowHz = pulses / (INTERVAL / 1000.0f);
        totalVolume  += pulses / PULSES_PER_L;

        // ---- 2. DHT11 ----
        float hum  = dht.readHumidity();
        float tC   = dht.readTemperature();
        float tF   = dht.readTemperature(true);
        if (isnan(hum) || isnan(tC)) {
            hum = 50.0f; tC = 25.0f; tF = 77.0f;
            currentDhtOk = false;
        } else {
            currentDhtOk = true;
        }
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
        currentNH3Alert = alert;

        // ---- 4. CCS811 ----
        if (ccsOK) {
            ccs.setEnvironmentalData(hum, tC);
            if (ccs.available() && !ccs.readData()) {
                ccsECO2 = ccs.geteCO2();
                ccsTVOC = ccs.getTVOC();
            }
        }

        // Serial Output
        if (ccsOK) {
            Serial.printf("[M1 #%lu] T:%.1fC | H:%.1f%% | eCO2:%u (%s) | NH3:%.1f (%s) | Flow:%.1fHz | WiFi:%s\n",
                          loopCount, currentTC, currentHum, ccsECO2, co2Label(ccsECO2),
                          currentNH3, nh3Label(currentNH3, mq137Warmed), currentFlowHz,
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "AP-Only");
        } else {
            Serial.printf("[M1 #%lu] T:%.1fC | H:%.1f%% | eCO2:OFFLINE | NH3:%.1f (%s) | Flow:%.1fHz | WiFi:%s\n",
                          loopCount, currentTC, currentHum,
                          currentNH3, nh3Label(currentNH3, mq137Warmed), currentFlowHz,
                          WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "AP-Only");
        }

        // Send ThingsBoard Telemetry
        sendTelemetry(currentTC, currentTF, currentHum, currentHI, currentDhtOk,
                      currentNH3, nh3Label(currentNH3, mq137Warmed), currentMQ137V, currentNH3Alert,
                      pulses, currentFlowHz, totalVolume, flowLabel(currentFlowHz),
                      ccsECO2, co2Label(ccsECO2), ccsTVOC, tvocLabel(ccsTVOC), ccsOK);

        // Update live screen numbers
        if (lcd_available) {
            updateLCD();
        }
    }

    // 5. Rotate LCD screen page every 5 seconds (5000ms)
    if (lcd_available) {
        if (now - lastLcdSwitch >= LCD_PAGE_INTERVAL) {
            lastLcdSwitch = now;
            lcdScreenPage = (lcdScreenPage + 1) % 2;
            lcd.clear();
            updateLCD();
        }
    }
}
