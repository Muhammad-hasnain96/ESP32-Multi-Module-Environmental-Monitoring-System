# 🌊 Module 2: Water Quality & Flow Monitoring System

## 📌 Sensor Pinout Table

| Sensor | Sensor Pin | ESP32 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` / `5V` | Ambient Temperature & Humidity |
| | `VCC` | `3.3V` / `5V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **Analog TDS Meter** | `AOUT` (Signal) | **`GPIO 34`** | `3.3V` | `ADC1_CH6` — Analog TDS measurement (Clean input pin) |
| | `VCC` | `3.3V` / `5V` | — | Power supply |
| | `GND` | `GND` | — | Common Ground |
| **Water Flow Sensor** | `Signal` (Yellow) | **`GPIO 27`** | `5V (VIN)` | Hardware Interrupt (`FALLING` edge) — Flow rate & Liters |
| (FS200A / YF-S201) | `VCC` (Red) | `5V (VIN)` | — | 5V Turbine Power |
| | `GND` (Black) | `GND` | — | Common Ground |
| **E-201-C BNC pH Sensor** | `Po` (Analog Signal) | **`GPIO 35`** | `5V (VIN)` | `ADC1_CH7` — 2-Point Calibrated pH (Water 1.16V / Vinegar 1.995V) |
| (pH-4502C Module) | `VCC` | `5V (VIN)` | — | Power (Requires 5V for op-amp linear headroom) |
| | `GND` (Power) | `GND` | — | Common Ground |
| | `GND` (Analog) | `GND` | — | Analog probe ground reference |
| **2004 I2C LCD** | `SDA` | **`GPIO 19`** | `5V (VIN)` | `Wire` Dedicated I2C Data bus |
| | `SCL` | **`GPIO 18`** | — | `Wire` Dedicated I2C Clock bus (100 kHz) |
| | `VCC` | `5V (VIN)` | — | ⚠️ **CRITICAL:** HW-61 backpack / LCD contrast needs 5V |
| | `GND` | `GND` | — | Ground |

---

## ⚙️ Key Technical Features
* **2004 I2C LCD Live Display:**
  * Line 0: Ambient Temp & Humidity (`T: 27.2C   H: 54.6%`)
  * Line 1: Water pH & Status (`pH: 6.20  [ACIDIC]`)
  * Line 2: TDS Value & Quality (`TDS: 280ppm [GOOD]`)
  * Line 3: Flow Rate & Total Volume (`Fl: 0.0L/m Tot:0.00L`)
* **Median Noise Filter:** 30 samples taken per reading to eliminate electrical jitter in water.
* **Temperature Compensation:** Ambient DHT11 temperature calibrates water electrical conductivity ($T_{coeff} = 1.0 + 0.02 \times (T - 25.0)$).
* **Dry Probe Protection:** Automatically zeroes out to 0.0 ppm when probe is out of water.
* **Flow Calculations:** Flow rate in L/min, mL/sec, and cumulative liters ($Q = \text{Freq} / 7.5$).
* **ThingsBoard Cloud:** Pushes all dedicated telemetry parameters (`m2_temperature`, `m2_humidity`, `tdsPPM`, `phValue`, `flowRateLMin`, etc.) every 3 seconds over HTTPS.
