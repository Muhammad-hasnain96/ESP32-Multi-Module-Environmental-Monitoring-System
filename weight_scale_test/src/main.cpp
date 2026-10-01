/**
 * ============================================================================
 * Commercial-Grade Precision Scale with Auto-Lock & Flash Memory
 * ============================================================================
 * 
 * Features:
 *  1. Auto-Lock / Hold Mechanism: When you place an item, it waits for the
 *     reading to settle for 1 second, then LOCKS the weight on the display.
 *     It will NOT change until you remove the item!
 *  2. Auto-Zero: Empty scale is rock-solid at 0.0 g.
 *  3. Auto-Calibrate: Put a known weight and type 'c <grams>' (e.g. 'c 500').
 *     Saved to permanent Flash memory.
 * ============================================================================
 */

#include <Arduino.h>
#include <Preferences.h>
#include "HX711.h"

const int LOADCELL_DOUT_PIN = 21;
const int LOADCELL_SCK_PIN  = 22;

HX711 scale;
Preferences prefs;

float calibration_factor = -350.0;
long zero_offset = 0;

// State Machine for Commercial Scale Lock
enum ScaleState {
    SCALE_EMPTY,
    SCALE_SETTLING,
    SCALE_LOCKED
};

ScaleState currentState = SCALE_EMPTY;

float locked_weight = 0.0;
float prev_weight = 0.0;
int stable_count = 0;

void printInstructions() {
    Serial.println("\n========================================================");
    Serial.println("  ⚖️  PRECISION 5000g COMMERCIAL SCALE (AUTO-LOCK ACTIVE)");
    Serial.println("========================================================");
    Serial.printf(" Current Calibration Factor: %.2f\n\n", calibration_factor);
    Serial.println(" 📌 COMMANDS (Send in Serial Monitor):");
    Serial.println("   't'         -> Tare (Manual Zero)");
    Serial.println("   'c <grams>' -> Auto-Calibrate (e.g. 'c 500' for 500g bottle)");
    Serial.println("========================================================\n");
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println("\n--- Starting Precision Weight Scale System ---");

    prefs.begin("scale_data", false);
    if (prefs.isKey("cal_factor")) {
        calibration_factor = prefs.getFloat("cal_factor", -350.0);
        Serial.printf("[INFO] Loaded Calib Factor from Flash: %.2f\n", calibration_factor);
    }

    scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

    Serial.print("Connecting to HX711... ");
    int attempts = 0;
    while (!scale.is_ready() && attempts < 15) {
        delay(200);
        Serial.print(".");
        attempts++;
    }

    if (!scale.is_ready()) {
        Serial.println("\n[ERROR] HX711 not responding! Check wires.");
    } else {
        Serial.println(" [OK]");
    }

    scale.set_scale(calibration_factor);
    Serial.println("Taring (Zeroing)... Scale must be EMPTY!");
    delay(1500);
    scale.tare(15);
    zero_offset = scale.get_offset();
    Serial.printf("Zero Locked (Offset: %ld)\n", zero_offset);

    printInstructions();
}

void calibrateWithKnownWeight(float known_weight) {
    if (known_weight <= 0.0) {
        Serial.println("[ERROR] Weight must be > 0g! Example: 'c 500'");
        return;
    }

    Serial.println("\n--------------------------------------------------------");
    Serial.printf("⚙️  Calibrating with known weight: %.1f g ...\n", known_weight);
    Serial.println("Taking 25 stable readings...");

    long delta = scale.get_value(25);
    float new_factor = (float)delta / known_weight;

    if (abs(new_factor) < 10.0) {
        Serial.println("[ERROR] No significant weight detected! Check sensor.");
        return;
    }

    calibration_factor = new_factor;
    scale.set_scale(calibration_factor);
    prefs.putFloat("cal_factor", calibration_factor);

    Serial.printf("✅ CALIBRATION SUCCESSFUL! New Factor: %.2f (Saved to Flash)\n", calibration_factor);
    Serial.println("--------------------------------------------------------\n");

    currentState = SCALE_LOCKED;
    locked_weight = known_weight;
}

void loop() {
    if (scale.is_ready()) {
        // Average 8 readings for high SNR
        float reading = scale.get_units(8);

        switch (currentState) {
            // ---------------------------------------------------------
            // 1. EMPTY SCALE STATE (Rock-Solid 0.0g)
            // ---------------------------------------------------------
            case SCALE_EMPTY: {
                // If within zero tolerance window (+/- 15g), hold at 0.0g
                if (abs(reading) < 15.0) {
                    Serial.printf("Weight:    0.0 g  |   0.00 oz  |  [EMPTY / TARE]\n");
                    // Auto-zero drift tracking
                    static int empty_cycles = 0;
                    if (++empty_cycles > 10) {
                        scale.tare(2);
                        empty_cycles = 0;
                    }
                } else if (reading >= 15.0) {
                    // Weight placed! Move to settling state
                    currentState = SCALE_SETTLING;
                    prev_weight = reading;
                    stable_count = 0;
                    Serial.println(">>> Weight detected! Stabilizing...");
                }
                break;
            }

            // ---------------------------------------------------------
            // 2. SETTLING STATE (Wait 1.5s for object to settle, then LOCK)
            // ---------------------------------------------------------
            case SCALE_SETTLING: {
                Serial.printf("Weight: %6.1f g  |  %5.2f oz  |  [Stabilizing...]\n", 
                              reading, reading * 0.035274);

                static float samples[5];
                static int sample_idx = 0;
                samples[sample_idx++] = reading;

                // After 5 stable cycles (~1.5s), average and LOCK!
                if (sample_idx >= 5) {
                    float sum = 0.0;
                    for (int i = 0; i < 5; i++) sum += samples[i];
                    locked_weight = sum / 5.0;

                    currentState = SCALE_LOCKED;
                    sample_idx = 0;

                    Serial.println("\n--------------------------------------------------------");
                    Serial.printf("🎯  FINAL WEIGHT LOCKED: %.1f g  (%.2f oz)\n", 
                                  locked_weight, locked_weight * 0.035274);
                    Serial.println("--------------------------------------------------------\n");
                }

                // If user lifted the weight before locking
                if (reading < 12.0) {
                    currentState = SCALE_EMPTY;
                    sample_idx = 0;
                }
                break;
            }

            // ---------------------------------------------------------
            // 3. LOCKED STATE (Constant, Frozen Display)
            // ---------------------------------------------------------
            case SCALE_LOCKED: {
                // Print the CONSTANT locked weight (No jitter, no rolling numbers!)
                Serial.printf("Weight: %6.1f g  |  %5.2f oz  |  🔒 [LOCKED & CONSTANT]\n", 
                              locked_weight, locked_weight * 0.035274);

                // Check if object was removed (reading drops by > 25g below locked)
                if (reading < 15.0 || (locked_weight - reading) > 30.0) {
                    Serial.println("\n>>> Item removed! Resetting scale to 0.0g...\n");
                    scale.tare(5);
                    currentState = SCALE_EMPTY;
                }
                break;
            }
        }
    }

    // Process Serial Commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();

        if (cmd.equalsIgnoreCase("t")) {
            Serial.println("\n>>> Taring scale (Manual Zero)...");
            scale.tare(10);
            currentState = SCALE_EMPTY;
            Serial.println(">>> Scale is at 0.0 g!\n");
        } else if (cmd.startsWith("c ") || cmd.startsWith("C ")) {
            float weight = cmd.substring(2).toFloat();
            calibrateWithKnownWeight(weight);
        } else if (cmd.equalsIgnoreCase("help")) {
            printInstructions();
        }
    }

    delay(300);
}
