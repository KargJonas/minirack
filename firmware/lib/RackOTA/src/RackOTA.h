#pragma once
/*
 * RackOTA — keep every minirack app updatable over HTTP, same UX as the loader:
 *
 *   GET  /         app info
 *   POST /update   raw app image -> other OTA slot, reboot into it
 *                  curl --data-binary @firmware.bin http://<ip>/update
 *   POST /loader   reboot into the factory loader
 *   POST /reboot   just reboot
 *
 * Usage:
 *   void setup() { ...network up...; RackOTA.begin("my-app v1"); }
 *   void loop()  { RackOTA.handle(); }
 *
 * begin() also calls esp_ota_mark_app_valid_cancel_rollback(): the loader's
 * bootloader is built with app rollback enabled, so an app that crashes
 * before reaching begin() is rolled back automatically on the next reset.
 */

#include <Arduino.h>

class WebServer;

class RackOTAClass {
public:
    /* Call once after the network is up. appInfo is shown on GET /. */
    void begin(const char *appInfo = "", uint16_t port = 80);
    /* Call from loop(). */
    void handle();

    /* The underlying server, for registering app-specific routes after begin(). */
    WebServer *server() { return _server; }

    /* Called right before an OTA/HTTP-triggered restart, e.g. to park pins
     * in a safe state (strapping pins!). */
    void onReboot(void (*cb)()) { _onReboot = cb; }

private:
    void handleInfo();
    void handleUpdateData();
    void handleUpdateDone();
    void handleLoader();
    void handleReboot();
    void scheduleReboot();

    WebServer *_server = nullptr;
    String _info;
    void (*_onReboot)() = nullptr;
    bool _rebootPending = false;
    uint32_t _rebootAt = 0;
};

extern RackOTAClass RackOTA;
