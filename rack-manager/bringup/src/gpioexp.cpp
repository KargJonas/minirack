#include "gpioexp.h"
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

static const int      PIN_SDA  = 33;
static const int      PIN_SCL  = 32;
static const int      PIN_nINT  = 36;  // nINT (input-only, active low)
static const uint8_t  EXP_ADDR = 0x20;

/**
 * Fast-mode. The PCA9555 tops out here (datasheet 9.1.1: "Fast-mode:
 * fSCL = 400 kHz"), as do the TCA9548A and the sensors behind it.
 *
 * Bus capacitance is the thing to watch: Fast-mode wants tr <= 300 ns and
 * tr ~= 0.847 * Rp * Cb, so the board's 4.7k pull-ups (R17/R18) allow only
 * ~75 pF. On-board load is ~30-45 pF, which is why this works, but J13 hangs
 * off the same bus - anything plugged in there with a cable can exceed the
 * budget. Drop R17/R18 to 2.2k (~161 pF) if J13 is ever populated.
 *
 * Must stay in agreement with I2C_HZ in i2cmux.cpp: both modules call
 * Wire.begin()/setClock() on this same shared bus.
 */
static const uint32_t I2C_HZ   = 400000;

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
    Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
    Wire.setTimeOut(50);
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

String gpioExpTestJson()
{
    uint8_t in[2] = {0xFF, 0xFF}, out[2] = {0xFF, 0xFF};
    uint8_t pol[2] = {0x00, 0x00}, cfg[2] = {0xFF, 0xFF};

    bool ok = readPair(REG_INPUT_0, in);
    parkPointer();
    ok = readPair(REG_OUTPUT_0, out) && ok;
    ok = readPair(REG_POL_0, pol) && ok;
    ok = readPair(REG_CONFIG_0, cfg) && ok;

    char buf[384];
    snprintf(buf, sizeof(buf),
        "{\"addr\":\"0x20\",\"sda\":33,\"scl\":32,\"int_pin\":36,"
        "\"int_level\":%d,\"alive\":%s,"
        "\"input\":[\"0x%02X\",\"0x%02X\"],"
        "\"output\":[\"0x%02X\",\"0x%02X\"],"
        "\"polarity\":[\"0x%02X\",\"0x%02X\"],"
        "\"config\":[\"0x%02X\",\"0x%02X\"]}\n",
        digitalRead(PIN_nINT), ok ? "true" : "false",
        in[0], in[1], out[0], out[1],
        pol[0], pol[1], cfg[0], cfg[1]);

    return String(buf);
}

void gpioExpTestSerial()
{
    uint8_t cfg[2], in[2];

    if (!readPair(REG_CONFIG_0, cfg)) {
        Serial.println("[gpioexp] PCA9555 @ 0x20: NO RESPONSE");
        return;
    }

    bool ok = readPair(REG_INPUT_0, in);
    parkPointer();

    if (!ok) {
        Serial.println("[gpioexp] PCA9555 @ 0x20: input read FAILED");
        return;
    }

    Serial.printf("[gpioexp] PCA9555 @ 0x20: ALIVE  config=0x%02X%02X "
        "input=0x%02X%02X  INT(IO36)=%d\n",
        cfg[0], cfg[1], in[0], in[1], digitalRead(PIN_nINT));
}

/* --- logic-analyzer self-test -------------------------------------------
 *
 * Drives a known sequence out of port 0 (header J24) so a logic analyzer can
 * confirm every bit lands on the pin it should, and so the update rate can be
 * measured against something other than our own micros().
 *
 * Capture layout, all on port 0:
 *   preamble  0xFF 2 ms, 0x00 2 ms   - pulses far longer than anything that
 *                                      follows, so the start is unmistakable
 *   ramp      0x00..0xFF, ~1 ms each - proves bit-to-pin mapping; each step is
 *                                      also read back and compared in firmware
 *   gap       0x00 5 ms
 *   burst A   0x00/0xFF flat out, 8-bit writes at 100 kHz
 *   gap, burst B  same at 400 kHz
 *   gap, burst C  16-bit writes (both ports) at 400 kHz
 *   end       port 0 back to inputs, pull-ups park it at 0xFF
 *
 * The burst values alternate 0x00/0xFF, so on the LA the update rate is just
 * half the observed square-wave frequency on any port-0 pin.
 * ------------------------------------------------------------------------- */

struct Bench {
    uint32_t writes, errors, elapsed_us;
};

/* Single-shot timings, as opposed to the sustained rates above. These are the
 * latency numbers: how long one call blocks before the pin has moved (write)
 * or before the new input byte is in hand (read). */
