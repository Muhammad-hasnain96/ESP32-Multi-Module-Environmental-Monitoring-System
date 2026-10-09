// =====================================================================
// ESP32-S3 — MODULE 3: Light, Soil Moisture & Environmental System
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
//   * NO RELAYS IN THIS MODULE (Pure Monitoring Hub)
//   * Access Point: "ESP32-S3_Light_Soil" (192.168.4.1)
//   * Login Credentials: Username: admin | Password: admin
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
//  2. WiFi, WebServer, Preferences & ThingsBoard Configuration
// =====================================================================
const char* AP_SSID = "ESP32-S3_Light_Soil";
const char* AP_PASS = "12345678"; // Min 8 chars for WPA2 AP

// Default Station credentials
String wifi_ssid = "Meeting Room";
String wifi_pass = "Shahid786$$";

// ThingsBoard Cloud
const char* TB_HOST   = "https://things.digitalm.cloud";
const char* TB_TOKEN  = "52kqr3ax2flcp0gdy56s";
int lastTbHttpCode = 0;
unsigned long tbSendCount = 0;

// WebServer on port 80
WebServer server(80);
Preferences prefWifi;
Preferences prefAuth;

// Authentication Credentials (Default: admin / admin)
String auth_user = "admin";
String auth_pass = "admin";

// Live Readings & States
float liveTempC = 0.0f;
float liveTempF = 0.0f;
float liveHum = 0.0f;
float liveHeatIndex = 0.0f;
bool  liveDhtOk = false;

float liveSoilMoisture = 0.0f;
float liveSoilVoltage = 0.0f;
int   liveSoilADC = 0;

