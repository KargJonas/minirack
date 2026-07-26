#include "adc.h"
#include <SPI.h>
#include <esp_rom_crc.h>
#include <soc/gpio_struct.h>

/* ---------------------------------------------------------------------------
 * ADS131M02 driver
 *
 * SPI framing / register map referenced from TI's ADS131M0x example code:
 *   https://github.com/TexasInstruments/precision-adc-examples
 *   (see devices/ads131m08/).
 *
 * Behavioural details cited below are from the TI datasheet kept at
 * docs/ads131m02.pdf (SBAS853A).
 *
 * Three dual-channel ADS131M02IRUKR (ADC1/2/3) hang off one shared SPI bus;
 * each has its own chip-select. Pins below are taken straight from the PCB
 * nets (eda/rack-manager, U4 = WT32-ETH01):
 *
 *   ADC1-SCLK  -> IO12      ADC1-CS -> IO4
 *   ADC1-DIN   -> IO5       ADC2-CS -> IO17
 *   ADC1-DOUT  -> IO39      ADC3-CS -> IO2
 *   ADC1-CLKIN -> IO14      ADC1-DRDY -> IO35 (only ADC1's DRDY is wired)
 *   ADC1-SYNC/RESET -> IO15 (shared, active low)
 *
 * The ADC's entire digital/SPI interface is clocked from CLKIN, so nothing
 * responds until we drive CLKIN. We generate it on IO14 with LEDC at 4 MHz
 * (validated clean in system-design/design.md). SPI runs in mode 1
 * (CPOL=0, CPHA=1), 24-bit words. A register read is two CS-framed frames:
 * frame 1 issues RREG, frame 2 clocks NULL and the value comes back in the
 * first word. Frame length = channels(2) + 2 = 4 words = 12 bytes.
 *
 * Command words and register data are 16 bits sitting at the top of a 24-bit
 * word, so they go out MSB-aligned with a zero trailing byte (datasheet
 * 9.1.6). Conversion data uses the full 24 bits.
 *
 * SYNC/RESET is one pin shared by all three chips: that, plus the common
 * CLKIN, is what keeps them phase-locked and lets ADC1's DRDY speak for all
 * three. It doubles as reset and as sync, told apart only by pulse width.
 * ------------------------------------------------------------------------- */

static const int PIN_SCLK  = 12;
static const int PIN_DIN   = 5;             // MOSI
static const int PIN_DOUT  = 39;            // MISO (input-only pin)
static const int PIN_CLKIN = 14;
static const int PIN_RESET = 15;            // shared SYNC/nRESET (active low)
static const int PIN_DRDY  = 35;            // ADC1 only (input-only pin)
static const int ADC_CS[ADC_COUNT] = {4, 17, 2};   // indexed by AdcId

/**
 * A frame is 4 words x 24 bits = 96 SCLK cycles, and at the 31.25 kSPS this
 * board is configured for there are 31.25 us in which to read all three chips
 * - 288 bits. That needs better than 9.2 Mbit/s of payload alone, so anything
 * below ~10 MHz cannot keep up. The part's own ceiling is tc(SC) >= 64 ns,
 * i.e. 15.625 MHz, which leaves 12 MHz as a working point with margin on both
 * sides rather than a marginal one.
 */
static const uint32_t SPI_SCLK  = 12000000;

/* ADC input clock (not SPI clock) */
static const uint32_t CLKIN_HZ  = 4000000;  // Clock rate of the ADC
static const int CLKIN_LEDC_CH  = 0;        // Channel of the ESPs PWM generator

/* ID reads back 22xxh on a 2-channel ADS131M02 (datasheet 8.6.1): the high
 * byte is reserved=2h then CHANCNT=2h. Anything else means the chip is absent,
 * unclocked, or not the part we think it is. */
static const uint8_t ADC_ID_HI = 0x22;