struct Lat {
    uint32_t n, min_us, max_us, sum_us;
};

static void latSample(Lat *l, uint32_t us)
{
    if (l->n == 0 || us < l->min_us) l->min_us = us;
    if (us > l->max_us) l->max_us = us;
    l->sum_us += us;
    l->n++;
}

static bool     s_stPending = false;
static bool     s_stDone    = false;
static uint32_t s_stRampErrors   = 0;
static uint16_t s_stFirstBadStep = 0xFFFF;
static uint8_t  s_stFirstBadRead = 0;
static uint32_t s_stTotalMs = 0;
static Bench    s_b8_100, s_b8_400, s_b16_400;
static Lat      s_lw100, s_lw400, s_lr100, s_lr400;

void gpioExpSelfTestRequest()
{
    s_stPending = true;
}

/* Alternate the port as fast as the bus allows for 'ms'. Port 1's latch is
 * carried through untouched so the 16-bit case never disturbs J25. */
static void runBench(Bench *b, uint32_t hz, uint32_t ms, bool both)
{
    Wire.setClock(hz);
    uint32_t n = 0, err = 0;
    uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < ms * 1000UL) {
        uint8_t lo = (n & 1) ? 0xFF : 0x00;
        bool ok = both ? gpioExpWriteBoth((uint16_t)s_output[1] << 8 | lo)
                       : gpioExpWrite(PORT_0, lo);
        if (!ok) err++;
        n++;
    }
    b->elapsed_us = (uint32_t)(micros() - t0);
    b->writes = n;
    b->errors = err;
}

void gpioExpSelfTestPump()
{
    if (!s_stPending) return;
    s_stPending = false;

    uint32_t tStart = millis();
    s_stRampErrors = 0;
    s_stFirstBadStep = 0xFFFF;

    /* Park low before switching to outputs, so the turn-around does not put a
     * spurious 0xFF on the header (the latch powers up all-high). */
    gpioExpWrite(PORT_0, 0x00);
    gpioExpSetDirection(PORT_0, 0x00);   /* all eight pins outputs */

    gpioExpWrite(PORT_0, 0xFF); delay(2);
    gpioExpWrite(PORT_0, 0x00); delay(2);

    for (uint16_t v = 0; v <= 0xFF; v++) {
        gpioExpWrite(PORT_0, (uint8_t)v);
        delayMicroseconds(300);
        /* Outputs are unloaded (the LA is high-impedance), so the input
         * register must mirror exactly what we drove. This is the readback
         * check the LA capture is cross-checked against. Alternate the two
         * read paths so both get covered: the single-port read, and the
         * paired read that costs two transactions rather than four. */
        uint8_t rb;
        if (v & 1) {
            uint16_t both = 0xFFFF;
            gpioExpReadBoth(&both);
            rb = (uint8_t)(both & 0xFF);
        } else {
            rb = gpioExpRead(PORT_0);
        }
        if (rb != (uint8_t)v) {
            if (s_stFirstBadStep == 0xFFFF) {
                s_stFirstBadStep = v;
                s_stFirstBadRead = rb;
            }
            s_stRampErrors++;
        }
        delayMicroseconds(300);
    }

    /* Latency pass: isolated calls with idle gaps between them, so nothing is
     * pipelined and each sample is a standalone call-to-completion time. The
     * read path is what gpioExpService() runs on an interrupt, so it is timed
     * exactly as that uses it: paired input read plus the errata pointer park. */
    memset(&s_lw100, 0, sizeof(s_lw100)); memset(&s_lw400, 0, sizeof(s_lw400));
    memset(&s_lr100, 0, sizeof(s_lr100)); memset(&s_lr400, 0, sizeof(s_lr400));
    for (int pass = 0; pass < 2; pass++) {
        uint32_t hz = pass ? 400000 : 100000;
        Lat *lw = pass ? &s_lw400 : &s_lw100;
        Lat *lr = pass ? &s_lr400 : &s_lr100;
        Wire.setClock(hz);
        for (int i = 0; i < 40; i++) {
            uint8_t in[2];
            uint32_t t0 = micros();
            gpioExpWrite(PORT_0, (i & 1) ? 0xFF : 0x00);
            latSample(lw, (uint32_t)(micros() - t0));
            delayMicroseconds(500);
            t0 = micros();
            readPair(REG_INPUT_0, in);
            parkPointer();
            latSample(lr, (uint32_t)(micros() - t0));
            delayMicroseconds(500);
        }
    }

    gpioExpWrite(PORT_0, 0x00); delay(5);
    runBench(&s_b8_100, 100000, 50, false);
    gpioExpWrite(PORT_0, 0x00); delay(5);
    runBench(&s_b8_400, 400000, 50, false);
    gpioExpWrite(PORT_0, 0x00); delay(5);
    runBench(&s_b16_400, 400000, 50, true);

    /* Restore: bus back to the speed the other devices were set up with, and
     * port 0 back to inputs so nothing is left driving the header. */
    Wire.setClock(I2C_HZ);
    gpioExpWrite(PORT_0, 0x00);
    gpioExpSetDirection(PORT_0, 0xFF);

    s_stTotalMs = millis() - tStart;
    s_stDone = true;

    Serial.printf("[gpioexp] selftest: ramp %u/256 mismatches, %lu ms total\n",
                  (unsigned)s_stRampErrors, (unsigned long)s_stTotalMs);
}

