#include <Arduino.h>
#include <EasyOTA.h>
#include <ESPAsyncWebServer.h>

#include "adc.h"
#include "i2cmux.h"
#include "gpioext.h"

#define APP_VERSION "rack-monitor built " __DATE__ " " __TIME__

struct Metrics {
    float rail12_v, rail24_v;   /* DC rail voltages  */
    float rail12_a, rail24_a;   /* DC rail currents  */
    float wall_v, wall_a, wall_w; /* wall side (CT/PT) */
};

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

    gpioExtInit();
    gpioExtTestSerial();

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
        req->send(200, "application/json", gpioExtTestJson());
    });

    Serial.println("rack-monitor up; /metrics, /adc-test, /mux-test");
}

void loop()
{
    EasyOTA.handle();

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 10000) {
        lastBeat = millis();
        Serial.printf("[%lus] alive, " APP_VERSION "\n", millis() / 1000);
    }
}
