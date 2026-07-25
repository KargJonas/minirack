/*
 * EasyOTA network bringup. Folded into the EasyOTA library so a single
 * dependency provides both "get on the network" and "stay updatable over it".
 * Kept in its own translation unit to isolate the heavy ETH.h / WiFi.h
 * includes and the interface #if.
 */
#include "EasyOTA.h"

#if defined(EASYOTA_USE_ETH)
#include <ETH.h>

void EasyOTAClass::beginNetwork()
{
    /* WT32-ETH01: LAN8720 PHY addr 1, MDC=GPIO23, MDIO=GPIO18, 50MHz clock
     * in on GPIO0 from the external oscillator, oscillator enable on GPIO16 */
    event("net-begin");
    ETH.begin(1, 16, 23, 18, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    ETH.setHostname(hostname().c_str());
    while (!ETH.linkUp() || ETH.localIP() == IPAddress()) {
        delay(250);
        Serial.print(".");
    }
    event("net-up");
    Serial.printf("\nethernet up: %s\n", ETH.localIP().toString().c_str());
}

#elif defined(EASYOTA_USE_WIFI)
#include <WiFi.h>

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

void EasyOTAClass::beginNetwork()
{
    event("net-begin");
    WiFi.mode(WIFI_STA);
    /* modem sleep makes the board miss mDNS multicast queries -> flaky
     * discovery; we're mains-powered, keep the radio awake */
    WiFi.setSleep(false);
    WiFi.setHostname(hostname().c_str());
    /* GUI-configured credentials first; fall back to the compiled-in ones so
     * a typo saved via the web form can't strand the board */
    String ssid = wifiSsid(EASYOTA_WIFI_SSID);
    String pass = wifiPass(EASYOTA_WIFI_PASS);
    while (true) {
        if (tryWifi(ssid, pass, 20000)) break;
        Serial.printf("\n'%s' failed, trying compiled-in '%s'\n",
                      ssid.c_str(), EASYOTA_WIFI_SSID);
        if (tryWifi(EASYOTA_WIFI_SSID, EASYOTA_WIFI_PASS, 20000)) break;
        Serial.println("\nstill no wifi, retrying both");
    }
    event("net-up");
    Serial.printf("\nwifi up: %s (%s)\n",
                  WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());
}

#else
#error "define EASYOTA_USE_WIFI or EASYOTA_USE_ETH"
#endif
