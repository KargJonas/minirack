#pragma once
#include <Arduino.h>

/**
 * ADS131M02 bringup. Three dual-channel ADCs (ADC1/2/3) on one shared SPI bus.
 * See adc.cpp for the pinout and framing details.
 */

/* Bring up CLKIN, SPI and release the shared reset. Call once from setup(). */
void adcBringupInit();

/* Read ID/STATUS/MODE from every ADC and report liveness. */
String adcTestJson();
void   adcTestSerial();
