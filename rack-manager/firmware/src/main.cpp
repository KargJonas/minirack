#include <Arduino.h>
#include <EasyOTA.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>

#include "adc.h"
#include "adcstream.h"
#include "i2cmux.h"
#include "gpioexp.h"

#define APP_VERSION "rack-monitor built " __DATE__ " " __TIME__

struct Metrics {
    float rail12_v, rail24_v;   /* DC rail voltages  */
    float rail12_a, rail24_a;   /* DC rail currents  */
    float wall_v, wall_a, wall_w; /* wall side (CT/PT) */
};

/* Log every input change the expander reports. */
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

/* Collector address for the raw stream, in NVS so it survives an OTA and can
 * be changed without a flash. Empty host means sample but do not stream. */
static const char *NVS_NAMESPACE = "rackmon";

static Preferences prefs;

static String streamStatusJson()
{
    AdcStreamStats s = adcStreamGetStats();

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"session\":%u,\"connected\":%s,\"connects\":%u,\"drops\":%u,"
             "\"attempts\":%u,\"last_fail\":\"%s\",\"last_errno\":%d,"
             "\"packets\":%u,\"frames_sampled\":%llu,\"frames_sent\":%llu,"
             "\"frames_lost\":%llu,\"collector\":\"%s:%u\"}",
             (unsigned)s.session, s.connected ? "true" : "false",
             (unsigned)s.connects, (unsigned)s.drops,
             (unsigned)s.attempts, adcStreamFailName(s.lastFailStage),
             s.lastErrno, (unsigned)s.packets,
             (unsigned long long)s.framesSampled,
             (unsigned long long)s.framesSent,
             (unsigned long long)s.framesLost,
             prefs.getString("collector_host", "").c_str(),
             (unsigned)prefs.getUShort("collector_port", 0));
    return String(buf);
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

    /* Before the routes: the server is already accepting by this point, so a
     * handler could otherwise reach an unopened Preferences. */
    prefs.begin(NVS_NAMESPACE, false);

    adcInit();
    i2cMuxInit();

    gpioExpInit();
    gpioExpOnChange(onGpioExpChange);

    /* App routes on the shared server, registered after begin() so they can
     * never shadow the EasyOTA endpoints (handlers match in registration
     * order). */
    EasyOTA.server()->on("/metrics", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", metricsJson());
    });

    /* Subpaths first, and this is not cosmetic: a handler matches when the URL
     * merely starts with its pattern plus '/', so "/stream" registered ahead of
     * these would answer /stream/hello and /stream/collector as well. */

    /* The handshake exactly as the collector receives it, so the board can be
     * asked what it thinks its own ADC configuration is without standing a
     * collector up first. */
    EasyOTA.server()->on("/stream/hello", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", adcStreamHelloJson());
    });

    /* Retarget the stream. Takes effect on the next boot: the pusher caches
     * the address for the life of its task, and restarting it underneath a
     * live connection buys nothing that a reboot does not. */
    EasyOTA.server()->on("/stream/collector", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("host")) {
            req->send(400, "text/plain", "want ?host=<addr>&port=<n>\n");
            return;
        }
        String   host = req->getParam("host")->value();
        uint16_t port = req->hasParam("port")
                            ? (uint16_t)req->getParam("port")->value().toInt()
                            : 0;

        prefs.putString("collector_host", host);
        prefs.putUShort("collector_port", port);
        req->send(200, "text/plain", "saved; reboot to apply\n");
    });

    /* General route last, for the reason above. */
    EasyOTA.server()->on("/stream", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", streamStatusJson());
    });

    String   host = prefs.getString("collector_host", "");
    uint16_t port = prefs.getUShort("collector_port", 0);

    adcStreamBegin(host.c_str(), port);

    Serial.println("rack-monitor up; /metrics /stream /stream/hello");
}

void loop()
{
    EasyOTA.handle();
    gpioExpService();

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 10000) {
        lastBeat = millis();
        Serial.printf("[%lus] alive, " APP_VERSION "\n", millis() / 1000);
    }
}
