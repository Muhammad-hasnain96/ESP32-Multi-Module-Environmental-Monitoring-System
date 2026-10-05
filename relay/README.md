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

### 🌡️ 3x DHT11 Temperature & Humidity Sensors
| Sensor | Data Pin | ESP32-S3 Pin | Notes |
|---|---|---|---|
| **DHT11 #1 (T1)** | `DATA` | **`GPIO 8`** | Primary Sensor |
| **DHT11 #2 (T2)** | `DATA` | **`GPIO 3`** | Multi-Zone Sensor 2 |
| **DHT11 #3 (T3)** | `DATA` | **`GPIO 42`** | Multi-Zone Sensor 3 |
| **Power** | `VCC` / `GND` | **`3.3V / 5V` & `GND`** | Common Ground & Clean Power |

> [!NOTE]
> Relay ON/OFF decisions are calculated dynamically based on the **Average (Mean)** of all active connected sensors. If any sensor is unplugged, the system smoothly falls back to the remaining active sensors.

### 🌊 Water Flow Sensor (Hall Effect Pulse - Display Only)
| Sensor Wire | ESP32-S3 Pin | Notes |
|---|---|---|
| **Signal (Yellow)** | **`GPIO 46`** | Hardware interrupt pulse counter |
| **VCC (Red)** | **`5V` (VIN)** | 5V power supply |
| **GND (Black)** | **`GND`** | Common Ground |

> [!IMPORTANT]
> The Water Flow Sensor output is **for real-time monitoring and display only**. Relays are completely unaffected by water flow rate and are solely governed by Temperature and Humidity thresholds.

### 🔘 4 Physical Navigation Buttons (Internal Pullup)
| Button | ESP32-S3 Pin | Function |
|---|---|---|
| **Menu / Enter** | **`GPIO 38`** | Enter menu / select / save |
| **Up** | **`GPIO 39`** | Navigate up / increment threshold (+0.5°C) |
| **Down** | **`GPIO 40`** | Navigate down / decrement threshold (-0.5°C) |
| **Back / Exit** | **`GPIO 41`** | Return to previous screen / home |

---

## ⚡ Automation Logic: Outside Range ON (Based on Average)

The relays follow the dual-threshold window logic evaluated against the **Average Temperature / Humidity**:
* **Relay turns ON** when $\text{Average} \ge \text{Max Threshold}$ *(Too Hot / Cooling)*
* **Relay turns ON** when $\text{Average} \le \text{Min Threshold}$ *(Too Cold / Heating)*
* **Relay automatically turns OFF** when $\text{Min Threshold} < \text{Average} < \text{Max Threshold}$ *(Normal / Safe Zone)*

---

## 📱 Mobile Web Dashboard (`http://192.168.4.1`)
Connect your smartphone to the ESP32 Access Point:
* **SSID:** `ESP32-16Relay-Hub`
* **Password:** `12345678`
* **IP Address:** `192.168.4.1`

### Dashboard Features:
1. **Live Sensor Cards:**
   * **Avg Temperature:** Live reading with individual T1, T2, T3 breakdown.
   * **Avg Humidity:** Live reading with individual H1, H2, H3 breakdown.
   * **Water Flow (GPIO 46):** Live Flow Rate (L/min), Total Liters, and Pulse counter.
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

### 🏠 Professional Home Screen (Auto-Cycles every 5 seconds)
To provide a spacious, clean display without crowding, the idle home screen rotates automatically between **Temperature**, **Humidity**, and **Water Flow** every 5 seconds:

#### Page 1: Temperature Overview (T1, T2, T3 & Live Average)
```text
 Temp 1 :   28.4 °C 
 Temp 2 :   27.8 °C 
 Temp 3 :   28.1 °C 
>AVG TEMP:  28.1 °C 
```

#### Page 2: Humidity Overview (H1, H2, H3 & Live Average)
```text
 Humid 1:   58.2 %  
 Humid 2:   60.1 %  
 Humid 3:   59.0 %  
>AVG HUM :  59.1 %  
```

#### Page 3: Water Flow Overview (Display Only)
```text
=== WATER SENSOR ===
 Rate :    2.5 L/min
 Total:   14.8 Liter
 Pulse:   5580      
```
Pressing **`[MENU]`**, **`[UP]`**, or **`[DOWN]`** at any time instantly opens the **Main Menu**.

---

### 📋 Main Menu (Clean & Classic 3 Options)
```text
==== MAIN MENU =====
> 1. Temperature    
  2. Humidity       
  3. Relays (1-16)  
```
* **`1. Temperature` (Global All-Relay Setup):**
  Displays live readings and allows editing Min and Max temperature thresholds applied across all 16 relays in `Auto Temperature` mode.
* **`2. Humidity` (Global All-Relay Setup):**
  Displays live readings and allows editing Min and Max humidity thresholds applied across all 16 relays in `Auto Humidity` mode.
* **`3. Relays (1-16)` (Individual Relay Customization):**
  Scroll through Relays 01 to 16 with `UP`/`DOWN`. Press `[MENU]` to customize that relay:
  - Toggle Mode: `Auto Temp` $\longleftrightarrow$ `Auto Hum`
  - Edit Min threshold (with auto-repeat on button hold)
  - Edit Max threshold (with auto-repeat on button hold)
  - Press `[MENU]` to **Save** and trigger immediately, or `[BACK]` to **Cancel**.

