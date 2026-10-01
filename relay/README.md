# ⚡ ESP32-S3 16-Channel Smart Climate & Relay Hub

A comprehensive, production-grade 16-channel automation hub powered by the **ESP32-S3 DevKitC-1**, featuring a **2004 I2C LCD**, **DHT11 Sensor**, **4 Physical Navigation Buttons**, and an interactive **Mobile Web Dashboard** (`192.168.4.1`).

---

## 📌 1. Hardware Pinout Table

### 🔌 Relay Modules Control (Active-LOW: `LOW` = ON, `HIGH` = OFF)
| Relay Channel | Module | Board Header Pin | ESP32-S3 Pin | Notes |
|---|---|---|---|---|
| **Relay 01** | Module 1 (8-Ch) | `IN1` | **`GPIO 4`** | Header 1 |
| **Relay 02** | Module 1 (8-Ch) | `IN2` | **`GPIO 5`** | Header 1 |
| **Relay 03** | Module 1 (8-Ch) | `IN3` | **`GPIO 6`** | Header 1 |
| **Relay 04** | Module 1 (8-Ch) | `IN4` | **`GPIO 7`** | Header 1 |
| **Relay 05** | Module 1 (8-Ch) | `IN5` | **`GPIO 15`** | Header 1 |
| **Relay 06** | Module 1 (8-Ch) | `IN6` | **`GPIO 16`** | Header 1 |
| **Relay 07** | Module 1 (8-Ch) | `IN7` | **`GPIO 21`** | Header 2 (Safe Non-PSRAM) |
| **Relay 08** | Module 1 (8-Ch) | `IN8` | **`GPIO 47`** | Header 2 (Output Capable) |
| **Relay 09** | Module 2 (4-Ch) | `IN1` | **`GPIO 1`** | Header 1 |
| **Relay 10** | Module 2 (4-Ch) | `IN2` | **`GPIO 2`** | Header 1 |
| **Relay 11** | Module 2 (4-Ch) | `IN3` | **`GPIO 9`** | Header 1 |
| **Relay 12** | Module 2 (4-Ch) | `IN4` | **`GPIO 10`** | Header 1 |
| **Relay 13** | Module 3 (4-Ch) | `IN1` | **`GPIO 11`** | Header 1 |
| **Relay 14** | Module 3 (4-Ch) | `IN2` | **`GPIO 12`** | Header 1 |
| **Relay 15** | Module 3 (4-Ch) | `IN3` | **`GPIO 13`** | Header 1 |
| **Relay 16** | Module 3 (4-Ch) | `IN4` | **`GPIO 14`** | Header 1 |

### 📟 2004 I2C LCD Display (Address `0x27` / `0x3F`)
| LCD Pin | ESP32-S3 Pin | Notes |
|---|---|---|
| **SDA** | **`GPIO 17`** | Direct connection with internal pullup |
| **SCL** | **`GPIO 18`** | Direct connection with internal pullup |
| **VCC** | **`5V` (VIN)** | 5V rail for crisp character contrast |
| **GND** | **`GND`** | Common Ground |

### 🌡️ DHT11 Temperature & Humidity Sensor
| DHT11 Pin | ESP32-S3 Pin | Notes |
|---|---|---|
| **DATA** | **`GPIO 8`** | Single-wire digital data pin |
| **VCC** | **`3.3V` / `5V`** | Clean power |
| **GND** | **`GND`** | Common Ground |

### 🔘 4 Physical Navigation Buttons (Internal Pullup)
| Button | ESP32-S3 Pin | Function |
|---|---|---|
| **Menu / Enter** | **`GPIO 38`** | Enter menu / select / save |
| **Up** | **`GPIO 39`** | Navigate up / increment threshold (+0.5°C) |
| **Down** | **`GPIO 40`** | Navigate down / decrement threshold (-0.5°C) |
| **Back / Exit** | **`GPIO 41`** | Return to previous screen / home |

---

## ⚡ Automation Logic: Outside Range ON

The relays follow the dual-threshold window logic:
* **Relay turns ON** when $\text{Temperature} \ge \text{Max Threshold}$ *(Too Hot / Cooling)*
* **Relay turns ON** when $\text{Temperature} \le \text{Min Threshold}$ *(Too Cold / Heating)*
* **Relay automatically turns OFF** when $\text{Min Threshold} < \text{Temperature} < \text{Max Threshold}$ *(Normal / Safe Zone)*

---

## 📱 Mobile Web Dashboard (`http://192.168.4.1`)
Connect your smartphone to the ESP32 Access Point:
* **SSID:** `ESP32-16Relay-Hub`
* **Password:** `12345678`
* **IP Address:** `192.168.4.1`

### Dashboard Features:
1. **Live Sensor Cards:** Displays real-time Temperature (°C) and Humidity (%).
2. **Individual Relay Control:**
   * Live status badge (`ON` / `OFF`) with reason text.
   * Threshold configuration inputs for Min and Max values.
   * `🎯 Set ON` preset: Auto-calculates thresholds to turn the relay ON immediately.
   * `⭕ Set OFF` preset: Auto-calculates thresholds to turn the relay OFF immediately.
   * `💾 Save`: Saves settings to NVS flash (`Preferences`) and applies instantly.
3. **Master Controls:**
   * `🤖 ALL AUTO`: Resets all 16 relays to automatic climate control.
   * `⚡ ALL ON`: Manual master switch ON.
   * `⬛ ALL OFF`: Manual master switch OFF.
4. **Fast Batch Setup:** Set Min and Max range across all 16 relays simultaneously with 1 tap.
5. **WiFi Router Manager:** Scan for local Wi-Fi networks and connect the hub to your home router.

---

## 📟 4. 2004 LCD Menu Navigation Guide

### 🏠 Professional Home Screen
Displays live readings in a clean, uncluttered format:
```text
====================
 TEMP :    32.4 °C 
 HUMID:    58.2 %  
[MENU] Main Settings
```
Pressing `[MENU]` (or `[UP]`/`[DOWN]`) enters the **Main Menu**.

### 📋 Main Menu (3 Top-Level Options)
```text
==== MAIN MENU =====
> 1. Temperature    
  2. Humidity       
  3. Relays (1-16)  
```
* **`1. Temperature` (Global All-Relay Setup):**
  Sets Min and Max temperature thresholds across all 16 relays simultaneously in `Auto Temperature` mode.
* **`2. Humidity` (Global All-Relay Setup):**
  Sets Min and Max humidity thresholds across all 16 relays simultaneously in `Auto Humidity` mode.
* **`3. Relays (1-16)` (Individual Relay Customization):**
  Scroll through Relays 01 to 16 with `UP`/`DOWN`. Press `[MENU]` to customize that relay:
  - Toggle Mode: `Auto Temp` $\longleftrightarrow$ `Auto Hum`
  - Edit Min threshold (with auto-repeat on button hold)
  - Edit Max threshold (with auto-repeat on button hold)
  - Press `[MENU]` to **Save** and trigger immediately, or `[BACK]` to **Cancel**.

