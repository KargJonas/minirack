/*
 * minirack clock test — outputs hardware clocks (LEDC, 50% duty) on the pins
 * currently wired to the logic analyzer:
 *
 *   LA ch1  GPIO0   1 kHz    (strapping pin: parked high before OTA reboots)
 *   LA ch2  GPIO4   10 kHz
 *   LA ch3  GPIO5   100 kHz
 *   LA ch4  GPIO2   1 MHz
 *
 * Stays OTA-updatable via RackOTA (curl --data-binary @firmware.bin .../update).
 */
#include <Arduino.h>
#include <RackOTA.h>

#if defined(APP_USE_ETH)
#include <ETH.h>
#elif defined(APP_USE_WIFI)
#include <WiFi.h>
#else
#error "define APP_USE_WIFI or APP_USE_ETH"
#endif

#define APP_VERSION "clock-test built " __DATE__ " " __TIME__

#if defined(APP_USE_WIFI)
static bool tryWifi(const String &ssid, const String &pass, uint32_t timeoutMs)
{
    Serial.printf("wifi: trying '%s' ", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (WiFi.status() == WL_CONNECTED) return true;
        delay(250);
        Serial.print(".");
    }
    WiFi.disconnect();
    return false;
}
#endif

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

#ifdef CRASH_TEST
    /* rollback drill: die before RackOTA.begin() can validate the image */
    Serial.println("CRASH_TEST: aborting before validation");
    delay(200);
    abort();
#endif

#if defined(APP_USE_ETH)
    ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    ETH.setHostname(RackOTA.hostname().c_str());
    while (!ETH.linkUp() || ETH.localIP() == IPAddress()) {
        delay(250);
        Serial.print(".");
    }
    Serial.printf("\nethernet up: %s\n", ETH.localIP().toString().c_str());
#else
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(RackOTA.hostname().c_str());
    /* GUI-configured credentials first; fall back to the compiled-in ones so
     * a typo saved via the web form can't strand the board */
    String ssid = RackOTA.wifiSsid(APP_WIFI_SSID);
    String pass = RackOTA.wifiPass(APP_WIFI_PASS);
    while (true) {
        if (tryWifi(ssid, pass, 20000)) break;
        Serial.printf("\n'%s' failed, trying compiled-in '%s'\n",
                      ssid.c_str(), APP_WIFI_SSID);
        if (tryWifi(APP_WIFI_SSID, APP_WIFI_PASS, 20000)) break;
        Serial.println("\nstill no wifi, retrying both");
    }
    Serial.printf("\nwifi up: %s (%s)\n",
                  WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
#endif

    RackOTA.begin(APP_VERSION);
    /* GPIO0 straps to download mode if sampled low during a reboot */
    RackOTA.onReboot([]() {
        ledcDetachPin(0);
        pinMode(0, OUTPUT);
        digitalWrite(0, HIGH);
    });

    startClock(0, 0, 1000, 10);    /* 1 kHz   */
    startClock(2, 4, 10000, 10);   /* 10 kHz  */
    startClock(4, 5, 100000, 8);   /* 100 kHz */
    startClock(6, 2, 1000000, 4);  /* 1 MHz   */
    Serial.println("clocks running: GPIO0=1k GPIO4=10k GPIO5=100k GPIO2=1M");
}

void loop()
{
    RackOTA.handle();

    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 10000) {
        lastBeat = millis();
        Serial.printf("[%lus] alive, " APP_VERSION "\n", millis() / 1000);
    }
}
