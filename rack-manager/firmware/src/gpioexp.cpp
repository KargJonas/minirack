#include "gpioexp.h"
#include "i2cbus.h"
#include <Wire.h>

/* ---------------------------------------------------------------------------
 * PCA9555 GPIO expander driver
 *
 * Register map and I2C framing referenced from the PCA95x5 Arduino library:
 *   https://github.com/hideakitai/PCA95x5
 *
 * Behavioural details cited below are from the TI datasheet kept at
 * docs/pca9555.pdf (SCPS131J).
 *
 * The PCA9555PW (U6) sits on the same master I2C bus as the TCA9548A mux
 * (it is upstream of the mux, not behind it). From the PCB nets
 * (eda/rack-manager, U4 = WT32-ETH01):
 *
 *   SDA -> IO33   A0 = A1 = A2 = GND -> address 0x20
 *   SCL -> IO32   nINT-> IO36 (ESP32 input-only), pulled to 3V3 by R14 (10k)
 *
 * Port 0 (IO0_0..IO0_7) breaks out to header J24 and port 1 (IO1_0..IO1_7) to J25.
 *
 * The eight registers form four pairs - input, output, polarity, config - with
 * one register per port. Within a pair the command-byte pointer auto-toggles,
 * so both ports move in a single transaction (datasheet 8.5.2.4). The command
 * byte only encodes three bits, so 0x00..0x07 is the entire address space:
 * there is no pull-up control register, the input pull-ups are fixed silicon.
 *
 * Writable registers are shadowed in RAM. The input register reflects actual
 * pin voltages rather than commanded state, so it must never be used to build
 * an output byte - a loaded output or a pin still set as input would feed back
 * a level we never asked for.
 * ------------------------------------------------------------------------- */

/* SDA/SCL and the bus clock belong to i2cbus.cpp; these are the expander's own. */
static const int      PIN_nINT  = 36;  // nINT (input-only, active low)
static const uint8_t  EXP_ADDR = 0x20;

/* PCA9555 register (command byte) map */
enum : uint8_t {
    REG_INPUT_0  = 0x00, REG_INPUT_1  = 0x01,
    REG_OUTPUT_0 = 0x02, REG_OUTPUT_1 = 0x03,
    REG_POL_0    = 0x04, REG_POL_1    = 0x05,
    REG_CONFIG_0 = 0x06, REG_CONFIG_1 = 0x07,
};

/**
 * Shadows, initialised to the power-on defaults (datasheet table 8-3) and
 * re-seeded from the chip in gpioExpInit().
 */
static uint8_t s_input[2]  = {0xFF, 0xFF};
static uint8_t s_output[2] = {0xFF, 0xFF};
static uint8_t s_polarity[2] = {0x00, 0x00};
static uint8_t s_config[2] = {0xFF, 0xFF};   // 1 = input

static bool s_online = false;
static GpioExpChangeCb s_onChange = nullptr;

/**
 * nINT stays asserted until the port that changed is read, so the edge interrupt
 * is only a latency optimisation. The level check in gpioExpService() is what
 * makes a missed edge harmless. Wire is not ISR-safe, so the handler does nothing
 * but raise a flag.
 */
static volatile bool s_intPending = false;

static void IRAM_ATTR gpioExpIsr()
{
    s_intPending = true;
}

/**
 * Datasheet 8.4.1.1: if the command pointer is left at 0x00 - which a port 0
 * input read does, and another slave on the bus ACKs a read address, nINT is
 * de-asserted behind our back and the change is lost. The TCA9548A and the
 * sensors behind it share this bus, so that might happen. Sending a command
 * byte with no data bytes moves the pointer without writing a register.
 *
 * Only 0x00 is dangerous. A read addressed at any other command byte leaves
 * the pointer somewhere harmless and needs no park - which is what
 * readInputsFast() below exploits.
 */
static void parkPointer()
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(REG_OUTPUT_0);
    Wire.endTransmission();
}

/**
 * Read one 8-bit register; returns -1 if the device didn't respond.
 */
static int readReg(uint8_t reg)
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(reg);

    if (Wire.endTransmission(false) != 0) {   // repeated start
        s_online = false;
        return -1;
    }

    if (Wire.requestFrom((int)EXP_ADDR, 1) != 1) {
        s_online = false;
        return -1;
    }

    s_online = true;

    return Wire.read();
}

/**
 * Read both registers of a pair in one transaction, relying on the pointer
 * auto-toggle. 'reg' must be the port 0 register of the pair.
 */
static bool readPair(uint8_t reg, uint8_t out[2])
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(reg);

    if (Wire.endTransmission(false) != 0) {
        s_online = false;
        return false;
    }

    if (Wire.requestFrom((int)EXP_ADDR, 2) != 2) {
        s_online = false;
        return false;
    }

    out[0] = Wire.read();
    out[1] = Wire.read();
    s_online = true;

    return true;
}

/**
 * Read both input registers in one transaction and dont park afterwards.
 *
 * The trick is which end of the pair we address. The pointer auto-toggles
 * within a pair, so asking for 2 bytes from REG_INPUT_1 returns port 1 then
 * port 0 and toggles back to 0x01. Command byte 0x00 is never written and the
 * pointer never rests there, so the 8.4.1.1 errata - whose sole trigger is a
 * pointer left at 0x00 - cannot fire. The park it replaces exists only to
 * undo the damage of having addressed 0x00 in the first place.
 */
