/*
 * Base image
 *
 * The image serial-flashed into ota_0 when a board is first commissioned:
 * nothing but network bringup plus EasyOTA, so the board is reachable and
 * flashable over HTTP from day one. After that it is an ordinary app -
 * later uploads overwrite it like any other slot content.
 */
#include <Arduino.h>
#include <EasyOTA.h>

#define BASE_VERSION "base image built " __DATE__ " " __TIME__

void setup()
{
    Serial.begin(115200);
    Serial.println("\n" BASE_VERSION);
    EasyOTA.beginNetwork();
    EasyOTA.begin(BASE_VERSION);
}

void loop()
{
    EasyOTA.handle();
}
