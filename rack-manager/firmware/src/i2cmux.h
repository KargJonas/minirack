#pragma once
#include <Arduino.h>

/**
 * TCA9548A I2C mux driver (U5, address 0x70). See i2cmux.cpp for the pinout.
 * Channel 0 carries an AHT20+BMP280 sensor board.
 */

/**
 * The eight downstream channels. Every one of them is wired to an AHT20+BMP280
 * sensor board (system-design/design.md), so the channel number is what
 * identifies a sensor - name them after their rack position here once the
 * boards are mounted.
 */
enum I2cMuxChannel : uint8_t {
    MUX_CH0 = 0,
    MUX_CH1 = 1,
    MUX_CH2 = 2,
    MUX_CH3 = 3,
    MUX_CH4 = 4,
    MUX_CH5 = 5,
    MUX_CH6 = 6,
    MUX_CH7 = 7,
};

/**
 * Bring up the shared I2C bus. Call once from setup().
 */
void i2cMuxInit();

/**
 * False if the most recent transaction to the mux failed. Nothing is written
 * to the mux until the first i2cMuxSelect()/i2cMuxDisableAll(), so this reads
 * false until then rather than reporting an untested chip as present.
 */
bool i2cMuxOnline();

/**
 * Connect exactly one downstream channel. Everything addressed on the shared
 * bus until the next call reaches that channel; devices upstream of the mux
 * (the PCA9555) stay reachable throughout.
 */
bool i2cMuxSelect(I2cMuxChannel channel);

/**
 * Disconnect every channel, leaving only the upstream devices on the bus.
 */
bool i2cMuxDisableAll();