static void benchJson(char *dst, size_t n, const char *name, const Bench *b, uint8_t bits)
{
    float us = b->writes ? (float)b->elapsed_us / b->writes : 0.0f;
    float hz = us > 0 ? 1e6f / us : 0.0f;
    snprintf(dst, n,
             "\"%s\":{\"writes\":%lu,\"errors\":%lu,\"elapsed_us\":%lu,"
             "\"us_per_write\":%.2f,\"writes_per_s\":%.0f,\"kbit_per_s\":%.1f}",
             name, (unsigned long)b->writes, (unsigned long)b->errors,
             (unsigned long)b->elapsed_us, us, hz, hz * bits / 1000.0f);
}

String gpioExpSelfTestJson()
{
    if (!s_stDone)
        return String("{\"done\":false}\n");

    char j8a[220], j8b[220], j16[220];
    benchJson(j8a, sizeof(j8a), "w8_100k", &s_b8_100, 8);
    benchJson(j8b, sizeof(j8b), "w8_400k", &s_b8_400, 8);
    benchJson(j16, sizeof(j16), "w16_400k", &s_b16_400, 16);

    char lat[420];
    snprintf(lat, sizeof(lat),
             "\"latency_us\":{"
             "\"write8_100k\":{\"n\":%lu,\"min\":%lu,\"avg\":%.1f,\"max\":%lu},"
             "\"write8_400k\":{\"n\":%lu,\"min\":%lu,\"avg\":%.1f,\"max\":%lu},"
             "\"read16_100k\":{\"n\":%lu,\"min\":%lu,\"avg\":%.1f,\"max\":%lu},"
             "\"read16_400k\":{\"n\":%lu,\"min\":%lu,\"avg\":%.1f,\"max\":%lu}}",
             (unsigned long)s_lw100.n, (unsigned long)s_lw100.min_us,
             s_lw100.n ? (float)s_lw100.sum_us / s_lw100.n : 0.0f, (unsigned long)s_lw100.max_us,
             (unsigned long)s_lw400.n, (unsigned long)s_lw400.min_us,
             s_lw400.n ? (float)s_lw400.sum_us / s_lw400.n : 0.0f, (unsigned long)s_lw400.max_us,
             (unsigned long)s_lr100.n, (unsigned long)s_lr100.min_us,
             s_lr100.n ? (float)s_lr100.sum_us / s_lr100.n : 0.0f, (unsigned long)s_lr100.max_us,
             (unsigned long)s_lr400.n, (unsigned long)s_lr400.min_us,
             s_lr400.n ? (float)s_lr400.sum_us / s_lr400.n : 0.0f, (unsigned long)s_lr400.max_us);

    char buf[1400];
    snprintf(buf, sizeof(buf),
             "{\"done\":true,\"total_ms\":%lu,"
             "\"ramp\":{\"steps\":256,\"mismatches\":%lu,"
             "\"first_bad_step\":%d,\"first_bad_read\":\"0x%02X\"},"
             "%s,%s,%s,%s}\n",
             (unsigned long)s_stTotalMs, (unsigned long)s_stRampErrors,
             s_stFirstBadStep == 0xFFFF ? -1 : (int)s_stFirstBadStep,
             s_stFirstBadRead, j8a, j8b, j16, lat);
    return String(buf);
}

/* --- port-to-port loopback ------------------------------------------------
 *
 * For a board with J24 wired 1:1 to J25. One port drives, the other reads, so
 * the read and interrupt paths finally see a real external edge instead of
 * pins this driver is itself driving. Everything the write self-test above
 * could not reach lives here: port 1 as an input, the polarity register, nINT
 * timing, and end-to-end "pin moved -> application knows".
 *
 * HAZARD - the reason every direction change goes through lbSetDriver():
 * PCA9555 outputs are push-pull (datasheet 8.3.1, latched outputs with
 * high-current drive). With the loom fitted, two ports configured as outputs
 * at once means eight drivers fighting eight drivers through a short. The
 * chip survives it - latch-up is specified past 100 mA per JESD 78 Class II -
 * but nothing about it is good, so both ports are always released to inputs
 * before either is enabled, never one config write straight to the other.
 *
 * The bit mapping is discovered rather than assumed. If the loom follows
 * header pin order it cancels out - J24 and J25 are both IDC-Header_2x06 and
 * interleave their nibbles identically, so pin N to pin N lands IO0_i on
 * IO1_i - but that is a fact to measure, not to trust.
 * ------------------------------------------------------------------------- */

