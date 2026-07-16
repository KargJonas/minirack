#pragma once
/*
 * RackOTA. Keep every minirack firmware updatable and inspectable over HTTP:
 *
 *   GET  /             tiny HTML GUI: status, rollback diagnostics, firmware
 *                      upload, config
 *   GET  /status       one JSON object with everything (identity, slots,
 *                      memory, config, rollback diagnostics) - the
 *                      machine-readable interface
 *   POST /update       raw app image -> spare OTA slot, reboot into it
 *                      curl --data-binary @firmware.bin http://<ip>/update
 *   POST /update-form  same, as multipart/form-data (used by the GUI form)
 *   POST /config       persist hostname / wifi ssid / wifi pass to NVS + reboot
 *   POST /boot?part=<label>  boot a specific partition (loader|ota_0|ota_1)
 *   POST /loader       reboot into the loader (apps only; = /boot?part=loader)
 *   POST /reboot       just reboot
 *
 * The same class serves apps and the loader itself: running from the factory
 * partition it reports role "loader" in /status, hides the "reboot into
 * loader" action, and skips the rollback validation handshake.
 *
 * Usage:
 *   void setup() { ...network up...; RackOTA.begin("my-app v1"); }
 *   void loop()  { RackOTA.handle(); }
 *
 * Built on ESPAsyncWebServer: requests are served from the async_tcp task,
 * so handle() only runs deferred reboots - but keep calling it from loop().
 *
 * Rollback: the bootloader boots fresh uploads in "pending verify" state and
 * RackOTA.begin() marks them valid; RackOTA also overrides the core's weak
 * verifyRollbackLater() so a crash anywhere before begin() reverts to the
 * previous image on the next reset.
 */

#include <Arduino.h>

class AsyncWebServer;
class AsyncWebServerRequest;

class RackOTAClass {
public:
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
    void handleLoader(AsyncWebServerRequest *req);
    void scheduleReboot();

    AsyncWebServer *_server = nullptr;
    String _info;
    bool _isLoader = false;
    /* set instead of touching Update when an upload is rejected up front;
     * only accessed from the async_tcp task */
    String _updateErr;
    void (*_onReboot)() = nullptr;
    /* written from the async_tcp task, polled from loop() */
    volatile bool _rebootPending = false;
    volatile uint32_t _rebootAt = 0;
};

extern RackOTAClass RackOTA;
