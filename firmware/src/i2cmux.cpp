#include "i2cmux.h"
#include <Wire.h>

/* ---------------------------------------------------------------------------
 * TCA9548A I2C mux bringup
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

static const int      PIN_SDA   = 33;
static const int      PIN_SCL   = 32;
static const uint8_t  MUX_ADDR  = 0x70;
static const uint32_t I2C_HZ    = 100000;

static const uint8_t  AHT20_ADDR      = 0x38;
static const uint8_t  BMP280_ADDR_LO  = 0x76;
static const uint8_t  BMP280_ADDR_HI  = 0x77;
static const uint8_t  BMP280_REG_ID   = 0xD0;
static const uint8_t  BMP280_CHIP_ID  = 0x58;

/* Enable exactly one downstream channel (or 0x00 for none). */
static bool muxSelect(uint8_t channel)
{
    Wire.beginTransmission(MUX_ADDR);
    Wire.write((uint8_t)(1u << channel));
    return Wire.endTransmission() == 0;
}

static bool i2cAck(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

/* Read one 8-bit register; returns false if the device didn't respond. */
static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t *out)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0)   /* repeated start */
        return false;
    if (Wire.requestFrom((int)addr, 1) != 1)
        return false;
    *out = Wire.read();
    return true;
}

void i2cMuxInit()
{
    Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
    Wire.setTimeOut(50);
}

/* Probe the mux, then walk all 8 channels scanning for I2C devices.
 * Channel 0's AHT20/BMP280 are identified explicitly. */
String i2cMuxTestJson()
{
    bool muxPresent = i2cAck(MUX_ADDR);

    String out = "{\"mux_addr\":\"0x70\",\"sda\":33,\"scl\":32,\"mux_present\":";
    out += muxPresent ? "true" : "false";
    out += ",\"channels\":[";

    for (uint8_t ch = 0; ch < 8; ch++) {
        muxSelect(ch);

        out += ch ? ",{\"ch\":" : "{\"ch\":";
        out += ch;
        out += ",\"devices\":[";
        bool first = true;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (addr == MUX_ADDR)   /* the mux itself acks on every channel */
                continue;
            if (i2cAck(addr)) {
                char b[8];
                snprintf(b, sizeof(b), "%s\"0x%02X\"", first ? "" : ",", addr);
                out += b;
                first = false;
            }
        }
        out += "]";

        if (ch == 0) {   /* identify the known sensor board */
            uint8_t id = 0;
            bool bmpLo = i2cReadReg(BMP280_ADDR_LO, BMP280_REG_ID, &id) &&
                         id == BMP280_CHIP_ID;
            uint8_t idHi = 0;
            bool bmpHi = i2cReadReg(BMP280_ADDR_HI, BMP280_REG_ID, &idHi) &&
                         idHi == BMP280_CHIP_ID;
            char b[160];
            snprintf(b, sizeof(b),
                     ",\"aht20\":{\"addr\":\"0x38\",\"present\":%s},"
                     "\"bmp280\":{\"addr\":\"%s\",\"chip_id\":\"0x%02X\",\"ok\":%s}",
                     i2cAck(AHT20_ADDR) ? "true" : "false",
                     bmpLo ? "0x76" : (bmpHi ? "0x77" : "none"),
                     bmpLo ? id : idHi,
                     (bmpLo || bmpHi) ? "true" : "false");
            out += b;
        }
        out += "}";
    }
    Wire.beginTransmission(MUX_ADDR);   /* leave all channels off */
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();

    out += "]}\n";
    return out;
}

/* Probe one address and return the raw Wire.endTransmission() code:
 * 0=ACK, 2=NACK on address, 3=NACK on data, 4=other, 5=timeout. */
static int i2cAddrProbe(uint8_t addr)
{
    Wire.beginTransmission(addr);
    return Wire.endTransmission(true);
}

/* Deep-probe the BMP280 on channel 0. AHT20 (same channel) is the control:
 * if it reads back but 0x76/0x77 give code 2 (address NACK), the BMP280 is
 * not acknowledging its own address at all - a hardware condition (e.g. CSB
 * not tied high so the part is in SPI mode, or a bad SDI/SCK/CSB joint),
 * not something firmware can talk past. */
String bmp280DebugJson()
{
    muxSelect(0);

    /* Control: a bare read of the AHT20 returns its status byte. */
    int ahtCount = Wire.requestFrom((int)AHT20_ADDR, 1);
    int ahtStatus = ahtCount == 1 ? Wire.read() : -1;

    String out = "{\"channel\":0,";
    char b[96];
    snprintf(b, sizeof(b),
             "\"control_aht20\":{\"addr\":\"0x38\",\"bytes\":%d,\"status\":%d},",
             ahtCount, ahtStatus);
    out += b;
    out += "\"bmp280_probes\":[";

    const uint32_t speeds[] = {100000, 50000, 400000};
    const uint8_t  addrs[]  = {BMP280_ADDR_LO, BMP280_ADDR_HI};
    bool first = true;

    for (uint32_t hz : speeds) {
        Wire.setClock(hz);
        for (uint8_t addr : addrs) {
            int probe = i2cAddrProbe(addr);

            Wire.beginTransmission(addr);
            Wire.write(BMP280_REG_ID);
            int regWrite = Wire.endTransmission(false);   /* repeated start */
            int rdCount = Wire.requestFrom((int)addr, 1);
            int id = rdCount == 1 ? Wire.read() : -1;

            snprintf(b, sizeof(b),
                     "%s{\"hz\":%u,\"addr\":\"0x%02X\",\"probe_code\":%d,"
                     "\"regwrite_code\":%d,\"read_bytes\":%d,\"chip_id\":%d}",
                     first ? "" : ",", hz, addr, probe, regWrite, rdCount, id);
            out += b;
            first = false;
        }
    }

    Wire.setClock(I2C_HZ);
    Wire.beginTransmission(MUX_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();

    out += "],\"code_legend\":\"0=ACK 2=addr-NACK 3=data-NACK 4=other 5=timeout\"}\n";
    return out;
}

void i2cMuxTestSerial()
{
    bool muxPresent = i2cAck(MUX_ADDR);
    Serial.printf("[i2cmux] TCA9548A @ 0x70: %s\n",
                  muxPresent ? "ACK (present)" : "NO RESPONSE");
    if (!muxPresent)
        return;

    for (uint8_t ch = 0; ch < 8; ch++) {
        muxSelect(ch);
        Serial.printf("  ch%u:", ch);
        bool any = false;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            if (addr == MUX_ADDR)
                continue;
            if (i2cAck(addr)) {
                Serial.printf(" 0x%02X", addr);
                any = true;
            }
        }
        if (!any)
            Serial.print(" -");
        if (ch == 0) {
            uint8_t id = 0;
            bool bmp = (i2cReadReg(BMP280_ADDR_LO, BMP280_REG_ID, &id) ||
                        i2cReadReg(BMP280_ADDR_HI, BMP280_REG_ID, &id));
            Serial.printf("   [AHT20 %s, BMP280 id=0x%02X %s]",
                          i2cAck(AHT20_ADDR) ? "OK" : "missing",
                          id, id == BMP280_CHIP_ID ? "OK" : "??");
        }
        Serial.println();
    }
    Wire.beginTransmission(MUX_ADDR);
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();
}