/**
 * ADS131M0x SPI opcodes. The register addresses live in adc.h.
 * This only includes the opcodes we actually use in the driver.
 *
 * RREG and WREG carry the register address in bits 12:7 and a count-minus-one
 * in bits 6:0; we only ever move one register at a time, so the count stays
 * zero. WREG_ACK is the form the device echoes back in the following frame.
 */
enum AdcOpcode : uint16_t {
    OPCODE_NULL     = 0x0000,
    OPCODE_RREG     = 0xA000,
    OPCODE_WREG     = 0x6000,
    OPCODE_WREG_ACK = 0x4000,
};

/* CLOCK register fields (datasheet 8.6.4, reset 030Eh). */
enum : uint16_t {
    CLOCK_CH0_EN  = 1u << 8,
    CLOCK_CH1_EN  = 1u << 9,
    CLOCK_TBM     = 1u << 5,   // TurboMode: OSR 64, overriding OSR[2:0]
    CLOCK_PWR_HR  = 0x0002,    // high-resolution power mode
};
static const uint8_t CLOCK_OSR_POS = 2;

/* CFG register fields (datasheet 8.6.7, reset 0600h). */
enum : uint16_t {
    CFG_GC_EN      = 1u << 8,
    CFG_GC_DLY_DEF = 0x0600,   // GC_DLY[3:0] = 0011b, the reset value
};

/* GAIN1 holds both channels: PGAGAIN0 in bits 2:0, PGAGAIN1 in bits 6:4. */
static const uint8_t GAIN1_CH1_POS = 4;

static const uint8_t  ADC_FRAME_WORDS = 4;  // 2 channels + status + crc
static const uint8_t  ADC_WORD_BYTES  = 3;  // 24-bit word length
static const uint8_t  ADC_FRAME_BYTES = ADC_FRAME_WORDS * ADC_WORD_BYTES;

/* The frame CRC covers every word ahead of the CRC word itself. */
static const uint8_t  ADC_CRC_BYTES = (ADC_FRAME_WORDS - 1) * ADC_WORD_BYTES;

/* Full-scale range at gain 1 (datasheet 8.3: FSR = +/-1.2 V / gain), and the
 * +/-2^23 span of a 24-bit two's-complement code. */
static const float ADC_FSR_V = 1.2f;
static const float ADC_FULL_SCALE_CODES = 8388608.0f;

/**
 * VSPI is one of the two SPI interfaces of the ESP32.
 * It corresponds to pins 5 (MOSI), 12 (SCLK) and 39 (MISO).
 */
static SPIClass adcSpi(VSPI);
static const SPISettings ADC_SPI_CFG(SPI_SCLK, MSBFIRST, SPI_MODE1);

static bool     s_online[ADC_COUNT]    = {false, false, false};
static uint32_t s_crcErrors[ADC_COUNT] = {0, 0, 0};

/* Gain shadow, so adcVolts() can scale without a bus round-trip. Seeded with
 * the GAIN1 reset value. */
static AdcGain s_gain[ADC_COUNT][ADC_CHANNELS] = {
    {ADC_GAIN_1, ADC_GAIN_1},
    {ADC_GAIN_1, ADC_GAIN_1},
    {ADC_GAIN_1, ADC_GAIN_1},
};

/* OSR and chop shadows, for the same reason and with the same caveat: they
 * track what we last wrote, not what the chip currently holds. Seeded with the
 * CLOCK and CFG reset values (OSR 1024, chop off). */
static AdcOsr s_osr[ADC_COUNT]  = {ADC_OSR_1024, ADC_OSR_1024, ADC_OSR_1024};
static bool   s_chop[ADC_COUNT] = {false, false, false};

/* Chip-select bit masks for the direct GPIO path below. All three CS pins are
 * under 32, so one register pair covers them. */
static uint32_t s_csMask[ADC_COUNT] = {0, 0, 0};

/**
 * digitalWrite() costs over a microsecond apiece on this core, and a frame
 * pays for two of them. The set/clear registers drop that to a single store
 * with no read-modify-write and no re-derivation of the pin's port.
 */
