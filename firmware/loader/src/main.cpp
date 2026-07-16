/*
 * minirack OTA loader
 *
 * Lives in the "loader" partition (subtype factory - what the bootloader
 * ultimately falls back to) and is only ever flashed over serial. It is
 * nothing but network bringup plus RackOTA: every endpoint it serves
 * (/, /status, /update, /boot, ...) is the exact same code the apps run,
 * defined once in lib/RackOTA. RackOTA detects it is running from the
 * factory partition and reports role "loader" in /status.
 */
#include <Arduino.h>
#include <RackOTA.h>
#include <RackNet.h>

#define LOADER_VERSION "minirack loader built " __DATE__ " " __TIME__

void setup()
{
    Serial.begin(115200);
    Serial.println("\n" LOADER_VERSION);
    rackNetBegin();
    RackOTA.begin(LOADER_VERSION);
}

void loop()
{
    RackOTA.handle();
}
