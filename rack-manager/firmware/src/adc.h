#pragma once
#include <Arduino.h>

/**
 * ADS131M02 driver. Three dual-channel ADCs (ADC1/2/3) on one shared SPI bus.
 * See adc.cpp for the pinout and framing details.
 */

/* Number of ADS131M02s on the bus, and channels on each. */
static const uint8_t ADC_COUNT    = 3;
static const uint8_t ADC_CHANNELS = 2;

/**
 * The three chips, named after their schematic designators. Which signal each
 * one carries is fixed by the board (system-design/design.md):
 *   ADC1 - ch0 wall current (CT), ch1 wall voltage (PT)
 *   ADC2 - ch0 12V rail voltage,  ch1 12V rail current (1 mOhm shunt, gain 32)
 *   ADC3 - ch0 24V bus voltage,   ch1 unused (bus current is derived)
 */
enum AdcId : uint8_t {
    ADC1 = 0,
    ADC2 = 1,
    ADC3 = 2,
};

enum AdcChannel : uint8_t {
    ADC_CH0 = 0,
    ADC_CH1 = 1,
};

/**
 * ADS131M0x register addresses.
 * This only includes the addresses we actually use in the driver.
 */
enum AdcRegister : uint8_t {
    ADC_REG_ID     = 0x00,
    ADC_REG_STATUS = 0x01,
    ADC_REG_MODE   = 0x02,
    ADC_REG_CLOCK  = 0x03,
    ADC_REG_GAIN1  = 0x04,
    ADC_REG_CFG    = 0x06,
};

/**
 * PGA gain. Full-scale range is +/-1.2 V divided by the gain, so the rule from
 * system-design/design.md is the highest gain where the signal still fits:
 * Imax * Rshunt < 0.9 * FSR. The enumerator value is the register code, which
 * happens to be log2 of the gain.
 */
enum AdcGain : uint8_t {
    ADC_GAIN_1   = 0,
    ADC_GAIN_2   = 1,
    ADC_GAIN_4   = 2,
    ADC_GAIN_8   = 3,
    ADC_GAIN_16  = 4,
    ADC_GAIN_32  = 5,
    ADC_GAIN_64  = 6,
    ADC_GAIN_128 = 7,
};

/**
 * Oversampling ratio. Data rate is fMOD / OSR, and fMOD is half of CLKIN, so
 * at our 4 MHz CLKIN the rates are as listed. Lower OSR trades noise for
 * bandwidth.
 *
 * The rates below are for an unchopped chip. Global-chop replaces the period
 * with tGC_DLY + 3 x OSR x tMOD, so it costs rather more than the factor of
 * two you might expect - see adcSetChop().
 *
 * OSR 64 is the CLOCK register's TurboMode bit rather than an OSR[2:0] code;
 * the driver hides that difference.
 */
enum AdcOsr : uint8_t {
    ADC_OSR_64    = 0,   /* 31.25 kSPS */
    ADC_OSR_128   = 1,   /* 15.63 kSPS */
    ADC_OSR_256   = 2,   /*  7.81 kSPS */
    ADC_OSR_512   = 3,   /*  3.91 kSPS */
    ADC_OSR_1024  = 4,   /*  1.95 kSPS - chip default */
    ADC_OSR_2048  = 5,
    ADC_OSR_4096  = 6,
    ADC_OSR_8192  = 7,
    ADC_OSR_16256 = 8,
};

/**
 * One conversion frame: both channels plus the STATUS word that arrives with
 * them. Codes are sign-extended from the wire's 24-bit two's complement, so
 * they span +/-8388608 and map onto +/-FSR. Use adcVolts() to scale one.
 */
struct AdcSample {
    int32_t  ch[ADC_CHANNELS];
    uint16_t status;
    bool     crcOk;
};

/**
 * Bring up CLKIN, SPI and the shared reset, probe all three chips and apply
 * the board configuration from system-design/design.md (gain, global-chop and
 * data rate per chip). Call once from setup().
 */
void adcInit();

/**
 * Read one 16-bit register from 'adc' into *value. This takes two SPI frames.
 * The RREG response only comes back in the frame after the request.
 *
 * Returns false only for an out-of-range 'adc'; SPI has no acknowledge, so a
 * chip that is absent or unclocked still "succeeds" here and reports whatever
 * the idle bus level is. Use adcOnline() to tell the difference.
 */
bool adcReadRegister(AdcId adc, AdcRegister address, uint16_t *value);

/**
 * Write one 16-bit register. Unlike the read, this is genuinely checked: the
 * device echoes back which register it wrote in the following frame, and a
 * mismatch returns false. That echo is the only acknowledgement the SPI
 * interface offers, so prefer this over a blind write.
 */
bool adcWriteRegister(AdcId adc, AdcRegister address, uint16_t value);

/**
 * Whether this chip is usable. Because SPI cannot report a failed
 * transaction, liveness is inferred rather than observed: adcProbe() reads the
 * ID register and checks the chip identifies as a 2-channel ADS131M02. False
 * until the first probe.
 *
 * adcInit() also clears this for a chip that answers its ID but then refuses
 * its configuration, since such a chip would sample at default gain and rate
 * and hand back plausible but wrong numbers.
 */
