#include "i2cmux.h"
#include "i2cbus.h"
#include <Wire.h>

/* ---------------------------------------------------------------------------
 * TCA9548A I2C mux driver
 *
 * Channel-select framing referenced from Rob Tillaart's TCA9548 library:
 *   https://github.com/RobTillaart/TCA9548
 *
 * The TCA9548AMRGER (U5) upstream I2C and its address straps, from the PCB
 * nets (eda/rack-manager, U4 = WT32-ETH01):
 *
 *   mux SDA -> IO33      A0 = A1 = A2 = GND  -> address 0x70
 *   mux SCL -> IO32      ~RESET tied to the ESP32 EN/reset net (auto-released)
 *
 * Selecting a channel is a single write to 0x70 of a one-hot bitmask
 * (bit N = channel N). Channel 0 carries an AHT20+BMP280 sensor board:
 *   AHT20  -> 0x38 (fixed)
 *   BMP280 -> 0x76 (SDO=GND) or 0x77 (SDO=VDDIO); chip-id reg 0xD0 reads 0x58.
 * ------------------------------------------------------------------------- */

/* SDA/SCL and the bus clock belong to i2cbus.cpp. */
static const uint8_t  MUX_ADDR  = 0x70;

static bool s_online = false;

/* Write the channel bitmask; 0x00 disconnects everything. */
static bool muxWrite(uint8_t mask)
{
    Wire.beginTransmission(MUX_ADDR);
    Wire.write(mask);
    s_online = (Wire.endTransmission() == 0);
    return s_online;
}

void i2cMuxInit()
{
    i2cBusInit();
}

bool i2cMuxOnline()
{
    return s_online;
}

bool i2cMuxSelect(I2cMuxChannel channel)
{
    if (channel > MUX_CH7) return false;   // guards a cast-in channel
    return muxWrite((uint8_t)(1u << channel));
}

bool i2cMuxDisableAll()
{
    return muxWrite(0x00);
}
