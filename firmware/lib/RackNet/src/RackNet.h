#pragma once
/*
 * RackNet. The one network bringup shared by the loader and every app.
 *
 * Build flags select the interface:
 *   -DRACK_USE_ETH    WT32-ETH01 (LAN8720 pinout baked in)
 *   -DRACK_USE_WIFI   station mode; RACK_WIFI_SSID / RACK_WIFI_PASS are the
 *                     compiled-in fallback credentials
 *
 * WiFi credentials configured via the RackOTA GUI (NVS) are tried first,
 * the compiled-in ones second - a typo saved in the web form can't strand
 * the board. The RackOTA-persisted hostname is requested via DHCP.
 */
#include <Arduino.h>

/* Bring the network up. Blocks until the board has an IP. */
void rackNetBegin();