static const uint32_t LB_INT_TIMEOUT_US = 5000;
static const int      LB_LAT_ITERS      = 50;

static bool     s_lbPending = false;
static bool     s_lbDone    = false;
static uint32_t s_lbTotalMs = 0;

/* Phase A - wiring */
static uint8_t  s_lbMap[8];          /* port 0 bit i drives port 1 bit map[i] */
static uint8_t  s_lbInv[8];          /* the other direction */
static uint8_t  s_lbWalkHi[8], s_lbWalkLo[8];
static bool     s_lbMapOk = false;
static bool     s_lbMapIdentity = false;
static const char *s_lbWiringNote = "";

/* Phase B - ramps */
static uint16_t s_lbRamp01Err = 0, s_lbRamp10Err = 0;
static int      s_lbRamp01Bad = -1, s_lbRamp10Bad = -1;
static uint8_t  s_lbRamp01Got = 0,  s_lbRamp10Got = 0;

/* Phase C - polarity */
static bool     s_lbPolOk = false;
static uint8_t  s_lbPolDriven = 0, s_lbPolNormal = 0, s_lbPolInverted = 0;

/* Phase D - per-port interrupt clearing */
static bool     s_lbIntPortOk = false;
static int      s_lbIntOnChange = -1, s_lbIntAfterP0 = -1, s_lbIntAfterP1 = -1;

/* Phase E/F - latency */
static Lat      s_lbLatWrite, s_lbLatAssert, s_lbLatRead, s_lbLatTotal, s_lbLatE2E;
static uint32_t s_lbIntTimeouts = 0, s_lbReadBad = 0;
static uint32_t s_lbE2ETimeouts = 0, s_lbE2EBad = 0;

/* Phase G/H - read throughput and the fast path */
static Bench    s_lbR8_400, s_lbR16_400, s_lbR16f_400, s_lbR16_100;
static bool     s_lbFastOk = false;
static uint8_t  s_lbFastP0 = 0, s_lbFastP1 = 0;

void gpioExpLoopbackRequest()
{
    s_lbPending = true;
}

static uint8_t lbApply(uint8_t v, const uint8_t *map)
{
    uint8_t r = 0;
    for (uint8_t i = 0; i < 8; i++)
        if (v & (1u << i)) r |= (uint8_t)(1u << map[i]);
    return r;
}

/* The only sanctioned way to change which port drives. See the HAZARD note. */
static void lbSetDriver(GpioExpPort driver, uint8_t initial)
{
    gpioExpSetDirection(PORT_0, 0xFF);
    gpioExpSetDirection(PORT_1, 0xFF);
    gpioExpWrite(driver, initial);
    gpioExpSetDirection(driver, 0x00);
}

/* Read both inputs and resync the shadows, which also clears nINT on both
 * ports - the standard "start from a known quiet state" step between phases. */
static void lbQuiesce()
{
    uint16_t v;
    gpioExpReadBoth(&v);
    s_intPending = false;
}

/* Sustained read rate, mirroring runBench() on the write side. */
static void lbBenchRead(Bench *b, uint32_t hz, uint32_t ms, int path)
{
    Wire.setClock(hz);
    uint32_t n = 0, err = 0;
    uint8_t in[2];
    uint16_t v;
    uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < ms * 1000UL) {
        bool ok;
        if (path == 0) {                      /* one port, one transaction */
            ok = readReg(REG_INPUT_1) >= 0;
        } else if (path == 1) {               /* paired read + errata park */
            ok = gpioExpReadBoth(&v);
        } else {                              /* paired read, no park */
            ok = readInputsFast(in);
        }
        if (!ok) err++;
        n++;
    }
    b->elapsed_us = (uint32_t)(micros() - t0);
    b->writes = n;
    b->errors = err;
}

/* Lightweight stand-in for the application callback during phase F. The real
 * one logs over Serial, which at 115200 would cost milliseconds and swamp the
 * measurement. Timestamp is taken before anything else. */
static volatile uint32_t s_lbCbUs = 0;
static volatile uint8_t  s_lbCbValue = 0;
static volatile bool     s_lbCbFired = false;

