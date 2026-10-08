# 🌿 ESP32-S3 Module 3: Light, Soil Moisture & Environmental System

## 📌 Sensor Pinout Table (ESP32-S3)

| Sensor | Sensor Pin | ESP32-S3 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` / `5V` | Ambient Temperature (°C/°F) & Humidity (%) |
| | `VCC` | `3.3V` / `5V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **BH1750 Light Sensor** | `SDA` | **`GPIO 5`** *(fallback: `GPIO 15`)* | `3.3V` / `5V` | `Wire1` I2C Data bus |
| | `SCL` | **`GPIO 16`** | — | `Wire1` I2C Clock bus |
| | `ADDR` | **`GND`** | — | Sets I2C address to `0x23` |
| | `VCC` | `3.3V` / `5V` | — | Power supply |
| | `GND` | `GND` | — | Ground |
| **Capacitive Soil v2.0** | `AOUT` (Signal) | **`GPIO 1`** | `3.3V` | `ADC1_CH0` — Analog moisture level (0%–100%) |
| | `VCC` | `3.3V` | — | Direct 3.3V linearity |
| | `GND` | `GND` | — | Common Ground |
| **2004 I2C LCD Display** | `SDA` | **`GPIO 17`** | `5V (VIN)` | Dedicated I2C Bus (`Wire`: Address `0x27` / `0x3F`) |
| (20x4 Character Screen) | `SCL` | **`GPIO 18`** | — | Dedicated I2C Bus (`Wire`) |
| | `VCC` | `5V (VIN)` | — | Power (5V required for crisp contrast & backlight) |
| | `GND` | `GND` | — | Common Ground |

---

## ⚙️ Key Technical Features
* **Smart I2C Bus Initialization:** Auto-detects BH1750 on `SDA = GPIO 5, SCL = GPIO 16` (and falls back to `GPIO 15` if needed) at addresses `0x23` and `0x5C`. Includes periodic background re-scan if reconnected.
* **Direct Real Light Sensor Output (Raw Lux):** Native factory-calibrated illuminance values directly from the BH1750 sensor.
* **Capacitive Corrosion-Free Probe:** Insulated PCB electrodes that do not corrode in soil.
* **ThingsBoard Cloud:** Pushes real-time telemetry keys (`temperature`, `humidity`, `soil_moisture`, `light_lux`, `light_state`) over HTTPS.
