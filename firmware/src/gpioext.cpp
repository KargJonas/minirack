#include "gpioext.h"
#include <Wire.h>

/* ---------------------------------------------------------------------------
 * PCA9555 GPIO extender bringup
 *
 * Register map and I2C framing referenced from the PCA95x5 Arduino library:
 *   https://github.com/hideakitai/PCA95x5
 *
 * The PCA9555PW (U6) sits on the same master I2C bus as the TCA9548A mux
 * (it is upstream of the mux, not behind it). From the PCB nets
 * (eda/rack-manager, U4 = WT32-ETH01):
 *
 *   SDA -> IO33   A0 = A1 = A2 = GND -> address 0x20
 *   SCL -> IO32   ~INT -> IO36 (ESP32 input-only)
 *
 * Port 0 (IO0_0..IO0_7) breaks out to header J24 and port 1 (IO1_0..IO1_7) to J25.
 * At power-on every pin is an input (config = 0xFF), so this bringup stays read-only.
 * It dumps all eight registers to confirm the chip answers and its register map is
 * coherent, and never writes a direction/output (which could drive a header pin).
 * ------------------------------------------------------------------------- */

static const int      PIN_SDA  = 33;
static const int      PIN_SCL  = 32;
static const int      PIN_INT  = 36;   /* ~INT, input-only, active low */
static const uint8_t  EXP_ADDR = 0x20;
static const uint32_t I2C_HZ   = 100000;

/* PCA9555 register (command byte) map */
enum : uint8_t {
    REG_INPUT_0  = 0x00, REG_INPUT_1  = 0x01,
    REG_OUTPUT_0 = 0x02, REG_OUTPUT_1 = 0x03,
    REG_POL_0    = 0x04, REG_POL_1    = 0x05,
    REG_CONFIG_0 = 0x06, REG_CONFIG_1 = 0x07,
};

/* Read one 8-bit register; returns -1 if the device didn't respond. */
static int extRead(uint8_t reg)
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)   /* repeated start */
        return -1;
    if (Wire.requestFrom((int)EXP_ADDR, 1) != 1)
        return -1;
    return Wire.read();
}

void gpioExtInit()
{
    /* Shared with the mux; Wire.begin() is idempotent so this stays
     * independent of module init order. */
    Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
    Wire.setTimeOut(50);
    pinMode(PIN_INT, INPUT);
}

String gpioExtTestJson()
{
    int in0  = extRead(REG_INPUT_0),  in1  = extRead(REG_INPUT_1);
    int out0 = extRead(REG_OUTPUT_0), out1 = extRead(REG_OUTPUT_1);
    int pol0 = extRead(REG_POL_0),    pol1 = extRead(REG_POL_1);
    int cfg0 = extRead(REG_CONFIG_0), cfg1 = extRead(REG_CONFIG_1);
    bool alive = cfg0 >= 0 && cfg1 >= 0;

    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"addr\":\"0x20\",\"sda\":33,\"scl\":32,\"int_pin\":36,"
             "\"int_level\":%d,\"alive\":%s,"
             "\"input\":[\"0x%02X\",\"0x%02X\"],"
             "\"output\":[\"0x%02X\",\"0x%02X\"],"
             "\"polarity\":[\"0x%02X\",\"0x%02X\"],"
             "\"config\":[\"0x%02X\",\"0x%02X\"]}\n",
             digitalRead(PIN_INT), alive ? "true" : "false",
             in0 & 0xFF, in1 & 0xFF, out0 & 0xFF, out1 & 0xFF,
             pol0 & 0xFF, pol1 & 0xFF, cfg0 & 0xFF, cfg1 & 0xFF);
    return String(buf);
}

void gpioExtTestSerial()
{
    int cfg0 = extRead(REG_CONFIG_0), cfg1 = extRead(REG_CONFIG_1);
    if (cfg0 < 0 || cfg1 < 0) {
        Serial.println("[gpioext] PCA9555 @ 0x20: NO RESPONSE");
        return;
    }
    int in0 = extRead(REG_INPUT_0), in1 = extRead(REG_INPUT_1);
    Serial.printf("[gpioext] PCA9555 @ 0x20: ALIVE  config=0x%02X%02X "
                  "input=0x%02X%02X  INT(IO36)=%d\n",
                  cfg0, cfg1, in0, in1, digitalRead(PIN_INT));
}
