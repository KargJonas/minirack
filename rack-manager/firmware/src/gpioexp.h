#pragma once
#include <Arduino.h>

/* PCA9555 16-bit GPIO expander driver (U6, address 0x20). See gpioexp.cpp for
 * the pinout, register map and datasheet references.
 *
 * The chip is byte-oriented: every I2C access moves one whole 8-bit register,
 * so all eight pins of a port are read or written together. This API mirrors
 * that directly - there is no per-pin layer. Bit n of every mask and value
 * below is pin n of the selected port; port 0 breaks out to header J24 and
 * port 1 to J25. */

enum GpioExpPort : uint8_t { PORT_0 = 0, PORT_1 = 1 };

/* Pin bit masks. Every mask and value below is byte-wide, so these OR together
 * to build one: gpioExpSetDirection(PORT_0, PIN_2 | PIN_3) leaves pins 2 and 3
 * of J24 as inputs and makes the other six outputs. Kept as loose constants
 * rather than a parameter type precisely because they are meant to be OR'd. */
enum GpioExpPin : uint8_t {
    PIN_0 = 1u << 0,
    PIN_1 = 1u << 1,
    PIN_2 = 1u << 2,
    PIN_3 = 1u << 3,
    PIN_4 = 1u << 4,
    PIN_5 = 1u << 5,
    PIN_6 = 1u << 6,
    PIN_7 = 1u << 7,
    PIN_NONE = 0x00,
    PIN_ALL  = 0xFF,
};

/* Change notification. 'value' is the port's new input byte, 'changed' carries
 * a 1 bit for every pin that differs from the previous read. Called from
 * gpioExpService(), i.e. from loop() context, so it may use I2C and Serial. */
typedef void (*GpioExpChangeCb)(GpioExpPort port, uint8_t value, uint8_t changed);

/* Bring up the shared I2C bus, seed the register shadows from the chip and
 * attach the nINT handler. Call once from setup(). Does not change any pin
 * direction or level. */
void gpioExpInit();

/* False if the most recent transaction to the expander failed. Read functions
 * report failure as 0xFF, which is indistinguishable from a genuinely all-high
 * port, so check this when the difference matters. */
bool gpioExpOnline();

/* Live pin levels of one port. Also consumes any pending change notification
 * for that port - and only that port: the read clears nINT in hardware, so the
 * change is reported here instead of through the callback. Reading port 0 does
 * not clear a port 1 notification or vice versa (datasheet 8.4.1), so an
 * application that polls only one port must still service the other. */
uint8_t gpioExpRead(GpioExpPort port);

/* Live pin levels of both ports at once, port 0 in the low byte. Two bus
 * transactions rather than the four that two gpioExpRead() calls cost, so
 * prefer this whenever both ports are wanted. Returns false (and leaves
 * *value untouched) if the expander did not respond. */
bool gpioExpReadBoth(uint16_t *value);

/* As gpioExpReadBoth(), but one transaction instead of two: it addresses the
 * pair from the port 1 end so that the errata park is never needed (see
 * gpioexp.cpp). ~40% quicker, and the read path that gpioExpService() uses. */
bool gpioExpReadBothFast(uint16_t *value);

/* Drive a whole port. Bits belonging to pins still configured as inputs are
 * latched but have no effect until those pins become outputs. */
bool gpioExpWrite(GpioExpPort port, uint8_t value);

/* Drive both ports in one transaction, port 0 in the low byte. The command
 * pointer auto-toggles inside a register pair, so this carries twice the
 * payload for one extra data byte - it is the high-bandwidth path. */
bool gpioExpWriteBoth(uint16_t value);

/* Pin directions: a 1 bit leaves the pin an input (with its fixed ~100uA
 * internal pull-up), a 0 bit makes it a push-pull output. Both ports power up
 * as 0xFF, all inputs. */
bool gpioExpSetDirection(GpioExpPort port, uint8_t inputMask);

/* Invert selected bits of gpioExpRead(). Affects the input path only, never
 * outputs. Both ports power up as 0x00, no inversion. */
bool gpioExpSetPolarity(GpioExpPort port, uint8_t invertMask);

/* Shadowed register contents, no bus traffic. */
uint8_t gpioExpGetOutput(GpioExpPort port);
uint8_t gpioExpGetDirection(GpioExpPort port);
uint8_t gpioExpGetPolarity(GpioExpPort port);

/* Register a change callback, or nullptr to stop notifications. */
void gpioExpOnChange(GpioExpChangeCb cb);

/* Poll for input changes and dispatch the callback. Call from loop(); it costs
 * one digitalRead() when nothing has changed. */
void gpioExpService();
