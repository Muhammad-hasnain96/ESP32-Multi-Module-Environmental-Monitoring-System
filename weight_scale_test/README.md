# ⚖️ ESP32 5000g / 5kg Weight Scale Test Guide

## 📌 1. Hardware Overview
* **Capacity:** 5000g × 1g / 177oz × 0.1oz (5kg max)
* **Amplifier / ADC:** HX711 24-bit Weighing Sensor Module
* **Microcontroller:** Classic ESP32 DevKit (connected on **`COM10`**)

---

## 🔌 2. Complete Wiring Diagram

### A. Load Cell (4 Wires) $\rightarrow$ HX711 Module (Input Side)
| Load Cell Wire | HX711 Terminal Pin | Function |
|---|---|---|
| 🔴 **Red** | **`E+`** | Excitation Voltage Positive |
| ⚫ **Black** | **`E-`** | Excitation Ground Negative |
| ⚪ **White** | **`A-`** | Channel A Differential Signal Negative |
| 🟡 **Yellow** | **`A+`** | Channel A Differential Signal Positive |

> [!NOTE]
> Standard 5kg kitchen scale load cells use Yellow for Signal+ (instead of green). If readings increase in the negative direction when weight is placed, simply swap **White (`A-`)** and **Yellow (`A+`)**.

---

### B. HX711 Module (Output Side) $\rightarrow$ Classic ESP32
| HX711 Pin | Classic ESP32 Pin | Voltage / Type | Notes |
|---|---|---|---|
| **`VCC`** | **`5V` (VIN / V5)** or **`3.3V`** | Power | Recommended **5V** for lowest noise and higher excitation signal |
| **`GND`** | **`GND`** | Ground | Common Ground |
| **`DT`** (DOUT) | **`GPIO 21`** | Input | 24-bit Serial Data Output |
| **`SCK`** (CLK) | **`GPIO 22`** | Output | Serial Clock Input |

---

## 🛠️ 3. Physical Mounting Rules (Very Important for Load Cells!)
A load cell measures deflection (bending strain). If it is lying flat on a table, it will **NOT** measure weight properly!
1. **Direction Arrow:** Look for the arrow sticker on the metal aluminum bar of the load cell. It points in the direction of the applied force (downwards).
2. **Fixed End vs Floating End:**
   * One end (two screw holes) must be bolted firmly to a **base plate**.
   * The other end (two screw holes) must be bolted to the **weighing platform / plate**, suspended in the air with clearance so it can bend slightly when pressed.

---

## 🚀 4. How to Test & Calibrate (Step-by-Step)

### Step 1: Upload Firmware
In VS Code PlatformIO terminal:
```powershell
pio run -d "weight_scale_test" -t upload
```

### Step 2: Open Serial Monitor
```powershell
pio device monitor -p COM10 -b 115200
```

### Step 3: Zero / Tare the Scale
* Make sure no object is on the platform.
* Type **`t`** in the Serial Monitor and press Enter to Tare (zero) the scale.
* The reading should show `0.0 g`.

### Step 4: Place a Known Weight & Calibrate
* Place an object with known weight on the scale (e.g., a full 500 mL water bottle = approx **500.0g**, or a smartphone = e.g., **185g**).
* If the reading is higher than actual, increase the calibration factor:
  * Send **`+`** (adds 10) or **`]`** (adds 100).
* If the reading is lower than actual, decrease the calibration factor:
  * Send **`-`** (subtracts 10) or **`[`** (subtracts 100).
* Once the displayed weight matches your known weight, note down the `Calib Factor` shown on the screen.
* Put that number into `calibration_factor = YOUR_NUMBER;` in `src/main.cpp`!
