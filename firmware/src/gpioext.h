#pragma once
#include <Arduino.h>

/* PCA9555 GPIO extender bringup (U6, address 0x20). See gpioext.cpp for the
 * pinout. Read-only: dumps the register map to prove the chip responds
 * without changing any pin direction/state. */

/* Ensure the shared I2C bus is up. Call once from setup(). */
void gpioExtInit();

/* Probe the extender and read back its full register map. */
String gpioExtTestJson();
void   gpioExtTestSerial();