const unsigned long SAMPLE_INTERVAL = 3000UL; // Read & send telemetry every 3s
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

    // 1. Check Address 0x23 (ADDR to GND)
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

    // 2. Check Address 0x5C (ADDR to VCC)
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
    // Simple sort for median
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
    p += "\"m3_temperature\":"    + String(tC, 1);
    p += ",\"m3_temperatureF\":"  + String(tF, 1);
    p += ",\"m3_humidity\":"      + String(hum, 1);
    p += ",\"m3_heatIndex\":"     + String(hi, 1);
    p += ",\"temperature\":"      + String(tC, 1);
    p += ",\"temperatureF\":"     + String(tF, 1);
    p += ",\"humidity\":"         + String(hum, 1);
    p += ",\"heatIndex\":"        + String(hi, 1);
    p += ",\"lux\":"              + String(lux, 1);
    p += ",\"lightLevel\":\""      + String(lightLv) + "\"";
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
//  HTML Web Pages (Login Screen & Monitoring Dashboard)
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
            color: #ffffff;
            border: none;
            border-radius: 8px;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            margin-top: 8px;
            transition: background 0.2s;
        }
        .btn:hover { background: var(--primary-hover); }
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
        <p class="sub">Light, Soil & Environment Monitor</p>

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
                .then(res => {
                    if (res.ok) {
                        window.location.href = '/';
                    } else {
                        err.style.display = 'block';
                        btn.disabled = false;
                        btn.innerText = 'Sign In';
                    }
                })
                .catch(() => {
                    err.innerText = '❌ Connection error. Try again.';
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
    <title>Module 3 Dashboard | Light & Soil Monitor</title>
    <style>
        :root {
            --bg: #0b1120;
            --card-bg: #1e293b;
            --border: #334155;
            --primary: #10b981;
            --primary-glow: rgba(16, 185, 129, 0.2);
            --amber: #f59e0b;
            --blue: #3b82f6;
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
            --danger: #ef4444;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
        body { background: var(--bg); color: var(--text-main); min-height: 100vh; padding: 16px; padding-bottom: 40px; }
        .container { max-width: 900px; margin: 0 auto; }
        
        /* Navbar */
        .navbar {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 12px 18px;
            background: var(--card-bg);
            border: 1px solid var(--border);
            border-radius: 14px;
            margin-bottom: 20px;
        }
        .nav-brand { display: flex; align-items: center; gap: 10px; font-weight: 700; font-size: 1.1rem; }
        .nav-brand span { color: var(--primary); }
        .btn-logout {
            background: #334155;
            color: #f8fafc;
            border: none;
            padding: 6px 14px;
            border-radius: 8px;
            font-size: 0.85rem;
            cursor: pointer;
            transition: background 0.2s;
        }
        .btn-logout:hover { background: var(--danger); }

        /* Status Banner */
        .banner {
            display: flex;
            flex-wrap: wrap;
            justify-content: space-between;
            align-items: center;
            background: rgba(16, 185, 129, 0.1);
            border: 1px solid rgba(16, 185, 129, 0.3);
            border-radius: 12px;
            padding: 10px 16px;
            margin-bottom: 20px;
            font-size: 0.85rem;
            gap: 8px;
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
        .status-optimal { background: rgba(16, 185, 129, 0.2); color: var(--primary); }
        .status-warn { background: rgba(245, 158, 11, 0.2); color: var(--amber); }
        .status-info { background: rgba(59, 130, 246, 0.2); color: var(--blue); }
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
        .fill-emerald { background: var(--primary); }
        .fill-blue { background: var(--blue); }
        .fill-amber { background: var(--amber); }

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
            background: #0f172a;
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
            color: #fff;
            border: none;
            border-radius: 8px;
            font-weight: 600;
            font-size: 0.9rem;
            cursor: pointer;
            white-space: nowrap;
            height: 41px;
        }
        .btn-action:hover { background: #059669; }
        .btn-scan { background: #3b82f6; }
        .btn-scan:hover { background: #2563eb; }
        .wifi-status-text { margin-top: 12px; font-size: 0.82rem; color: var(--text-muted); }
    </style>
</head>
<body>
    <div class="container">
        <!-- Navbar -->
        <div class="navbar">
            <div class="nav-brand">🌿 <span>Module 3</span> Monitoring Hub</div>
            <button class="btn-logout" onclick="window.location.href='/logout'">Sign Out</button>
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
                    <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:8px;">(<span id="valTempF">--.-</span> °F)</div>
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
                <div class="card-status status-info" id="statusHum" style="margin-top:10px;">Ambient Air</div>
            </div>

            <!-- 3. Heat Index -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Heat Index</span>
                    <span class="card-icon">🔥</span>
                </div>
                <div>
                    <div class="card-value"><span id="valHi">--.-</span><span class="card-unit">°C</span></div>
                </div>
                <div class="card-status status-warn" id="statusHi">Calculated Feel</div>
            </div>

            <!-- 4. Light Intensity -->
            <div class="card">
                <div class="card-header">
                    <span class="card-title">Light Intensity</span>
                    <span class="card-icon">☀️</span>
                </div>
                <div>
                    <div class="card-value"><span id="valLux">--.-</span><span class="card-unit">lx</span></div>
                    <div class="progress-bar">
                        <div class="progress-fill fill-amber" id="barLux" style="width: 0%;"></div>
                    </div>
                </div>
                <div style="display:flex; justify-content:space-between; align-items:center; margin-top:10px;">
                    <div class="card-status status-optimal" id="statusLux">--</div>
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="bhPin">BH1750 (Wire1)</span>
                </div>
            </div>

            <!-- 5. Soil Moisture -->
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
                    <span style="font-size:0.75rem; color:var(--text-muted);" id="soilRaw">ADC: ----</span>
                </div>
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
                    <button class="btn-action" style="background:#475569;" onclick="resetWiFi()">🗑️ Reset</button>
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

                    // Temperature
                    document.getElementById('valTemp').innerText = d.temp.toFixed(1);
                    document.getElementById('valTempF').innerText = d.tempF.toFixed(1);

                    // Humidity
                    document.getElementById('valHum').innerText = d.hum.toFixed(1);
                    document.getElementById('barHum').style.width = Math.min(d.hum, 100) + '%';

                    // Heat Index
                    document.getElementById('valHi').innerText = d.hi.toFixed(1);

                    // Light
                    document.getElementById('valLux').innerText = d.lux.toFixed(0);
                    document.getElementById('statusLux').innerText = d.lightStatus;
                    document.getElementById('barLux').style.width = Math.min((d.lux / 1000) * 100, 100) + '%';
                    document.getElementById('bhPin').innerText = d.bhOnline ? 'Wire1 (SDA=' + d.bhSda + ')' : 'OFFLINE';
                    document.getElementById('bhPin').style.color = d.bhOnline ? 'var(--primary)' : 'var(--danger)';

                    // Soil
                    document.getElementById('valSoil').innerText = d.soil.toFixed(1);
                    document.getElementById('statusSoil').innerText = d.soilStatus;
                    document.getElementById('barSoil').style.width = Math.min(d.soil, 100) + '%';
                    document.getElementById('soilRaw').innerText = 'ADC: ' + d.soilAdc + ' (' + d.soilV.toFixed(2) + 'V)';

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
    json += "\"temp\":"           + String(liveTempC, 1) + ",";
    json += "\"tempF\":"          + String(liveTempF, 1) + ",";
    json += "\"hum\":"            + String(liveHum, 1) + ",";
    json += "\"hi\":"             + String(liveHeatIndex, 1) + ",";
    json += "\"lux\":"            + String(currentLux, 1) + ",";
    json += "\"lightStatus\":\""  + String(getLightStatus(currentLux)) + "\",";
    json += "\"bhOnline\":"       + String(bh1750_available ? "true" : "false") + ",";
    json += "\"bhSda\":"          + String(activeBH1750_SDA) + ",";
    json += "\"soil\":"           + String(liveSoilMoisture, 1) + ",";
    json += "\"soilStatus\":\""   + String(getSoilStatus(liveSoilMoisture)) + "\",";
    json += "\"soilAdc\":"        + String(liveSoilADC) + ",";
    json += "\"soilV\":"          + String(liveSoilVoltage, 3) + ",";
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

    printLine('=');
    Serial.println(F("  ESP32-S3 — MODULE 3: LIGHT & SOIL ENVIRONMENT HUB"));
    printLine('=');
    Serial.println(F("  Sensors: DHT11 (GPIO 4) | BH1750 (Wire1: SDA=5/15, SCL=16) | Soil (GPIO 1)"));
    Serial.println(F("  Display: 2004 I2C LCD (Wire: SDA=17, SCL=18)"));
    Serial.println(F("  Utility: WebServer AP (192.168.4.1) + Secure Login Dashboard"));
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

    // 5. Load Stored WiFi and Auth Credentials
    prefWifi.begin("m3-wifi", false);
    wifi_ssid = prefWifi.getString("ssid", "Meeting Room");
    wifi_pass = prefWifi.getString("pass", "Shahid786$$");

    prefAuth.begin("m3-auth", false);
    auth_user = prefAuth.getString("user", "admin");
    auth_pass = prefAuth.getString("pass", "admin");

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

    // 7. Configure Web Server Headers & Routes
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
    }
}

// =====================================================================
//  Loop
// =====================================================================
void loop() {
    // 1. Process Incoming HTTP Clients Immediately
    server.handleClient();

    // 2. Periodic Sensor Sampling & Telemetry Dispatch (Non-blocking)
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