static inline void csAssert(uint8_t adc)  { GPIO.out_w1tc = s_csMask[adc]; }
static inline void csRelease(uint8_t adc) { GPIO.out_w1ts = s_csMask[adc]; }

/**
 * One CS-framed transfer of a whole 4-word frame, full duplex. 'tx' supplies
 * the leading words, MSB-aligned in their 24-bit slots and the rest sent as
 * zeros; 'buf' carries the received frame back as raw bytes.
 *
 * The caller owns the SPI transaction, so a burst across all three chips pays
 * the bus setup once instead of three times. Bytes rather than unpacked words
 * because the CRC has to see the frame exactly as it arrived.
 *
 * The frame always runs full length. Short frames are only legal while the
 * ADCs are disabled (datasheet 8.5.1.11), and leaving conversion data unread
 * is exactly what fills the output FIFO and makes DRDY misbehave.
 */
static void adcTransferFrame(uint8_t adc, const uint16_t *tx, uint8_t txWords,
                             uint8_t buf[ADC_FRAME_BYTES])
{
    memset(buf, 0, ADC_FRAME_BYTES);

    for (uint8_t i = 0; i < txWords && i < ADC_FRAME_WORDS; i++) {
        buf[i * ADC_WORD_BYTES]     = (uint8_t)(tx[i] >> 8);
        buf[i * ADC_WORD_BYTES + 1] = (uint8_t)(tx[i] & 0xFF);
        // third byte stays zero: the payload is MSB-aligned in the 24-bit word
    }

    csAssert(adc);
    adcSpi.transfer(buf, ADC_FRAME_BYTES);  // in-place: buf now holds RX
    csRelease(adc);
}

/* Command responses and register values are the top 16 bits of their word. */
static inline uint16_t frameWord16(const uint8_t *buf, uint8_t word)
{
    const uint8_t *w = &buf[word * ADC_WORD_BYTES];
    return (uint16_t)((uint16_t)w[0] << 8 | w[1]);
}

/* Conversion data uses all 24 bits of its word. */
static inline uint32_t frameWord24(const uint8_t *buf, uint8_t word)
{
    const uint8_t *w = &buf[word * ADC_WORD_BYTES];
    return ((uint32_t)w[0] << 16) | ((uint32_t)w[1] << 8) | w[2];
}

/**
 * CCITT-16, polynomial 1021h, seed FFFFh (datasheet 8.3.12), over every bit of
 * every frame word ahead of the CRC word - padding included, which is why this
 * runs on whole 24-bit words rather than the 16-bit payloads inside them.
 *
 * The ESP32 has no CRC peripheral, but its mask ROM carries a table-driven
 * implementation, so this costs no flash and no table of our own. The header
 * spells out our exact variant: CCITT-FALSE is crc16_be seeded with ~0xffff
 * and inverted on the way out.
 */
static inline uint16_t crc16Ccitt(const uint8_t *data, uint8_t len)
{
    return (uint16_t)~esp_rom_crc16_be((uint16_t)~0xFFFF, data, len);
}

/* Conversion codes arrive as 24-bit two's complement. */
static int32_t signExtend24(uint32_t raw)
{
    return (raw & 0x800000u) ? (int32_t)(raw | 0xFF000000u) : (int32_t)raw;
}

bool adcReadRegister(AdcId adc, AdcRegister address, uint16_t *value)
{
    if (adc >= ADC_COUNT) return false;   // guards a cast-in index

    const uint16_t cmd = OPCODE_RREG | ((uint16_t)address << 7);
    uint8_t buf[ADC_FRAME_BYTES];

    adcSpi.beginTransaction(ADC_SPI_CFG);
    adcTransferFrame(adc, &cmd, 1, buf);
    delayMicroseconds(5);
    adcTransferFrame(adc, nullptr, 0, buf);
    adcSpi.endTransaction();

    if (value) *value = frameWord16(buf, 0);
    return true;
}