static bool readInputsFast(uint8_t out[2])
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(REG_INPUT_1);

    if (Wire.endTransmission(false) != 0) {
        s_online = false;
        return false;
    }

    if (Wire.requestFrom((int)EXP_ADDR, 2) != 2) {
        s_online = false;
        return false;
    }

    out[1] = Wire.read();   // INPUT_1 arrives first
    out[0] = Wire.read();
    s_online = true;

    return true;
}

static bool writeReg(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(reg);
    Wire.write(value);
    s_online = (Wire.endTransmission() == 0);
    return s_online;
}

void gpioExpInit()
{
    i2cBusInit();
    pinMode(PIN_nINT, INPUT);

    // We fetch every shadow from the chip instead of assuming the power-on defaults.
    uint8_t v[2];

    if (readPair(REG_INPUT_0, v)) { s_input[0] = v[0]; s_input[1] = v[1]; }
    parkPointer();
    if (readPair(REG_OUTPUT_0, v)) { s_output[0] = v[0]; s_output[1] = v[1]; }
    if (readPair(REG_POL_0, v))    { s_polarity[0] = v[0]; s_polarity[1] = v[1]; }
    if (readPair(REG_CONFIG_0, v)) { s_config[0] = v[0]; s_config[1] = v[1]; }

    attachInterrupt(digitalPinToInterrupt(PIN_nINT), gpioExpIsr, FALLING);
}

bool gpioExpOnline()
{
    return s_online;
}

uint8_t gpioExpRead(GpioExpPort port)
{
    int v = readReg(REG_INPUT_0 + port);
    // Only a port 0 read leaves the pointer at the errata's trigger value.
    // A  port 1 read ends at 0x01 and is already safe, so it skips the park
    // and costs one transaction instead of two.
    if (port == PORT_0) parkPointer();
    if (v < 0) return 0xFF;

    s_input[port] = (uint8_t)v;

    return (uint8_t)v;
}

bool gpioExpReadBoth(uint16_t *value)
{
    uint8_t in[2];
    bool ok = readPair(REG_INPUT_0, in);  // pointer auto-toggles to INPUT_1
    parkPointer();
    if (!ok) return false;
    s_input[0] = in[0];
    s_input[1] = in[1];
    if (value) *value = (uint16_t)in[1] << 8 | in[0];
    return true;
}

bool gpioExpReadBothFast(uint16_t *value)
{
    uint8_t in[2];
    if (!readInputsFast(in))
        return false;
    s_input[0] = in[0];
    s_input[1] = in[1];
    if (value)
        *value = (uint16_t)in[1] << 8 | in[0];
    return true;
}

bool gpioExpWrite(GpioExpPort port, uint8_t value)
{
    s_output[port] = value;
    return writeReg(REG_OUTPUT_0 + port, value);
}

bool gpioExpWriteBoth(uint16_t value)
{
    s_output[0] = (uint8_t)(value & 0xFF);
    s_output[1] = (uint8_t)(value >> 8);
    Wire.beginTransmission(EXP_ADDR);
    Wire.write(REG_OUTPUT_0);
    Wire.write(s_output[0]);
    Wire.write(s_output[1]);  // pointer auto-toggles to OUTPUT_1
    s_online = (Wire.endTransmission() == 0);
    return s_online;
}

bool gpioExpSetDirection(GpioExpPort port, uint8_t inputMask)
{
    // Push the shadow into the latch before configuring the direction,
    // to prevent setting the output to something we don't want.
    if (!writeReg(REG_OUTPUT_0 + port, s_output[port])) return false;
    s_config[port] = inputMask;
    return writeReg(REG_CONFIG_0 + port, inputMask);
}

bool gpioExpSetPolarity(GpioExpPort port, uint8_t invertMask)
{
    s_polarity[port] = invertMask;
    return writeReg(REG_POL_0 + port, invertMask);
}

uint8_t gpioExpGetOutput(GpioExpPort port)
{
    return s_output[port];
}

uint8_t gpioExpGetDirection(GpioExpPort port)
{
    return s_config[port];
}

uint8_t gpioExpGetPolarity(GpioExpPort port)
{
    return s_polarity[port];
}

void gpioExpOnChange(GpioExpChangeCb cb)
{
    s_onChange = cb;
}

void gpioExpService()
{
    // The level test backs up the edge. nINT holds low until serviced, so an
    // edge lost to the ACK-pulse race (datasheet 8.4.1) still gets picked up
    // on the next pass.
    if (!s_intPending && digitalRead(PIN_nINT) != LOW) return;

    // Cleared before the read, so a change landing mid-transaction re-arms the
    // flag instead of being swallowed by this pass.
    s_intPending = false;

    // The fast path: this is the latency-critical read, and one transaction
    // here rather than two is most of the input-latency budget.
    uint8_t in[2];
    if (!readInputsFast(in)) return;

    for (uint8_t p = 0; p < 2; p++) {
        uint8_t changed = (in[p] ^ s_input[p]) & s_config[p];
        s_input[p] = in[p];
        if (changed && s_onChange) s_onChange((GpioExpPort)p, in[p], changed);
    }
}
