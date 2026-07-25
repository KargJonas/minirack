#pragma once
#include <Arduino.h>

/**
 * TCA9548A I2C mux bringup (U5, address 0x70). See i2cmux.cpp for the pinout.
 * Probes the mux, selects each channel and scans for downstream devices;
 * channel 0 carries an AHT20+BMP280 sensor board. */

/**
 * Bring up the I2C bus for the mux. Call once from setup().
 */
void i2cMuxInit();

/**
 * Probe the mux and scan its channels for responding I2C devices.
 */
String i2cMuxTestJson();
void   i2cMuxTestSerial();

/*
 * Deep diagnostic for the (silent) BMP280 on channel 0: reports raw I2C
 * transaction result codes at 0x76/0x77 across bus speeds, with an AHT20
 * read on the same channel as a known-good control.
 */
String bmp280DebugJson();