bool adcWriteRegister(AdcId adc, AdcRegister address, uint16_t value)
{
    if (adc >= ADC_COUNT) return false;

    /* Command word then the data word, both in the same frame (8.5.1.10.8). */
    const uint16_t tx[2] = {(uint16_t)(OPCODE_WREG | ((uint16_t)address << 7)), value};
    uint8_t buf[ADC_FRAME_BYTES];

    adcSpi.beginTransaction(ADC_SPI_CFG);
    adcTransferFrame(adc, tx, 2, buf);
    delayMicroseconds(5);
    adcTransferFrame(adc, nullptr, 0, buf);
    adcSpi.endTransaction();

    /* The echo carries 010b in bits 15:13 where the command had 011b, the
     * address back in bits 12:7, and the number of registers actually written
     * minus one in bits 6:0 - zero for our single-register writes. */
    const uint16_t expect = OPCODE_WREG_ACK | ((uint16_t)address << 7);
    return frameWord16(buf, 0) == expect;
}

bool adcProbe(AdcId adc)
{
    if (adc >= ADC_COUNT) return false;

    uint16_t id = 0;
    s_online[adc] = adcReadRegister(adc, ADC_REG_ID, &id) && (id >> 8) == ADC_ID_HI;
    return s_online[adc];
}

bool adcOnline(AdcId adc)
{
    return adc < ADC_COUNT && s_online[adc];
}

bool adcSetGain(AdcId adc, AdcChannel channel, AdcGain gain)
{
    if (adc >= ADC_COUNT || channel >= ADC_CHANNELS || gain > ADC_GAIN_128)
        return false;

    /* Both channels share GAIN1, so the other one comes from the shadow rather
     * than a read-back. */
    AdcGain ch0 = (channel == ADC_CH0) ? gain : s_gain[adc][ADC_CH0];
    AdcGain ch1 = (channel == ADC_CH1) ? gain : s_gain[adc][ADC_CH1];

    if (!adcWriteRegister(adc, ADC_REG_GAIN1,
                          (uint16_t)((ch1 << GAIN1_CH1_POS) | ch0)))
        return false;

    s_gain[adc][channel] = gain;
    return true;
}

bool adcSetChop(AdcId adc, bool enable)
{
    if (adc >= ADC_COUNT) return false;

    if (!adcWriteRegister(adc, ADC_REG_CFG,
                          enable ? (CFG_GC_DLY_DEF | CFG_GC_EN) : CFG_GC_DLY_DEF))
        return false;

    s_chop[adc] = enable;
    return true;
}

bool adcSetOsr(AdcId adc, AdcOsr osr)
{
    if (adc >= ADC_COUNT || osr > ADC_OSR_16256) return false;

    /* The OSR shares CLOCK with the channel enables and the power mode, so
     * they are rewritten here rather than left to a read-modify-write. */
    uint16_t clock = CLOCK_CH0_EN | CLOCK_CH1_EN | CLOCK_PWR_HR;

    if (osr == ADC_OSR_64)
        clock |= CLOCK_TBM;
    else
        clock |= (uint16_t)((osr - 1) & 0x7) << CLOCK_OSR_POS;

    if (!adcWriteRegister(adc, ADC_REG_CLOCK, clock)) return false;

    s_osr[adc] = osr;
    return true;
}

AdcGain adcGetGain(AdcId adc, AdcChannel channel)
{
    if (adc >= ADC_COUNT || channel >= ADC_CHANNELS) return ADC_GAIN_1;
    return s_gain[adc][channel];
}

AdcOsr adcGetOsr(AdcId adc)
{
    return adc < ADC_COUNT ? s_osr[adc] : ADC_OSR_1024;
}

bool adcGetChop(AdcId adc)
{
    return adc < ADC_COUNT && s_chop[adc];
}

uint8_t adcGetChopDelay(AdcId adc)
{
    (void)adc;   // one value for every chip; adcSetChop() never varies it
    return (uint8_t)((CFG_GC_DLY_DEF >> 9) & 0xF);
}

