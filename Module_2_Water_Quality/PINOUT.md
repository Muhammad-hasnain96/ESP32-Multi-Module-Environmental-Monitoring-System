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

---

## ⚙️ Key Technical Features
* **Median Noise Filter:** 30 samples taken per reading to eliminate electrical jitter in water.
* **Temperature Compensation:** Ambient DHT11 temperature calibrates water electrical conductivity ($T_{coeff} = 1.0 + 0.02 \times (T - 25.0)$).
* **Dry Probe Protection:** Automatically zeroes out to 0.0 ppm when probe is out of water.
* **Flow Calculations:** Flow rate in L/min, mL/sec, and cumulative liters ($Q = \text{Freq} / 7.5$).
* **ThingsBoard Cloud:** Pushes all 12 telemetry parameters every 3 seconds over HTTPS.
