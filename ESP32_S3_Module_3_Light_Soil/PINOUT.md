# 🌿 ESP32-S3 Module 3: Light, Soil Moisture & Environmental System

## 📌 Sensor Pinout Table (ESP32-S3)

| Sensor | Sensor Pin | ESP32-S3 Pin | Power Voltage | Function / Notes |
|---|---|---|---|---|
| **DHT11** | `DATA` | **`GPIO 4`** | `3.3V` / `5V` | Ambient Temperature (°C/°F) & Humidity (%) |
| | `VCC` | `3.3V` / `5V` | — | Power |
| | `GND` | `GND` | — | Ground |
| **BH1750 Light Sensor** | `SDA` | **`GPIO 15`** | `3.3V` | `Wire1` I2C Data bus |
| | `SCL` | **`GPIO 16`** | — | `Wire1` I2C Clock bus |
| | `ADDR` | **`GND`** | — | Sets I2C address to `0x23` |
| | `VCC` | `3.3V` | — | Power supply |
| | `GND` | `GND` | — | Ground |
| **Capacitive Soil v2.0** | `AOUT` (Signal) | **`GPIO 1`** | `3.3V` | `ADC1_CH0` — Analog moisture level (0%–100%) |
| | `VCC` | `3.3V` | — | Direct 3.3V linearity |
| | `GND` | `GND` | — | Common Ground |
| **2004 I2C LCD Display** | `SDA` | **`GPIO 17`** | `5V (VIN)` | Dedicated I2C Bus (`Wire`: Address `0x27` / `0x3F`) |
| (20x4 Character Screen) | `SCL` | **`GPIO 18`** | — | Dedicated I2C Bus (`Wire`) |
| | `VCC` | `5V (VIN)` | — | Power (5V required for crisp contrast & backlight) |
| | `GND` | `GND` | — | Common Ground |
| **4-Channel 5V Relay** | `IN1` | **`GPIO 7`** | — | Fan Control Trigger (Active-LOW: `LOW`=ON, `HIGH`=OFF) |
| (Optocoupler Isolated) | `VCC` | **`5V (VIN)`** | `5V` | Relay coil & optocoupler logic power |
| | `GND` | **`GND`** | — | Common Ground with ESP32-S3 |

---

## ⚡ Cooling Fan & Relay Output Wiring (Screw Terminals)

Connect Relay Channel 1 screw terminals as an inline switch for the fan:
```
[External Fan Power (+)] --------> [ Relay Channel 1: COM (Common) ]
                                   [ Relay Channel 1: NO (Normally Open) ] ------> [ Fan Red Wire (+) ]

[External Fan Power (-)] --------------------------------------------------------> [ Fan Black Wire (-) ]
```

* **When Temp $\ge 30^\circ\text{C}$:** ESP32 pulls GPIO 7 `LOW` $\rightarrow$ Relay clicks and connects `COM` to `NO` $\rightarrow$ **Fan turns ON**.
* **When Temp $< 27^\circ\text{C}$:** ESP32 pulls GPIO 7 `HIGH` $\rightarrow$ Relay opens $\rightarrow$ **Fan turns OFF**.
* **Hysteresis Band (27°C – 30°C):** Prevents rapid on/off cycling around the threshold.

---

## ⚙️ Key Technical Features
* **Active-LOW Optocoupler Isolation:** Prevents inductive EMF spikes from the fan motor from resetting the ESP32-S3.
* **Master Light Calibration:** Calibrated with a $0.7308\times$ multiplier to match master reference instrument (200–205 lx range in office lighting).
* **Capacitive Corrosion-Free Probe:** Insulated PCB electrodes that do not corrode in soil.
* **ThingsBoard Cloud:** Pushes 12 telemetry keys every 3 seconds over HTTPS (including `fan_status` and `relay_fan`).
