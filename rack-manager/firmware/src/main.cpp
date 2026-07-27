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

/* Raw-stream listen port, in NVS so it survives an OTA and can be changed
 * without a flash. 0 means sample but do not stream. The collector finds
 * the board by browsing _easyota._tcp and dials in. */
static const char *NVS_NAMESPACE = "rackmon";

static Preferences prefs;

static String streamStatusJson()
{
    AdcStreamStats s = adcStreamGetStats();

    char buf[352];
    snprintf(buf, sizeof(buf),
             "{\"session\":%u,\"listening\":%s,\"port\":%u,\"connected\":%s,"
             "\"collector\":\"%s\",\"connects\":%u,\"drops\":%u,"
             "\"last_fail\":\"%s\",\"last_errno\":%d,"
             "\"packets\":%u,\"frames_sampled\":%llu,\"frames_sent\":%llu,"
             "\"frames_lost\":%llu}",
             (unsigned)s.session, s.listening ? "true" : "false",
             (unsigned)s.port, s.connected ? "true" : "false", s.peer,
             (unsigned)s.connects, (unsigned)s.drops,
             adcStreamFailName(s.lastFailStage),
             s.lastErrno, (unsigned)s.packets,
             (unsigned long long)s.framesSampled,
             (unsigned long long)s.framesSent,
             (unsigned long long)s.framesLost);
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
     * these would answer /stream/hello and /stream/port as well. */

    /* The handshake exactly as the collector receives it, so the board can be
     * asked what it thinks its own ADC configuration is without standing a
     * collector up first. */
    EasyOTA.server()->on("/stream/hello", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", adcStreamHelloJson());
    });

    /* Move the stream to another port, or turn it off with port=0. Takes
     * effect on the next boot: the pusher binds once for the life of its task,
     * and rebinding underneath a live collector buys nothing that a reboot
     * does not. Rarely needed - the default is the whole point. */
    EasyOTA.server()->on("/stream/port", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("port")) {
            req->send(400, "text/plain", "want ?port=<n>  (0 disables)\n");
            return;
        }
        long port = req->getParam("port")->value().toInt();
        if (port < 0 || port > 65535) {
            req->send(400, "text/plain", "port out of range\n");
            return;
        }

        prefs.putUShort("stream_port", (uint16_t)port);
        req->send(200, "text/plain", "saved; reboot to apply\n");
    });

    /* General route last, for the reason above. */
    EasyOTA.server()->on("/stream", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", streamStatusJson());
    });

    adcStreamBegin(prefs.getUShort("stream_port", ADC_STREAM_PORT));

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