uint16_t adcGainMultiplier(AdcGain gain)
{
    return gain <= ADC_GAIN_128 ? (uint16_t)(1u << gain) : 1;
}

uint16_t adcOsrRatio(AdcOsr osr)
{
    /* OSR 64 is the TurboMode bit rather than an OSR[2:0] code, and the top of
     * the range is 16256 rather than the 16384 the doubling would give. */
    static const uint16_t RATIO[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16256};
    return osr <= ADC_OSR_16256 ? RATIO[osr] : 0;
}

uint32_t adcClkinHz(void)
{
    return CLKIN_HZ;
}

float adcFsrVolts(void)
{
    return ADC_FSR_V;
}

int adcDrdyPin(void)
{
    return PIN_DRDY;
}

bool adcDataReady()
{
    return digitalRead(PIN_DRDY) == LOW;
}

/* Decode a received frame into a sample. The CRC runs straight over the bytes
 * as they arrived - the covered words are the frame's own leading bytes, so
 * there is nothing to re-serialise. */
static void adcDecodeFrame(uint8_t adc, const uint8_t *buf, AdcSample *out)
{
    out->status = frameWord16(buf, 0);
    for (uint8_t c = 0; c < ADC_CHANNELS; c++)
        out->ch[c] = signExtend24(frameWord24(buf, 1 + c));

    out->crcOk = crc16Ccitt(buf, ADC_CRC_BYTES)
                 == frameWord16(buf, ADC_FRAME_WORDS - 1);
    if (!out->crcOk) s_crcErrors[adc]++;
}

bool adcReadSample(AdcId adc, AdcSample *out)
{
    if (adc >= ADC_COUNT || !out) return false;

    uint8_t buf[ADC_FRAME_BYTES];

    adcSpi.beginTransaction(ADC_SPI_CFG);
    adcTransferFrame(adc, nullptr, 0, buf);
    adcSpi.endTransaction();

    adcDecodeFrame(adc, buf, out);
    return true;
}

bool adcReadSampleAll(AdcSample out[ADC_COUNT])
{
    if (!out) return false;

    uint8_t buf[ADC_COUNT][ADC_FRAME_BYTES];

    /* One transaction for all three. Decoding waits until the bus work is
     * done so the chips are read as close together as the CS lines allow. */
    adcSpi.beginTransaction(ADC_SPI_CFG);
    for (uint8_t i = 0; i < ADC_COUNT; i++)
        adcTransferFrame(i, nullptr, 0, buf[i]);
    adcSpi.endTransaction();

    for (uint8_t i = 0; i < ADC_COUNT; i++)
        adcDecodeFrame(i, buf[i], &out[i]);

    return true;
}

uint32_t adcCrcErrors(AdcId adc)
{
    return adc < ADC_COUNT ? s_crcErrors[adc] : 0;
}

float adcVolts(AdcId adc, AdcChannel channel, int32_t code)
{
    if (adc >= ADC_COUNT || channel >= ADC_CHANNELS) return NAN;

    float fsr = ADC_FSR_V / (float)(1u << s_gain[adc][channel]);
    return (float)code * fsr / ADC_FULL_SCALE_CODES;
}

void adcSync()
{
    /* tw(SYL) is 1..2047 tCLKIN; 2048 or more would be read as a reset. At
     * 4 MHz CLKIN one period is 250 ns, so 10 us sits at 40 periods - clear of
     * the lower bound and nowhere near the upper one. */
    digitalWrite(PIN_RESET, LOW);
    delayMicroseconds(10);
    digitalWrite(PIN_RESET, HIGH);
    delayMicroseconds(50);
}

/**
 * Hold the shared SYNC/nRESET low long enough to force a RESET
 * (>2048 tCLKIN, ~512 us at 4 MHz), then let all three chips come back up.
 */
static void adcResetAll()
{
    digitalWrite(PIN_RESET, LOW);
    delay(5);
    digitalWrite(PIN_RESET, HIGH);
    delay(5);
}

