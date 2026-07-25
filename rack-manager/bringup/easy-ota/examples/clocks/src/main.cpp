/*
 * Clock-test demo app. This outputs hardware clocks (LEDC, 50% duty) on the pins
 * currently wired to the logic analyzer:
 *
 *   LA ch1  GPIO0   1 kHz    (strapping pin: parked high before OTA reboots)
 *   LA ch2  GPIO4   10 kHz
 *   LA ch3  GPIO5   100 kHz
 *   LA ch4  GPIO2   1 MHz
 *
 * Stays OTA-updatable via EasyOTA (curl --data-binary @firmware.bin .../easy-ota/update).
 */
#include <Arduino.h>
#include <EasyOTA.h>
#include <ESPAsyncWebServer.h> /* for app routes on EasyOTA.server() */
#ifdef WD_TEST
#include <WiFi.h>
#endif

#define APP_VERSION "clock-test built " __DATE__ " " __TIME__

/* Distinct frequencies need distinct LEDC timers; channels n and n+1 share
 * timer n/2, so use even channels only. Resolution must satisfy
 * freq * 2^bits <= 80MHz. */
static void startClock(uint8_t channel, uint8_t pin, uint32_t freq, uint8_t bits)
{
    ledcSetup(channel, freq, bits);
    ledcAttachPin(pin, channel);
    ledcWrite(channel, 1 << (bits - 1)); /* 50% duty */
}

void setup()
{
    Serial.begin(115200);
    Serial.println("\n" APP_VERSION);

    EasyOTA.beginNetwork();

#ifdef CRASH_TEST
    /* rollback drill: die before EasyOTA.begin() can validate the image -
     * after network bringup, so the crash timeline has something to tell */
    Serial.println("CRASH_TEST: aborting before validation");
    delay(200);
    abort();
#endif

    EasyOTA.begin(APP_VERSION);

#ifdef WD_TEST
    /* watchdog drill: a validated image whose server is unreachable - the
     * reachability watchdog must reboot once, then roll back */
    Serial.println("WD_TEST: killing the network after validation");
    WiFi.mode(WIFI_OFF);
#endif
    /* GPIO0 straps to download mode if sampled low during a reboot */
    EasyOTA.onReboot([]() {
        ledcDetachPin(0);
        pinMode(0, OUTPUT);
        digitalWrite(0, HIGH);
    });

    startClock(0, 0, 1000, 10);    /* 1 kHz   */
    startClock(2, 4, 10000, 10);   /* 10 kHz  */
    startClock(4, 5, 100000, 8);   /* 100 kHz */
    startClock(6, 2, 1000000, 4);  /* 1 MHz   */
    Serial.println("clocks running: GPIO0=1k GPIO4=10k GPIO5=100k GPIO2=1M");

    /* App-specific route on the shared server. Registered after
     * EasyOTA.begin(), so it can never shadow the EasyOTA endpoints
     * (handlers match in registration order). */
    EasyOTA.server()->on("/clocks", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json",
                  "{\"gpio0\":1000,\"gpio4\":10000,\"gpio5\":100000,"
                  "\"gpio2\":1000000}\n");
    });
}

void loop()
{
    EasyOTA.handle();

#ifdef CRASH_LOOP_TEST
    /* safeguard drill: a VALIDATED image that crash-loops - the crash-loop
     * guard must roll back to the previous image after 3 attempts */
    if (millis() > 15000) {
        Serial.println("CRASH_LOOP_TEST: aborting after validation");
        delay(100);
        abort();
    }
#endif

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 10000) {
        lastBeat = millis();
        Serial.printf("[%lus] alive, " APP_VERSION "\n", millis() / 1000);
    }
}