static void lbCallback(GpioExpPort port, uint8_t value, uint8_t changed)
{
    uint32_t now = micros();
    (void)changed;
    if (port != PORT_1) return;
    s_lbCbUs    = now;
    s_lbCbValue = value;
    s_lbCbFired = true;
}

void gpioExpLoopbackPump()
{
    if (!s_lbPending) return;
    s_lbPending = false;

    uint32_t tStart = millis();
    s_lbDone = false;
    s_lbMapOk = false;
    s_lbMapIdentity = false;
    s_lbRamp01Err = s_lbRamp10Err = 0;
    s_lbRamp01Bad = s_lbRamp10Bad = -1;
    s_lbIntTimeouts = s_lbReadBad = s_lbE2ETimeouts = s_lbE2EBad = 0;
    memset(&s_lbLatWrite, 0, sizeof(s_lbLatWrite));
    memset(&s_lbLatAssert, 0, sizeof(s_lbLatAssert));
    memset(&s_lbLatRead, 0, sizeof(s_lbLatRead));
    memset(&s_lbLatTotal, 0, sizeof(s_lbLatTotal));
    memset(&s_lbLatE2E, 0, sizeof(s_lbLatE2E));

    Wire.setClock(I2C_HZ);
    GpioExpChangeCb savedCb = s_onChange;
    s_onChange = nullptr;             /* no Serial logging inside the timed parts */

    /* --- Phase A: discover the wiring ------------------------------------
     * Walking 1 over a 0 background, then walking 0 over a 1 background. A
     * correctly wired pin j answers (1<<j) to the first and ~(1<<j) to the
     * second; requiring both catches the failures a single pattern cannot.
     * An open wire is the important one: the ~100 uA input pull-up parks a
     * disconnected pin high, so it passes walking-1 and fails walking-0. */
    lbSetDriver(PORT_0, 0x00);
    delayMicroseconds(500);

    bool mapOk = true;
    for (uint8_t i = 0; i < 8; i++) {
        gpioExpWrite(PORT_0, (uint8_t)(1u << i));
        delayMicroseconds(300);
        s_lbWalkHi[i] = gpioExpRead(PORT_1);
        gpioExpWrite(PORT_0, (uint8_t)~(1u << i));
        delayMicroseconds(300);
        s_lbWalkLo[i] = gpioExpRead(PORT_1);

        uint8_t hi = s_lbWalkHi[i];
        /* Exactly one bit high, and the complement pattern agreeing, is the
         * only response a single intact wire can give. */
        if (hi == 0 || (hi & (uint8_t)(hi - 1)) != 0 ||
            s_lbWalkLo[i] != (uint8_t)~hi) {
            mapOk = false;
            continue;
        }
        uint8_t j = 0;
        while (!(hi & (1u << j))) j++;
        s_lbMap[i] = j;
    }

    if (mapOk) {
        /* A permutation must be onto as well as into: two port 0 pins shorted
         * to the same port 1 pin would otherwise pass the per-bit test. */
        uint8_t seen = 0;
        for (uint8_t i = 0; i < 8; i++) seen |= (uint8_t)(1u << s_lbMap[i]);
        if (seen != 0xFF) mapOk = false;
    }

    if (!mapOk) {
        /* No loom, or a broken one. The remaining phases would read pull-ups
         * and time out on nINT, so stop with the walking data as evidence. */
        gpioExpSetDirection(PORT_0, 0xFF);
        gpioExpSetDirection(PORT_1, 0xFF);
        s_onChange = savedCb;
        s_lbWiringNote = "no 1:1 loom detected between J24 and J25";
        s_lbTotalMs = millis() - tStart;
        s_lbDone = true;
        Serial.println("[gpioexp] loopback: wiring check FAILED - is J24 wired to J25?");
        return;
    }

    s_lbMapOk = true;
    s_lbMapIdentity = true;
    for (uint8_t i = 0; i < 8; i++) {
        s_lbInv[s_lbMap[i]] = i;
        if (s_lbMap[i] != i) s_lbMapIdentity = false;
    }
    s_lbWiringNote = s_lbMapIdentity
        ? "IO0_i -> IO1_i, header pin order cancels out as expected"
        : "1:1 but permuted - loom does not follow header pin order";

    /* --- Phase B: full ramp, both directions -----------------------------
     * 256 values each way. The reverse pass is the first time port 1 has ever
     * driven anything and the first time port 0 has been read as a true
     * input, so it is not a duplicate of the forward pass. */
    for (uint16_t v = 0; v <= 0xFF; v++) {
        gpioExpWrite(PORT_0, (uint8_t)v);
        delayMicroseconds(200);
        uint8_t got = gpioExpRead(PORT_1);
        uint8_t want = lbApply((uint8_t)v, s_lbMap);
        if (got != want) {
            if (s_lbRamp01Bad < 0) { s_lbRamp01Bad = v; s_lbRamp01Got = got; }
            s_lbRamp01Err++;
        }
    }

    lbSetDriver(PORT_1, 0x00);
    delayMicroseconds(500);
    for (uint16_t v = 0; v <= 0xFF; v++) {
        gpioExpWrite(PORT_1, (uint8_t)v);
        delayMicroseconds(200);
        uint8_t got = gpioExpRead(PORT_0);
        uint8_t want = lbApply((uint8_t)v, s_lbInv);
        if (got != want) {
            if (s_lbRamp10Bad < 0) { s_lbRamp10Bad = v; s_lbRamp10Got = got; }
            s_lbRamp10Err++;
        }
    }

    /* Back to port 0 driving for everything that follows. */
    lbSetDriver(PORT_0, 0x00);
    delayMicroseconds(500);

    /* --- Phase C: polarity inversion --------------------------------------
     * Never exercised before, because inverting a port you are driving
     * yourself proves nothing. Affects the input path only. */
    s_lbPolDriven = 0xA5;
    gpioExpWrite(PORT_0, s_lbPolDriven);
    delayMicroseconds(300);
    s_lbPolNormal = gpioExpRead(PORT_1);
    gpioExpSetPolarity(PORT_1, 0xFF);
    delayMicroseconds(300);
    s_lbPolInverted = gpioExpRead(PORT_1);
    gpioExpSetPolarity(PORT_1, 0x00);
    s_lbPolOk = (s_lbPolNormal == lbApply(s_lbPolDriven, s_lbMap)) &&
                (s_lbPolInverted == (uint8_t)~s_lbPolNormal);
    lbQuiesce();

    /* --- Phase D: nINT is cleared per port, not per chip -------------------
     * Datasheet 8.4.1: "the interrupt caused by port 0 is not cleared by a
     * read of port 1 or vice versa". Worth proving rather than trusting: if
     * it were chip-wide, an application polling only gpioExpRead(PORT_0)
     * would silently wedge nINT low the first time port 1 moved. */
    gpioExpWrite(PORT_0, 0x00);
    lbQuiesce();
    gpioExpWrite(PORT_0, 0xFF);          /* port 1 changes -> nINT asserts */
    delayMicroseconds(200);
    s_lbIntOnChange = digitalRead(PIN_nINT);
    gpioExpRead(PORT_0);                 /* the wrong port: must not clear it */
    s_lbIntAfterP0 = digitalRead(PIN_nINT);
    gpioExpRead(PORT_1);                 /* the right port: must clear it */
    delayMicroseconds(200);
    s_lbIntAfterP1 = digitalRead(PIN_nINT);
    s_lbIntPortOk = (s_lbIntOnChange == LOW) && (s_lbIntAfterP0 == LOW) &&
                    (s_lbIntAfterP1 == HIGH);
    lbQuiesce();

    /* --- Phase E: input latency, decomposed -------------------------------
     * Split into the three terms that actually cost something, so the total
     * is measured rather than summed from a datasheet figure as before:
     *   write   - call entry until the driving write returns
     *   assert  - that point until nINT is observed low
     *   read    - that point until the new input byte is in hand
     * 'assert' is expected to come out at essentially zero: the pin settles
     * within tpv of the data byte's ACK, a few microseconds before the write
     * call returns, so nINT is already low by then. That is the finding, not
     * a measurement error - it means input latency is bus time and nothing
     * else. */
    gpioExpWrite(PORT_0, 0xFF);
    lbQuiesce();
    for (int i = 0; i < LB_LAT_ITERS; i++) {
        uint8_t v = (i & 1) ? 0xFF : 0x00;
        uint8_t want = lbApply(v, s_lbMap);

        uint32_t t0 = micros();
        gpioExpWrite(PORT_0, v);
        uint32_t t1 = micros();

        while (digitalRead(PIN_nINT) != LOW &&
               (uint32_t)(micros() - t1) < LB_INT_TIMEOUT_US) { }
        uint32_t t2 = micros();
        if (digitalRead(PIN_nINT) != LOW) {
            s_lbIntTimeouts++;
            lbQuiesce();
            continue;
        }

        uint8_t in[2];
        bool ok = readInputsFast(in);
        uint32_t t3 = micros();

        latSample(&s_lbLatWrite,  t1 - t0);
        latSample(&s_lbLatAssert, t2 - t1);
        latSample(&s_lbLatRead,   t3 - t2);
        latSample(&s_lbLatTotal,  t3 - t0);
        if (!ok || in[1] != want) s_lbReadBad++;

        s_intPending = false;
        delayMicroseconds(500);
    }

    /* --- Phase F: end to end, pin edge -> application callback -------------
     * What phase E measures plus the dispatch an application actually pays:
     * the ISR flag, gpioExpService() picking it up, and the callback being
     * entered. Timed from before the stimulus write, so it includes one write
     * latency - subtract s_lbLatWrite to get the pure input side. */
    gpioExpOnChange(lbCallback);
    gpioExpWrite(PORT_0, 0xFF);
    lbQuiesce();
    for (int i = 0; i < LB_LAT_ITERS; i++) {
        uint8_t v = (i & 1) ? 0xFF : 0x00;
        uint8_t want = lbApply(v, s_lbMap);
        s_lbCbFired = false;

        uint32_t t0 = micros();
        gpioExpWrite(PORT_0, v);
        while (!s_lbCbFired && (uint32_t)(micros() - t0) < LB_INT_TIMEOUT_US * 4)
            gpioExpService();

        if (!s_lbCbFired) {
            s_lbE2ETimeouts++;
            lbQuiesce();
            continue;
        }
        latSample(&s_lbLatE2E, s_lbCbUs - t0);
        if (s_lbCbValue != want) s_lbE2EBad++;
        delayMicroseconds(500);
    }
    gpioExpOnChange(nullptr);

    /* --- Phase H: the no-park read returns the same data ------------------
     * Correctness half of the optimisation the throughput bench below times.
     * Port 0 is driving, so its own input register must read back the byte we
     * put on it, and port 1 must read the mapped image of it. */
    gpioExpWrite(PORT_0, 0x5A);
    delayMicroseconds(300);
    uint8_t fast[2] = {0, 0};
    bool fastOk = readInputsFast(fast);
    s_lbFastP0 = fast[0];
    s_lbFastP1 = fast[1];
    s_lbFastOk = fastOk && fast[0] == 0x5A && fast[1] == lbApply(0x5A, s_lbMap);
    lbQuiesce();

    /* --- Phase G: sustained read throughput ------------------------------- */
    lbBenchRead(&s_lbR8_400,   400000, 40, 0);
    lbBenchRead(&s_lbR16_400,  400000, 40, 1);
    lbBenchRead(&s_lbR16f_400, 400000, 40, 2);
    lbBenchRead(&s_lbR16_100,  100000, 40, 1);

    /* Restore everything: bus speed, both ports released, shadows resynced so
     * the application callback does not fire on a stale delta, and only then
     * the real callback back in place. */
    Wire.setClock(I2C_HZ);
    gpioExpSetDirection(PORT_0, 0xFF);
    gpioExpSetDirection(PORT_1, 0xFF);
    gpioExpSetPolarity(PORT_1, 0x00);
    delayMicroseconds(500);
    lbQuiesce();
    s_onChange = savedCb;

    s_lbTotalMs = millis() - tStart;
    s_lbDone = true;

    Serial.printf("[gpioexp] loopback: map%s ok, ramp %u+%u mismatches, "
                  "e2e avg %.0f us, %lu ms total\n",
                  s_lbMapIdentity ? " (identity)" : " (permuted)",
                  (unsigned)s_lbRamp01Err, (unsigned)s_lbRamp10Err,
                  s_lbLatE2E.n ? (float)s_lbLatE2E.sum_us / s_lbLatE2E.n : 0.0f,
                  (unsigned long)s_lbTotalMs);
}

