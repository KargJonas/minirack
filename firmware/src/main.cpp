/*
 * Minirack monitor firmware. Runs on the WT32-ETH01 on the power board and
 * exposes the rack's electrical telemetry over the network, while staying
 * OTA-updatable and inspectable through EasyOTA (everything under /easy-ota;
 * this app owns "/" and /metrics).
 *
 * Sensing (see system-design/design.md for the analog front end):
 *   - DC rail voltages (12 V, 24 V) via resistor dividers to AGND
 *   - DC rail currents via shunts + ADS131M02 (low-side, ground-referenced)
 *   - wall power via transformer-isolated CT/PT into ADC1
 * The ADS131M02 / ADC driver is not wired up yet - readMetrics() returns
 * placeholders so the plumbing (bringup, OTA, the /metrics route) can be
 * exercised on real hardware first. TODO: implement the SPI driver.
 */
#include <Arduino.h>
#include <EasyOTA.h>
#include <ESPAsyncWebServer.h> /* for app routes on EasyOTA.server() */

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

    /* App route on the shared server, registered after begin() so it can
     * never shadow the EasyOTA endpoints (handlers match in registration
     * order). */
    EasyOTA.server()->on("/metrics", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", metricsJson());
    });

    Serial.println("rack-monitor up; telemetry on /metrics");
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
