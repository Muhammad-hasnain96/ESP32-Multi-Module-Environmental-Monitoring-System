# 💨 ESP32-S3 Module 1: Air Quality & Gas Monitoring System

## 📌 Sensor Pinout Table (ESP32-S3 DevKitC-1)

| Sensor | Sensor Pin | ESP32-S3 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` | Ambient Temperature, Humidity, Heat Index |
| | `VCC` | `3.3V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **MQ-137** | `AO` (Analog) | **`GPIO 1`** | `5V (VIN)` | `ADC1_CH0` — Ammonia gas concentration (0.0 ppm baseline) |
| | `DO` (Digital) | **`GPIO 2`** | — | Digital threshold alert output |
| | `VCC` | `5V (VIN)` | — | Heater coil requires 5V supply |
| | `GND` | `GND` | — | Common Ground |
| **FS200A** | `Signal` (Yellow) | **`GPIO 5`** | `5V (VIN)` | Hardware Interrupt (`FALLING` edge) — Air/Water flow |
| | `VCC` (Red) | `5V (VIN)` | — | 5V Turbine Power |
| | `GND` (Black) | `GND` | — | Common Ground |
| **CCS811 (CO2)** | `SDA` | **`GPIO 17`** | `3.3V` | `Wire1` I2C Data bus |
| | `SCL` | **`GPIO 16`** | — | `Wire1` I2C Clock bus (50 kHz) |
| | `WAK / WAKE` | **`GND`** | — | ⚠️ **CRITICAL:** Active-LOW wake pin. Must connect to GND. |
| | `RST` | **`3.3V`** | — | ⚠️ **CRITICAL:** Active-LOW reset pin. Must connect to 3.3V. |
| | `ADD / ADDR` | **`GND`** | — | Sets I2C address to `0x5A` |
| | `VCC` | `3.3V` | — | 3.3V Power supply |
| **2004 I2C LCD** | `SDA` | **`GPIO 15`** | `5V (VIN)` | `Wire` Dedicated I2C Data bus |
| | `SCL` | **`GPIO 18`** | — | `Wire` Dedicated I2C Clock bus (100 kHz) |
| | `VCC` | `5V (VIN)` | — | ⚠️ **CRITICAL:** HW-61 backpack / LCD contrast needs 5V |
| | `GND` | `GND` | — | Ground |

---

## ⚙️ Key Technical Features
* **Dual Independent I2C Buses (No USB Pin Conflicts!):**
  * `Wire` (`GPIO 15` SDA, `GPIO 18` SCL) -> Dedicated for 2004 Character LCD (addresses `0x27` / `0x3F`).
  * `Wire1` (`GPIO 17` SDA, `GPIO 16` SCL) -> Dedicated for CCS811 Air Quality sensor (50 kHz clock stretching).
  * Avoids `GPIO 19` and `GPIO 20` (reserved for ESP32-S3 native USB D- / D+).
* **CCS811 Environmental Compensation:** DHT11 temperature and humidity are fed into the CCS811 algorithm (`ccs.setEnvironmentalData`) to calculate accurate eCO2 & TVOC.
* **MQ-137 0.0 ppm Baseline:** Calibrated to read 0.0 ppm in clean air and scale dynamically up to 500 ppm on ADC1 (GPIO 1).
* **ThingsBoard Cloud:** Pushes 16 telemetry keys every 3 seconds over HTTPS.
* **2004 LCD Live 4-Line Display:**
  * Line 0: Temperature & Humidity (`T: 24.8C   H: 28.5%`)
  * Line 1: eCO2 & Air Quality Status (`CO2: 430ppm [EXCELL]`)
  * Line 2: TVOC & Ammonia NH3 (`TVOC: 28   NH3: 0.0p`)
  * Line 3: Flow Rate & System Uptime (`Flow: 0.0Hz Up:00:04`)
