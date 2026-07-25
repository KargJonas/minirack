#pragma once
#include <Arduino.h>

/**
 * The shared master I2C bus. See i2cbus.cpp for the pinout and the bus-speed
 * reasoning.
 *
 * Both the PCA9555 expander and the TCA9548A mux hang off it, so neither of
 * their drivers owns Wire: this module does. Their init functions call
 * i2cBusInit() and it is idempotent, so setup() order does not matter.
 */
void i2cBusInit();
