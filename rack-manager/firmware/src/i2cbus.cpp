#include "i2cbus.h"
#include <Wire.h>

/* ---------------------------------------------------------------------------
 * Shared master I2C bus
 *
 * From the PCB nets (eda/rack-manager, U4 = WT32-ETH01):
 *
 *   SDA -> IO33      SCL -> IO32
 *
 * Two devices sit directly on it: the PCA9555 expander (0x20) and the TCA9548A
 * mux (0x70), with eight sensor channels behind the latter. Both drivers used
 * to call Wire.begin() with their own copy of the pins and the clock, which
 * left the two copies to be kept in agreement by hand and made whichever
 * init() ran second silently re-initialise the bus. This module owns that
 * setup so there is exactly one place for it to be right.
 * ------------------------------------------------------------------------- */

static const int PIN_SDA = 33;
static const int PIN_SCL = 32;

/**
 * Fast-mode. The PCA9555 tops out here (datasheet 9.1.1: "Fast-mode:
 * fSCL = 400 kHz"), as do the TCA9548A and the sensors behind it.
 *
 * Bus capacitance is the thing to watch: Fast-mode wants tr <= 300 ns and
 * tr ~= 0.847 * Rp * Cb, so the board's 4.7k pull-ups (R17/R18) allow only
 * ~75 pF. On-board load is ~30-45 pF, which is why this works, but J13 hangs
 * off the same bus - anything plugged in there with a cable can exceed the
 * budget. Drop R17/R18 to 2.2k (~161 pF) if J13 is ever populated.
 */
static const uint32_t I2C_HZ = 400000;

/* Wire.begin() on an already-running bus re-runs the peripheral setup, so the
 * second caller has to be a no-op rather than a repeat. */
static bool s_begun = false;

void i2cBusInit()
{
    if (s_begun) return;

    Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
    Wire.setTimeOut(50);
    s_begun = true;
}