static void latJson(char *dst, size_t n, const char *name, const Lat *l)
{
    snprintf(dst, n, "\"%s\":{\"n\":%lu,\"min\":%lu,\"avg\":%.1f,\"max\":%lu}",
             name, (unsigned long)l->n, (unsigned long)l->min_us,
             l->n ? (float)l->sum_us / l->n : 0.0f, (unsigned long)l->max_us);
}

static void readBenchJson(char *dst, size_t n, const char *name, const Bench *b)
{
    float us = b->writes ? (float)b->elapsed_us / b->writes : 0.0f;
    float hz = us > 0 ? 1e6f / us : 0.0f;
    snprintf(dst, n,
             "\"%s\":{\"reads\":%lu,\"errors\":%lu,\"us_per_read\":%.2f,"
             "\"reads_per_s\":%.0f}",
             name, (unsigned long)b->writes, (unsigned long)b->errors, us, hz);
}

String gpioExpLoopbackJson()
{
    if (!s_lbDone)
        return String("{\"done\":false}\n");

    String s;
    s.reserve(2048);
    char buf[320];

    snprintf(buf, sizeof(buf), "{\"done\":true,\"total_ms\":%lu,",
             (unsigned long)s_lbTotalMs);
    s += buf;

    /* wiring */
    s += "\"wiring\":{\"ok\":";
    s += s_lbMapOk ? "true" : "false";
    snprintf(buf, sizeof(buf), ",\"identity\":%s,\"note\":\"%s\",\"map\":[",
             s_lbMapIdentity ? "true" : "false", s_lbWiringNote);
    s += buf;
    for (uint8_t i = 0; i < 8; i++) {
        if (s_lbMapOk) snprintf(buf, sizeof(buf), "%s%u", i ? "," : "", s_lbMap[i]);
        else           snprintf(buf, sizeof(buf), "%s-1", i ? "," : "");
        s += buf;
    }
    s += "],\"walk_hi\":[";
    for (uint8_t i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "%s\"0x%02X\"", i ? "," : "", s_lbWalkHi[i]);
        s += buf;
    }
    s += "],\"walk_lo\":[";
    for (uint8_t i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "%s\"0x%02X\"", i ? "," : "", s_lbWalkLo[i]);
        s += buf;
    }
    s += "]}";

    if (!s_lbMapOk) {
        s += "}\n";
        return s;
    }

    snprintf(buf, sizeof(buf),
             ",\"ramp\":{\"p0_to_p1\":{\"steps\":256,\"mismatches\":%u,"
             "\"first_bad\":%d,\"first_bad_read\":\"0x%02X\"},"
             "\"p1_to_p0\":{\"steps\":256,\"mismatches\":%u,"
             "\"first_bad\":%d,\"first_bad_read\":\"0x%02X\"}}",
             s_lbRamp01Err, s_lbRamp01Bad, s_lbRamp01Got,
             s_lbRamp10Err, s_lbRamp10Bad, s_lbRamp10Got);
    s += buf;

    snprintf(buf, sizeof(buf),
             ",\"polarity\":{\"ok\":%s,\"driven\":\"0x%02X\","
             "\"normal\":\"0x%02X\",\"inverted\":\"0x%02X\"}",
             s_lbPolOk ? "true" : "false",
             s_lbPolDriven, s_lbPolNormal, s_lbPolInverted);
    s += buf;

    snprintf(buf, sizeof(buf),
             ",\"int_per_port\":{\"ok\":%s,\"on_change\":%d,"
             "\"after_port0_read\":%d,\"after_port1_read\":%d}",
             s_lbIntPortOk ? "true" : "false",
             s_lbIntOnChange, s_lbIntAfterP0, s_lbIntAfterP1);
    s += buf;

    char lw[128], la[128], lr[128], lt[128], le[128];
    latJson(lw, sizeof(lw), "write",      &s_lbLatWrite);
    latJson(la, sizeof(la), "int_assert", &s_lbLatAssert);
    latJson(lr, sizeof(lr), "read",       &s_lbLatRead);
    latJson(lt, sizeof(lt), "total",      &s_lbLatTotal);
    latJson(le, sizeof(le), "end_to_end", &s_lbLatE2E);
    snprintf(buf, sizeof(buf), ",\"latency_us\":{%s,%s,%s,%s,%s,",
             lw, la, lr, lt, le);
    s += buf;
    snprintf(buf, sizeof(buf),
             "\"int_timeouts\":%lu,\"read_mismatches\":%lu,"
             "\"e2e_timeouts\":%lu,\"e2e_mismatches\":%lu}",
             (unsigned long)s_lbIntTimeouts, (unsigned long)s_lbReadBad,
             (unsigned long)s_lbE2ETimeouts, (unsigned long)s_lbE2EBad);
    s += buf;

    char r1[160], r2[160], r3[160], r4[160];
    readBenchJson(r1, sizeof(r1), "read8_400k",        &s_lbR8_400);
    readBenchJson(r2, sizeof(r2), "read16_400k",       &s_lbR16_400);
    readBenchJson(r3, sizeof(r3), "read16_fast_400k",  &s_lbR16f_400);
    readBenchJson(r4, sizeof(r4), "read16_100k",       &s_lbR16_100);
    snprintf(buf, sizeof(buf), ",\"throughput\":{%s,%s,", r1, r2);
    s += buf;
    snprintf(buf, sizeof(buf), "%s,%s}", r3, r4);
    s += buf;

    snprintf(buf, sizeof(buf),
             ",\"fast_read\":{\"ok\":%s,\"port0\":\"0x%02X\",\"port1\":\"0x%02X\"}}\n",
             s_lbFastOk ? "true" : "false", s_lbFastP0, s_lbFastP1);
    s += buf;

    return s;
}