bool adcOnline(AdcId adc);

/* Re-read the ID register and update adcOnline(). adcInit() probes all three,
 * so call this only to re-check a chip later. */
bool adcProbe(AdcId adc);

/* Per-channel PGA gain. Also updates the scaling adcVolts() applies. */
bool adcSetGain(AdcId adc, AdcChannel channel, AdcGain gain);

/**
 * Global-chop converts each sample twice with the input polarity swapped and
 * averages the pair, which cancels the ADC's own offset and its drift. It
 * corrects nothing ahead of the chip - front-end offset flips along with the
 * signal and survives - so a zero-calibration is still needed for that.
 *
 * The cost is throughput, and more than the doubling of conversions suggests:
 * the period becomes tGC_DLY + 3 x OSR x tMOD, because reversing the polarity
 * is a step change that the sinc3 filter needs three conversion periods to
 * settle out of. At OSR 64 and the default delay that is 104 us, or 9.6 kSPS.
 *
 * The sqrt(2) noise improvement is just the averaging of two conversions and
 * can equally be had by averaging in firmware; the drift cancellation cannot.
 */
bool adcSetChop(AdcId adc, bool enable);

/*
 * Data rate. Re-enables both channels and high-resolution power mode, which
 * share the CLOCK register with the OSR.
 */
bool adcSetOsr(AdcId adc, AdcOsr osr);

/**
 * Configuration read-back, from the driver's shadow rather than the bus.
 *
 * These exist for the stream handshake (adcstream.cpp): the server is told how
 * the chips are configured and derives volts and amps itself, so nothing here
 * needs to know what is wired to which input. Shadowed rather than read back
 * because the handshake runs while the sampler is mid-stream and the SPI bus
 * is busy every 104 us.
 */
AdcGain adcGetGain(AdcId adc, AdcChannel channel);
AdcOsr  adcGetOsr(AdcId adc);
bool    adcGetChop(AdcId adc);

/* Global-chop delay, as the GC_DLY[3:0] register code. The driver pins this to
 * the reset value; it is reported because it is a term in the conversion
 * period (tGC_DLY + 3 x OSR x tMOD) and so sets the true sample rate. */
uint8_t adcGetChopDelay(AdcId adc);

/* The PGA gain as a multiplier (1..128) rather than the register code, so a
 * consumer does not need TI's encoding table. */
uint16_t adcGainMultiplier(AdcGain gain);

/* The OSR as a ratio (64..16256) rather than the register code. */
uint16_t adcOsrRatio(AdcOsr osr);

/* CLKIN, in Hz, as generated on PIN_CLKIN. Together with the OSR, the chop
 * state and the chop delay this is what determines the conversion period. */
uint32_t adcClkinHz(void);

/* Full-scale range in volts at gain 1 (datasheet 8.3: FSR = +/-1.2 V / gain).
 * The other half of the count-to-volts scale, the first being the gain. */
float adcFsrVolts(void);

/* ADC1's DRDY pin, for a consumer that wants to drive sampling from its edge
 * rather than polling adcDataReady(). */
int adcDrdyPin(void);

/**
 * True while ADC1 is holding DRDY low, meaning a conversion is waiting. Only
 * ADC1's DRDY is wired, but the shared CLKIN and SYNC/RESET phase-lock all
 * three and adcInit() gives them identical OSR and chop settings, so their
 * conversion periods are equal and this one pin gates reads of all three.
 *
 * That last part is a property of the configuration, not of the board: give
 * two chips different chop or OSR settings and their periods diverge, at which
 * point this pin speaks only for ADC1 and the others have to be told apart by
 * the DRDY bits in each frame's STATUS word (AdcSample::status).
 */
bool adcDataReady();

/**
 * Read one conversion frame. Always returns the data; 'crcOk' in the sample
 * reports whether it survived the device's CRC rather than the read failing,
 * so a corrupt frame stays visible instead of leaving a gap in the timebase.
 * Returns false only for a bad argument.
 */
bool adcReadSample(AdcId adc, AdcSample *out);

/**
 * Read a frame from every chip, indexed by AdcId. This is the path a sampler
 * wants: the three share one conversion period and one DRDY, so they are
 * always read together, and doing it under a single SPI transaction pays the
 * bus setup once instead of three times. Returns false only for a null 'out'.
 */
bool adcReadSampleAll(AdcSample out[ADC_COUNT]);

/**
 * Strobe SYNC/RESET to realign all three chips' conversions and clear their
 * two-deep output FIFOs. Needed after configuration and after any pause in
 * reading, otherwise a full FIFO makes DRDY assert every second sample
 * (datasheet 8.5.1.9.1).
 */
void adcSync();

/* Frames that failed their CRC since boot, per chip. */
uint32_t adcCrcErrors(AdcId adc);

/**
 * Scale a sample code to volts at the ADC pin, using the gain currently set
 * for that channel. Front-end scaling (dividers, shunt, CT/PT) is the
 * caller's business. Returns NAN for a bad argument.
 */
float adcVolts(AdcId adc, AdcChannel channel, int32_t code);