/**
 * The per-chip settings from system-design/design.md. Gains follow the front
 * ends: everything is scaled to fill ~1 V at gain 1 except the 12V rail shunt,
 * whose 25 mV at full load needs gain 32 to recover the SNR that an amplifier
 * would otherwise have provided.
 *
 * Chop is on everywhere. It costs a lot of throughput - the period becomes
 * tGC_DLY + 3 x OSR x tMOD, so 104 us at OSR 64, i.e. 9.6 kSPS rather than the
 * 31.25 kSPS an unchopped chip manages - but it is the only mechanism that
 * cancels the ADC's own offset drift as the rack heats up. Everything else it
 * provides can be had in firmware; that cannot.
 *
 * Enabling it uniformly also makes all three chips share one conversion
 * period, which is what lets ADC1's DRDY speak for the other two. Chop at
 * 31.25 kSPS is not reachable at any legal clock: 3 x OSR bottoms out at 192
 * tMOD, and CLKIN cannot exceed ~10.2 MHz (tw(CLH)/tw(CLL) >= 49 ns).
 *
 * 9.6 kSPS is 192 samples per 50 Hz cycle with Nyquist at the 96th harmonic,
 * comfortably past the 50th that IEC 61000-4-7 asks for. This is a power
 * meter, not an oscilloscope.
 */
static void adcApplyBoardConfig()
{
    struct ChipConfig {
        AdcId   id;
        AdcGain gain[ADC_CHANNELS];
        bool    chop;
    };

    static const ChipConfig CONFIG[ADC_COUNT] = {
        {ADC1, {ADC_GAIN_1, ADC_GAIN_1},  true},   // wall current / wall voltage
        {ADC2, {ADC_GAIN_1, ADC_GAIN_32}, true},   // 12V voltage / 12V shunt
        {ADC3, {ADC_GAIN_1, ADC_GAIN_1},  true},   // 24V voltage / unused
    };

    for (const ChipConfig &c : CONFIG) {
        if (!adcOnline(c.id)) continue;   // nothing to configure on a dead chip

        bool ok = adcSetOsr(c.id, ADC_OSR_64);
        ok = adcSetChop(c.id, c.chop) && ok;
        ok = adcSetGain(c.id, ADC_CH0, c.gain[ADC_CH0]) && ok;
        ok = adcSetGain(c.id, ADC_CH1, c.gain[ADC_CH1]) && ok;

        /* A chip that answered its ID but will not take its configuration is
         * running at defaults - wrong gain, wrong rate - so its samples would
         * be quietly wrong rather than absent. Drop it instead. */
        if (!ok) s_online[c.id] = false;
    }
}

void adcInit()
{
    for (uint8_t i = 0; i < ADC_COUNT; i++) {
        pinMode(ADC_CS[i], OUTPUT);
        digitalWrite(ADC_CS[i], HIGH);
        s_csMask[i] = 1u << ADC_CS[i];   // all three pins are below 32
    }

    pinMode(PIN_RESET, OUTPUT);
    digitalWrite(PIN_RESET, HIGH);
    pinMode(PIN_DRDY, INPUT);

    // CLKIN must be live before the ADCs will talk.
    ledcSetup(CLKIN_LEDC_CH, CLKIN_HZ, 1);   // 1-bit res -> 50% square wave
    ledcAttachPin(PIN_CLKIN, CLKIN_LEDC_CH);
    ledcWrite(CLKIN_LEDC_CH, 1);
    delay(10);

    adcSpi.begin(PIN_SCLK, PIN_DOUT, PIN_DIN, -1);  // CS handled manually

    adcResetAll();

    for (uint8_t i = 0; i < ADC_COUNT; i++)
        adcProbe((AdcId)i);

    adcApplyBoardConfig();

    /* Conversions were already streaming while we configured, so the output
     * FIFOs hold stale samples. Realign all three before anyone reads. */
    adcSync();
}
