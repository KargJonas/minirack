#include <Arduino.h>
#include <EasyOTA.h>
#include <ESPAsyncWebServer.h>
#include <algorithm>

#include "adc.h"
#include "i2cmux.h"
#include "gpioexp.h"

#define APP_VERSION "rack-monitor built " __DATE__ " " __TIME__

struct Metrics {
    float rail12_v, rail24_v;   /* DC rail voltages  */
    float rail12_a, rail24_a;   /* DC rail currents  */
    float wall_v, wall_a, wall_w; /* wall side (CT/PT) */
};

/* Input-change latency is gated by how often loop() gets round to calling
 * gpioExpService(), so track the loop period. Ring is written from loop() and
 * read from the async task; a torn sample only perturbs one bucket. */
static const uint16_t LOOP_RING = 512;
static uint32_t s_loopRing[LOOP_RING];
static volatile uint32_t s_loopIdx = 0;   /* wraps at a multiple of LOOP_RING */
static uint32_t s_loopLast = 0;

static String loopStatsJson()
{
    uint32_t tmp[LOOP_RING];
    uint32_t idx = s_loopIdx;
    uint16_t n = idx < LOOP_RING ? (uint16_t)idx : LOOP_RING;
    if (n == 0) return String("{\"n\":0}\n");
    memcpy(tmp, s_loopRing, n * sizeof(tmp[0]));
    std::sort(tmp, tmp + n);
    uint32_t sum = 0;
    for (uint16_t i = 0; i < n; i++) sum += tmp[i];
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"n\":%u,\"loop_period_us\":{\"min\":%lu,\"med\":%lu,\"p95\":%lu,"
             "\"p99\":%lu,\"max\":%lu,\"avg\":%.1f}}\n",
             n, (unsigned long)tmp[0], (unsigned long)tmp[n / 2],
             (unsigned long)tmp[(n * 95) / 100], (unsigned long)tmp[(n * 99) / 100],
             (unsigned long)tmp[n - 1], (float)sum / n);
    return String(buf);
}

/* Bringup aid: log every input change the expander reports. */
static void onGpioExpChange(GpioExpPort port, uint8_t value, uint8_t changed)
{
    Serial.printf("[gpioexp] port%u -> 0x%02X (changed 0x%02X)\n",
                  (unsigned)port, value, changed);
}

/* TODO: replace with real ADS131M02 / ADC1 reads. */
static Metrics readMetrics()
{
    return Metrics{};
}

static String metricsJson()
{
    Metrics m = readMetrics();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"rail12_v\":%.3f,\"rail24_v\":%.3f,"
             "\"rail12_a\":%.3f,\"rail24_a\":%.3f,"
             "\"wall_v\":%.2f,\"wall_a\":%.3f,\"wall_w\":%.2f}\n",
             m.rail12_v, m.rail24_v, m.rail12_a, m.rail24_a,
             m.wall_v, m.wall_a, m.wall_w);
    return String(buf);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("\n" APP_VERSION);

    EasyOTA.beginNetwork();
    EasyOTA.begin(APP_VERSION);

    /* GPIO0 straps the ESP32 to download mode if sampled low at reset; the
     * board also feeds the 50 MHz PHY clock in on GPIO0. Park it high before
     * any OTA-triggered reboot so we always come back up running firmware. */
    EasyOTA.onReboot([]() {
        pinMode(0, OUTPUT);
        digitalWrite(0, HIGH);
    });

    adcBringupInit();
    adcTestSerial();

    i2cMuxInit();
    i2cMuxTestSerial();

    gpioExpInit();
    gpioExpOnChange(onGpioExpChange);
    gpioExpTestSerial();

    /* App routes on the shared server, registered after begin() so they can
     * never shadow the EasyOTA endpoints (handlers match in registration
     * order). */
    EasyOTA.server()->on("/metrics", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", metricsJson());
    });

    /* Re-runs the ADC self-test live on each request. */
    EasyOTA.server()->on("/adc-test", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", adcTestJson());
    });

    /* Re-runs the I2C mux scan live on each request. */
    EasyOTA.server()->on("/mux-test", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", i2cMuxTestJson());
    });

    /* Deep diagnostic for the BMP280 on mux channel 0. */
    EasyOTA.server()->on("/bmp-debug", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", bmp280DebugJson());
    });

    /* Re-runs the GPIO-expander register dump live on each request. */
    EasyOTA.server()->on("/gpio-test", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", gpioExpTestJson());
    });

    /* Arms the port-0 logic-analyzer self-test; it runs from loop() so this
     * handler never blocks the async TCP task. Results at /gpio-selftest. */
    EasyOTA.server()->on("/gpio-selftest-run", HTTP_GET, [](AsyncWebServerRequest *req) {
        gpioExpSelfTestRequest();
        req->send(200, "application/json", "{\"started\":true}\n");
    });

    EasyOTA.server()->on("/gpio-selftest", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", gpioExpSelfTestJson());
    });

    /* Port-to-port loopback: needs J24 wired 1:1 to J25. Detects the loom
     * first and stops harmlessly if it is absent, so it is safe to hit on a
     * stock board. Runs from loop(); results at /gpio-loopback. */
    EasyOTA.server()->on("/gpio-loopback-run", HTTP_GET, [](AsyncWebServerRequest *req) {
        gpioExpLoopbackRequest();
        req->send(200, "application/json", "{\"started\":true}\n");
    });

    EasyOTA.server()->on("/gpio-loopback", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", gpioExpLoopbackJson());
    });

    /* loop() period distribution - the wait term in input-change latency. */
    EasyOTA.server()->on("/loop-stats", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", loopStatsJson());
    });

    Serial.println("rack-monitor up; /metrics, /adc-test, /mux-test");
}

void loop()
{
    uint32_t now = micros();
    if (s_loopLast) s_loopRing[s_loopIdx++ % LOOP_RING] = now - s_loopLast;
    s_loopLast = now;

    EasyOTA.handle();
    gpioExpService();
    gpioExpSelfTestPump();
    gpioExpLoopbackPump();

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 10000) {
        lastBeat = millis();
        Serial.printf("[%lus] alive, " APP_VERSION "\n", millis() / 1000);
    }
}
