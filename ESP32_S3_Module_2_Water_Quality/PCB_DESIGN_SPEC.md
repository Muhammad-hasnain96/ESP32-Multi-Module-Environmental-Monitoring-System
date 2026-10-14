# 📐 ESP32-S3 Module 2 (Water Quality Hub) — PCB Hardware Design Specification

**Document Version:** 1.0  
**Project:** Smart Hydroponics / Aquaculture IoT System  
**Target MCU:** Espressif ESP32-S3 (DevKitC-1 or ESP32-S3-WROOM-1 Module)  
**Application:** Water Quality, pH, TDS, Flow Rate & Environmental Telemetry  

---

## 1. System Block Diagram & Power Architecture

```
                  [ 12V / 9V DC In ] (Optional DC Barrel / 2-Pin Terminal)
                           |
                     [ Buck / LDO ]
                           v
 +-------------------------------------------------------------------------+
 |                             +5V0 POWER BUS                              |
 +-------------------------------------------------------------------------+
     |                    |                      |                    |
 [ESP32 5V/VIN]   [pH-4502C VCC]         [2004 LCD VCC]      [Flow Sensor VCC]
     |
 [3.3V LDO on DevKit / Discrete AMS1117-3.3]
     v
 +-------------------------------------------------------------------------+
 |                            +3V3 POWER BUS                               |
 +-------------------------------------------------------------------------+
     |                    |                      |
[ESP32-S3 VDD]       [TDS Meter VCC]        [DHT11 VCC]
```

### Power Rails Summary
| Power Rail | Nominal Voltage | Max Current Load | Decoupling Per Header | Users |
|---|---|---|---|---|
| **`+5V0`** | 5.0V ± 5% | ~800 mA | 100 µF Electrolytic + 0.1 µF MLCC | pH-4502C op-amp, 2004 LCD backlight, FS200A Hall sensor |
| **`+3V3`** | 3.3V ± 2% | ~500 mA (Peak 1A RF) | 10 µF Tantalum/Ceramic + 0.1 µF MLCC | ESP32-S3 Core, TDS Meter sensor, DHT11 sensor |
| **`GND`** | 0.0V (Common Reference) | — | Solid Ground Plane (Bottom Layer) | Common return for digital, analog, and power |

---

## 2. MCU Pin Allocation & Netlist Table

| ESP32-S3 Pin | Net Name | Hardware Subsystem | I/O Type | Signal Voltage | PCB Routing & Layout Recommendations |
|---|---|---|---|---|---|
| **GPIO 1** | `NET_TDS_ADC` | Analog TDS Meter | ADC1_CH0 | 0.0V – 2.5V Analog | Route as sensitive analog trace away from switching lines. Add low-pass RC filter ($100\,\Omega + 100\,\text{nF}$). |
| **GPIO 2** | `NET_PH_ADC` | pH-4502C Module | ADC1_CH1 | 0.0V – 3.0V Analog | Sensitive high-impedance analog trace. Keep short, guard with GND pour, add RC filter ($100\,\Omega + 100\,\text{nF}$). |
| **GPIO 4** | `NET_DHT11_DATA` | DHT11 Air Sensor | GPIO (Bi-dir) | 3.3V LVTTL | Single-wire bus. Add external $4.7\,\text{k}\Omega$ or $10\,\text{k}\Omega$ pull-up resistor to `+3V3`. |
| **GPIO 5** | `NET_FLOW_PULSE` | FS200A Flow Turbine | Ext Interrupt | 3.3V / 5.0V* | Hall-effect square wave output. Add $10\,\text{k}\Omega$ pull-up to `+3V3` and $1\,\text{k}\Omega$ series current-limiting resistor. |
| **GPIO 17** | `NET_I2C_SDA` | 2004 I2C LCD Backpack | I2C0_SDA | 3.3V / 5.0V* | Open-drain data line. Include footprint for $4.7\,\text{k}\Omega$ pull-up to `+3V3`. |
| **GPIO 18** | `NET_I2C_SCL` | 2004 I2C LCD Backpack | I2C0_SCL | 3.3V / 5.0V* | Open-drain clock line. Include footprint for $4.7\,\text{k}\Omega$ pull-up to `+3V3`. |

> [!CAUTION]
> **Octal Flash / PSRAM Pinout Restriction:**
> On the ESP32-S3, **GPIO 33, 34, 35, 36, and 37 are strictly reserved** for internal high-speed Octal SPI Flash/PSRAM bus. Never route analog or digital sensor signals to GPIO 33–37.

---

## 3. Peripheral Connector Specifications (Footprints & Pinouts)

### J1: Main DC Power Input (Screw Terminal)
- **Connector Type:** 2-Pin 5.08mm pitch Screw Terminal (e.g., Phoenix Contact / KF301-2P)
- **Pin 1:** `+12V` / `+5V_EXT` (Main DC In)
- **Pin 2:** `GND` (Power Ground)
- **Protection:** Add a reverse-polarity Schottky diode (e.g., SS34 / SS54) and TVS diode (SMAJ5.0A).

### J2: DHT11 Ambient Sensor (3-Pin Header)
- **Connector Type:** 3-Pin JST-XH 2.54mm or 1x3 Male Header
| Pin # | Net Name | Voltage | Description |
|---|---|---|---|
| 1 | `+3V3` | 3.3V | VCC Power Supply |
| 2 | `NET_DHT11_DATA` | 3.3V | Single-Wire Serial Data (Pull-up $4.7\,\text{k}\Omega$ to 3.3V) |
| 3 | `GND` | 0V | Ground |

