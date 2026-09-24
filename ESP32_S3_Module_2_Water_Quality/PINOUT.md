# 🌊 ESP32-S3 Module 2: Water Quality, pH & Flow Monitoring System

## 📌 Sensor Pinout Table (ESP32-S3)

| Sensor | Sensor Pin | ESP32-S3 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` | Ambient Temperature & Humidity |
| | `VCC` | `3.3V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **Analog TDS Meter** | `AOUT` (Signal) | **`GPIO 1`** | `3.3V` | `ADC1_CH0` — Analog TDS measurement (0–4095 ADC) |
| | `VCC` | `3.3V` | — | Power supply |
| | `GND` | `GND` | — | Common Ground |
| **Water Flow Sensor** | `Signal` (Yellow) | **`GPIO 5`** | `3.3V` / `5V (VIN)` | Hardware Interrupt (`FALLING` edge) — Flow rate & Liters |
| (FS200A / YF-S201) | `VCC` (Red) | `3.3V` / `5V (VIN)` | — | Turbine Power |
| | `GND` (Black) | `GND` | — | Common Ground |
| **E-201-C BNC pH Sensor** | `Po` (Analog Signal) | **`GPIO 2`** | `5V (VIN)` | `ADC1_CH1` — 2-Point Calibrated pH (Water 1.16V / Vinegar 1.995V) |
| (pH-4502C Module) | `VCC` | `5V (VIN)` | — | Power (Requires 5V for op-amp linear headroom) |
| | `GND` (Power) | `GND` | — | Common Ground |
| | `GND` (Analog) | `GND` | — | Analog probe ground reference |
| **2004 I2C LCD Display** | `SDA` | **`GPIO 17`** | `5V (VIN)` | `Wire` Dedicated I2C Data bus |
| (20x4 Character Screen) | `SCL` | **`GPIO 18`** | — | `Wire` Dedicated I2C Clock bus (50 kHz) |
| | `VCC` | `5V (VIN)` | — | ⚠️ **CRITICAL:** HW-61 backpack / LCD contrast needs 5V |
| | `GND` | `GND` | — | Ground |

---

> [!WARNING]
> **GPIO 35 is NOT an ADC pin on the ESP32-S3!**
> On classic ESP32, GPIO 35 was an analog input. However, on the ESP32-S3, GPIO 33–37 are reserved for high-speed Octal Flash / PSRAM. Connecting an analog sensor to GPIO 35 on ESP32-S3 will cause crashes or read nothing.
> **The pH meter must be connected to `GPIO 2` (`ADC1_CH1`).**

---

## ⚙️ Key Technical Features
* **2004 I2C LCD Live Display (Rotating 2-Page System):**
  * **Screen 1 (Ambient & pH):**
    * Line 0: `-- ENV & pH  [1/2] -`
    * Line 1: `Temp    :  25.0 C   `
    * Line 2: `Humidity:  50.0 %   `
    * Line 3: `pH : 7.00 [NEUTRAL] `
  * **Screen 2 (TDS & Water Flow):**
    * Line 0: `-- TDS & FLOW [2/2]-`
    * Line 1: `TDS :  120ppm [EXCL]`
    * Line 2: `Flow :   0.0 L/min  `
    * Line 3: `Total:   0.00 Liters`
* **Median Noise Filter:** 30 samples taken per reading to eliminate electrical jitter in water.
* **Temperature Compensation:** Ambient DHT11 temperature calibrates water electrical conductivity ($T_{coeff} = 1.0 + 0.02 \times (T - 25.0)$).
* **Dry Probe Protection:** Automatically zeroes out to 0.0 ppm when probe is out of water.
* **Flow Calculations:** Flow rate in L/min, mL/sec, and cumulative liters ($Q = \text{Freq} / 7.5$).
* **ThingsBoard Cloud:** Pushes all dedicated telemetry parameters (`m2_temperature`, `m2_humidity`, `tdsPPM`, `phValue`, `flowRateLMin`, etc.) every 5 seconds over HTTPS.
