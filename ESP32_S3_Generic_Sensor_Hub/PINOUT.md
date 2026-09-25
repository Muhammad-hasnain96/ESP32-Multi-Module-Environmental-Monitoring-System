# ESP32-S3 Generic Smart Sensor Hub — Universal Plug & Play Guide

## Overview
The **ESP32-S3 Generic Smart Sensor Hub** features **true plug-and-play dynamic auto-detection**. You do not need to hardcode pins or re-flash the board when moving sensors or swapping between DHT11 and Capacitive Soil Moisture sensors.

---

## Dedicated Pin Allocations

### 1. 2004 Character LCD (Dedicated Primary I2C — `Wire`)
* **SDA**: `GPIO 17`
* **SCL**: `GPIO 18`
* **VCC**: `5V` (VIN)
* **GND**: `GND`
* **I2C Address**: Auto-scans `0x27` or `0x3F`

### 2. Secondary I2C Sensors (`Wire1` — BH1750 Light & CCS811 Air Quality)
* **Default Wiring**: `SDA -> GPIO 15`, `SCL -> GPIO 16`
* **Auto-Fallback**: Supports reverse `SDA -> GPIO 16`, `SCL -> GPIO 15`
* **Addresses**:
  * BH1750: `0x23` (or `0x5C`)
  * CCS811: `0x5A`

---

## Universal Auto-Sensing Pins (Digital + ADC1 Analog)

Any of the following pins can be used for **EITHER** a **DHT11** OR a **Capacitive Soil Moisture Sensor**:

> **Universal Candidate Pins**: `GPIO 4, 5, 6, 7, 1, 2, 8, 9, 10`

### How the Hub Distinguishes Sensors Automatically:
1. **DHT11 Plugged In**:
   - The Hub initiates an 18ms digital handshake pulse.
   - DHT11 responds with 80µs LOW / 80µs HIGH.
   - Hub immediately locks onto it: `[+] DHT11 DETECTED & INITIALIZED on GPIO X!`
2. **Soil Moisture Sensor Plugged In (Even on the same pin!)**:
   - Soil sensor outputs an active analog DC voltage (`~1.0V - 2.8V`, ADC `800 - 3500`).
   - It fails digital DHT handshake, but resists internal weak pull-down testing.
   - Hub immediately locks onto it: `[+] CAPACITIVE SOIL MOISTURE SENSOR DETECTED on GPIO X!`
3. **Hot-Unplug Protection**:
   - If you unplug DHT11 or Soil Moisture sensor, within 2 cycles the Hub marks it `[UNPLUGGED]` and starts scanning candidate pins again.
   - You can hot-swap DHT11 $\leftrightarrow$ Soil Moisture on the exact same header without rebooting!

---

## Capacitive Soil Moisture Sensor Wiring

| Soil Sensor Pin | ESP32-S3 Pin | Function |
|---|---|---|
| **VCC** | `3.3V` (or `5V`) | Power |
| **GND** | `GND` | Ground |
| **AOUT / Signal** | Any Universal Pin (`GPIO 6, 5, 1, 2, 7, etc.`) | Analog Output |

---

## Cloud & Display
* **ThingsBoard Cloud**: `thingsboard.cloud` | Token: `2HGvWTV145aFOdJbjAwQ`
* **Telemetry Keys**:
  * `temperature`, `humidity`, `heatIndex`, `dht11_online`, `dht11_pin`
  * `soil_moisture`, `soil_raw`, `soil_voltage`, `soil_online`, `soil_pin`
  * `light_lux`, `bh1750_online`
  * `co2_ppm`, `tvoc_ppb`, `ccs811_online`
  * `wifi_rssi`, `uptime_sec`
