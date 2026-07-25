#include "adc.h"
#include <SPI.h>

/* ---------------------------------------------------------------------------
 * ADS131M02 bringup
 *
 * SPI framing / register map referenced from TI's ADS131M0x example code:
 *   https://github.com/TexasInstruments/precision-adc-examples
 *   (see devices/ads131m08/).
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
 * ------------------------------------------------------------------------- */

static const int PIN_SCLK  = 12;
static const int PIN_DIN   = 5;             // MOSI
static const int PIN_DOUT  = 39;            // MISO (input-only pin)
static const int PIN_CLKIN = 14;
static const int PIN_RESET = 15;            // shared SYNC/nRESET (active low)
static const int PIN_DRDY  = 35;            // ADC1 only (input-only pin)
static const int ADC_CS[3] = {4, 17, 2};    // ADC1, ADC2, ADC3
static const int SPI_SCLK  = 1000000;       // SPI clock rate

/* ADC input clock (not SPI clock) */
static const int CLKIN_HZ  = 4000000;       // Clock rate of the ADC 
static const int CLKIN_LEDC_CH = 0;         // Channel of the ESPs PWM generator 

/* ADS131M0x SPI opcodes/registers */
static const uint16_t OPCODE_NULL = 0x0000;
static const uint16_t OPCODE_RREG = 0xA000;
static const uint8_t  REG_ID      = 0x00;
static const uint8_t  REG_STATUS  = 0x01;
static const uint8_t  REG_MODE    = 0x02;

static const uint8_t  ADC_FRAME_WORDS = 4;  // 2 channels + status + crc
static const uint8_t  ADC_WORD_BYTES  = 3;  // 24-bit word length
static const uint8_t  ADC_FRAME_BYTES = ADC_FRAME_WORDS * ADC_WORD_BYTES;

/**
 * VSPI is one of the two SPI interfaces of the ESP32.
 * It corresponds to pins 5 (MOSI), 12 (SCLK) and 39 (MISO).
 */
static SPIClass adcSpi(VSPI);
static const SPISettings ADC_SPI_CFG(SPI_SCLK, MSBFIRST, SPI_MODE1);

/**
 * One CS-framed 24-bit-word frame, full duplex. The RREG response lands in
 * the first word of the *following* frame, so callers issue two frames.
 */
static uint16_t adcTransferFrame(int csPin, uint16_t firstWord)
{
    uint8_t buf[ADC_FRAME_BYTES] = {0};
    buf[0] = firstWord >> 8;
    buf[1] = firstWord & 0xFF;

    adcSpi.beginTransaction(ADC_SPI_CFG);
    digitalWrite(csPin, LOW);
    adcSpi.transfer(buf, ADC_FRAME_BYTES);  // in-place: buf now holds RX
    digitalWrite(csPin, HIGH);
    adcSpi.endTransaction();

    return (uint16_t)(buf[0] << 8) | buf[1];
}

static uint16_t adcReadRegister(int csPin, uint8_t address)
{
    adcTransferFrame(csPin, OPCODE_RREG | ((uint16_t)address << 7));
    delayMicroseconds(5);
    return adcTransferFrame(csPin, OPCODE_NULL);
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

void adcBringupInit()
{
    for (int i = 0; i < 3; i++) {
        pinMode(ADC_CS[i], OUTPUT);
        digitalWrite(ADC_CS[i], HIGH);
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
}

/**
 * Read ID/STATUS/MODE from every ADC and judge liveness.
 * A 2-channel ADS131M02 reports ID high byte 0x22 (reserved=0x2, CHANCNT=0x2).
 */
String adcTestJson()
{
    String out = "{\"clkin_hz\":";
    out += CLKIN_HZ;
    out += ",\"adcs\":[";

    for (int i = 0; i < 3; i++) {
        uint16_t id     = adcReadRegister(ADC_CS[i], REG_ID);
        uint16_t status = adcReadRegister(ADC_CS[i], REG_STATUS);
        uint16_t mode   = adcReadRegister(ADC_CS[i], REG_MODE);
        bool ok = (id >> 8) == 0x22;

        char buf[192];
        snprintf(buf, sizeof(buf),
            "%s{\"name\":\"ADC%d\",\"cs\":%d,\"id\":\"0x%04X\","
            "\"status\":\"0x%04X\",\"mode\":\"0x%04X\","
            "\"chancnt\":%u,\"alive\":%s}",
            i ? "," : "", i + 1, ADC_CS[i], id, status, mode,
            (unsigned)((id >> 8) & 0x0F), ok ? "true" : "false");
        
        out += buf;
    }

    out += "]}\n";
    return out;
}

void adcTestSerial()
{
    Serial.println("[adc] bringup self-test (reading ID @ 0x00):");
    for (int i = 0; i < 3; i++) {
        uint16_t id     = adcReadRegister(ADC_CS[i], REG_ID);
        uint16_t status = adcReadRegister(ADC_CS[i], REG_STATUS);
        uint16_t mode   = adcReadRegister(ADC_CS[i], REG_MODE);
        bool ok = (id >> 8) == 0x22;

        Serial.printf("  ADC%d (CS=IO%d): ID=0x%04X STATUS=0x%04X MODE=0x%04X -> %s\n",
            i + 1, ADC_CS[i], id, status, mode,
            ok ? "ALIVE (ADS131M02)" : "NO RESPONSE / wrong ID");
    }
}
