# 💨 Module 1: Air Quality & Gas Monitoring System

## 📌 Sensor Pinout Table

| Sensor | Sensor Pin | ESP32 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` / `5V` | Ambient Temperature, Humidity, Heat Index |
| | `VCC` | `3.3V` / `5V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **MQ-137** | `AO` (Analog) | **`GPIO 33`** | `5V (VIN)` | `ADC1_CH5` — Ammonia gas concentration (0.0 ppm baseline) |
| | `DO` (Digital) | **`GPIO 25`** | — | Digital threshold alert output |
| | `VCC` | `5V (VIN)` | — | Heater coil requires 5V supply |
| | `GND` | `GND` | — | Common Ground |
| **FS200A** | `Signal` (Yellow) | **`GPIO 27`** | `5V (VIN)` | Hardware Interrupt (`FALLING` edge) — Air/Water flow |
| | `VCC` (Red) | `5V (VIN)` | — | 5V Turbine Power |
| | `GND` (Black) | `GND` | — | Common Ground |
| **CCS811** | `SDA` | **`GPIO 17`** | `3.3V` | `Wire1` I2C Data bus |
| | `SCL` | **`GPIO 16`** | — | `Wire1` I2C Clock bus (50 kHz) |
| | `WAK / WAKE` | **`GND`** | — | ⚠️ **CRITICAL:** Active-LOW wake pin. Must be GND to wake sensor. |
| | `RST` | **`3.3V`** | — | ⚠️ **CRITICAL:** Active-LOW reset pin. Must connect to 3.3V. |
| | `ADD / ADDR` | **`GND`** | — | Sets I2C address to `0x5A` |
| | `VCC` | `3.3V` | — | Power supply |
| | `GND` | `GND` | — | Ground |

---

## ⚙️ Key Technical Features
* **CCS811 Environmental Compensation:** DHT11 temperature and humidity are fed into the CCS811 algorithm (`ccs.setEnvironmentalData`) to calculate accurate eCO2 & TVOC.
* **MQ-137 0.0 ppm Baseline:** Calibrated to read 0.0 ppm in clean air and scale dynamically up to 500 ppm.
* **ThingsBoard Cloud:** Pushes 16 telemetry keys every 3 seconds over HTTPS.