### J3: Analog TDS Meter Module (3-Pin Header)
- **Connector Type:** 3-Pin JST-XH 2.54mm or 1x3 Male Header
| Pin # | Net Name | Voltage | Description |
|---|---|---|---|
| 1 | `NET_TDS_ADC` | 0 - 2.5V | Analog Output (`GPIO 1` / `ADC1_CH0`) |
| 2 | `+3V3` | 3.3V | VCC Power Supply |
| 3 | `GND` | 0V | Analog & Power Ground |

### J4: Water Flow Sensor FS200A (3-Pin Terminal / Header)
- **Connector Type:** 3-Pin 3.81mm/5.08mm Screw Terminal or JST-XH 2.54mm
| Pin # | Net Name | Wire Color | Description |
|---|---|---|---|
| 1 | `+5V0` (or `+3V3`) | Red | Hall Element VCC |
| 2 | `NET_FLOW_PULSE` | Yellow | Frequency Pulse Output (`GPIO 5`) |
| 3 | `GND` | Black | Ground Return |

### J5: pH-4502C Sensor Module Interface (4-Pin Header)
- **Connector Type:** 4-Pin JST-XH 2.54mm or 1x4 Male Header
| Pin # | Net Name | Voltage | Description |
|---|---|---|---|
| 1 | `+5V0` | 5.0V | ⚠️ **Mandatory 5V** (VCC for TL082 op-amp linear headroom) |
| 2 | `GND` | 0V | Power Ground |
| 3 | `NET_PH_ADC` | 0 - 3.0V | Analog Signal Out (`Po` pin -> `GPIO 2` / `ADC1_CH1`) |
| 4 | `GND` | 0V | Analog Signal Ground reference (`GND2`) |

*(Note: Pins `Do` and `To` on standard pH-4502C boards are threshold digital triggers and can be left unpopulated / NC).*

### J6: 2004 I2C Character LCD Interface (4-Pin Header)
- **Connector Type:** 4-Pin 2.54mm Male Header / JST-XH
| Pin # | Net Name | Voltage | Description |
|---|---|---|---|
| 1 | `GND` | 0V | Ground |
| 2 | `+5V0` | 5.0V | ⚠️ **Mandatory 5V** (LCD contrast and LED backlight require 5V) |
| 3 | `NET_I2C_SDA` | 3.3V / 5.0V | I2C Data (`GPIO 17`) |
| 4 | `NET_I2C_SCL` | 3.3V / 5.0V | I2C Clock (`GPIO 18`) |

---

## 4. Recommended Passive Components & Signal Conditioning

```
                        ANALOG INPUT FILTERING (GPIO 1 & GPIO 2)
                        
   Sensor AOUT >----+-----[ 100 Ohm ]-----+-----> ESP32-S3 ADC Pin
                    |                     |
                  [TVS]                [100nF]
                  Diode                Ceramic
                    |                     |
                   GND                   GND
```

1. **ADC Anti-Aliasing & Ripple RC Filters (for GPIO 1 & GPIO 2):**
   - Place a series resistor $R = 100\,\Omega$ (0805 or 0603).
   - Place a shunt ceramic capacitor $C = 100\,\text{nF}$ (X7R) to Ground right next to the ESP32 pin.
   - *Purpose:* Suppresses 50Hz/60Hz mains hum and pump inductive switching noise from the water body.

2. **I2C Bus Pull-Ups (GPIO 17 & GPIO 18):**
   - Provide two footprints for $4.7\,\text{k}\Omega$ resistors pulled up to `+3V3`.
   - If the PCF8574 LCD backpack already contains on-board $4.7\,\text{k}\Omega$ pull-ups to 5V, ensure clamping diodes (e.g., BAT54S) or a bi-directional logic level converter (BSS138 based) are added if true 3.3V clamping is desired.

3. **Flow Sensor Pulse Conditioning (GPIO 5):**
   - Series current-limiting resistor: $1\,\text{k}\Omega$.
   - Pull-up resistor: $10\,\text{k}\Omega$ to `+3V3`.
   - Small debounce capacitor: $1\,\text{nF}$ to GND.

---

## 5. PCB Layout & Routing Guidelines

1. **Ground Plane Architecture:**
   - Use a **2-layer or 4-layer PCB** with a solid, uninterrupted Ground Pour on Layer 2 (Bottom layer on 2-layer boards).
   - Do not cut ground planes beneath analog traces (`NET_TDS_ADC` and `NET_PH_ADC`).
2. **Trace Widths:**
   - **Power Traces (`+5V0`, `+3V3`, `V_IN`):** Minimum **24 to 32 mils (0.6mm - 0.8mm)** for low trace resistance.
   - **Analog Signals (`NET_TDS_ADC`, `NET_PH_ADC`):** **10 to 12 mils (0.25mm - 0.3mm)**, surrounded by ground shielding.
   - **Digital Signals (`I2C`, `DHT11`, `FLOW`):** **10 to 12 mils**.
3. **Decoupling Placement:**
   - Position $100\,\text{nF}$ decoupling capacitors as close as physically possible (within 3mm) to the MCU and connector power pins.
4. **Thermal Dissipation:**
   - If an on-board linear regulator (e.g. AMS1117-3.3 or LM7805) is used to step down 12V DC input, place a copper heat-sink pad with multiple thermal vias connected to the ground plane.

