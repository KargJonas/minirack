#pragma once
/*
 * RackOTA. Keep every minirack firmware updatable and inspectable over HTTP:
 *
 *   GET  /             tiny HTML GUI: status, rollback diagnostics, firmware
 *                      upload, config
 *   GET  /status       one JSON object with everything (identity, slots,
 *                      memory, config, rollback diagnostics) - the
 *                      machine-readable interface
 *   POST /update       raw app image -> the other slot, reboot into it
 *                      curl -H 'Content-Type: application/octet-stream'
 *                           --data-binary @firmware.bin http://<ip>/update
 *   POST /update-form  same, as multipart/form-data (used by the GUI form)
 *   POST /config       persist hostname / wifi ssid / wifi pass to NVS + reboot
 *   POST /boot?part=<label>  boot a specific slot (ota_0|ota_1)
 *   POST /reboot       just reboot
 *
 * Rollback: the bootloader boots fresh uploads in "pending verify" state and
 * RackOTA.begin() marks them valid; RackOTA also overrides the core's weak
 * verifyRollbackLater() so a crash anywhere before begin() reverts to the
 * previous image on the next reset.
 *
 * Safeguards against bad-but-validated images (which the bootloader alone
 * would happily boot forever, locking us out of a board with no UART):
 *  - crash-loop guard: 3 crash resets (panic/wdt) in a row without reaching
 *    5 min of uptime -> this image is marked invalid, the bootloader boots
 *    the previous one. Counted in RTC memory, checked before setup() runs.
 *  - reachability watchdog: an independent task probes the HTTP server over
 *    loopback every 15 s. Unreachable for 5 min -> reboot (cures leaks and
 *    wedged tasks); still unreachable after that reboot -> mark invalid and
 *    boot the previous image. Override the window with -DRACKOTA_WD_FAIL_MS.
 *
 * Usage:
 *   void setup() { ...network up...; RackOTA.begin("my-app v1"); }
 *   void loop()  { RackOTA.handle(); }
 *
 * Built on ESPAsyncWebServer: requests are served from the async_tcp task,
 * so a busy loop() can't stall HTTP; handle() only runs deferred reboots.
 */

#include <Arduino.h>

class AsyncWebServer;
class AsyncWebServerRequest;

class RackOTAClass {
public:
    /* Crash-loop accounting runs here, before setup(). */
    RackOTAClass();

    /* Call once after the network is up. appInfo is shown on / and /status. */
    void begin(const char *appInfo = "", uint16_t port = 80);
    /* Call from loop(). Only services deferred reboots; HTTP is async. */
    void handle();

    /* The underlying server, for registering app-specific routes after begin(). */
    AsyncWebServer *server() { return _server; }

    /* Called right before an OTA/HTTP-triggered restart, e.g. to park pins
     * in a safe state (strapping pins!). */
    void onReboot(void (*cb)()) { _onReboot = cb; }

    /* Persisted config (NVS namespace "rackota"), editable via the GUI.
     * Falls back to the given default while unset. Read these in setup()
     * for your network bringup - and keep a compiled-in fallback so a bad
     * SSID entered in the GUI can't strand the board. */
    String hostname(const char *def = "minirack") { return configValue("hostname", def); }
    String wifiSsid(const char *def = "") { return configValue("ssid", def); }
    String wifiPass(const char *def = "") { return configValue("pass", def); }


private:
    String configValue(const char *key, const char *def);

    void handleRoot(AsyncWebServerRequest *req);
    String rootToken(const String &var);
    void handleStatus(AsyncWebServerRequest *req);
    void handleConfig(AsyncWebServerRequest *req);
    void handleUpdateData(AsyncWebServerRequest *req, uint8_t *data, size_t len,
                          size_t index, size_t total);
    void handleUpdateForm(AsyncWebServerRequest *req, const String &filename,
                          size_t index, uint8_t *data, size_t len, bool final);
    void handleUpdateDone(AsyncWebServerRequest *req);
    bool updateTooBig(size_t bodySize, size_t slack);
    void handleBoot(AsyncWebServerRequest *req);
    void sendActionPage(AsyncWebServerRequest *req, const String &msg);
    void scheduleReboot();

    static void wdEntry(void *self);
    void watchdogTask();

    AsyncWebServer *_server = nullptr;
    String _info;
    uint16_t _port = 80;
    bool _stableMarked = false;
    void (*_onReboot)() = nullptr;
    /* set instead of touching Update when an upload is rejected up front;
     * only accessed from the async_tcp task */
    String _updateErr;
    /* written from the async_tcp task, polled from loop() */
    volatile bool _rebootPending = false;
    volatile uint32_t _rebootAt = 0;
};

extern RackOTAClass RackOTA;
