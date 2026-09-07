# 🌿 Module 3: Light, Soil Moisture & Environmental System

## 📌 Sensor Pinout Table

| Sensor | Sensor Pin | ESP32 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` / `5V` | Ambient Temperature (°C/°F) & Humidity (%) |
| | `VCC` | `3.3V` / `5V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **BH1750 Light Sensor** | `SDA` | **`GPIO 21`** | `3.3V` | I2C Data bus |
| | `SCL` | **`GPIO 22`** | — | I2C Clock bus |
| | `ADDR` | **`GND`** | — | Sets I2C address to `0x23` |
| | `VCC` | `3.3V` | — | Power supply |
| | `GND` | `GND` | — | Ground |
| **Capacitive Soil v2.0** | `AOUT` (Signal) | **`GPIO 34`** | `3.3V` | `ADC1_CH6` — Analog moisture level (0%–100%) |
| | `VCC` | `3.3V` | — | Direct 3.3V linearity |
| | `GND` | `GND` | — | Common Ground |

---

## ⚙️ Key Technical Features
* **Master Light Calibration:** Calibrated with a $0.7308\times$ multiplier to match master reference instrument (200–205 lx range in office lighting).
* **Capacitive Corrosion-Free Probe:** Insulated PCB electrodes that do not corrode in soil.
* **ThingsBoard Cloud:** Pushes all 10 telemetry keys every 3 seconds over HTTPS.
