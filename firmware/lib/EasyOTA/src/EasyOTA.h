#pragma once
/*
 * EasyOTA. Keep any ESP32 firmware updatable and inspectable over HTTP:
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
 * Discovery: the device advertises the mDNS service _easyota._tcp (TXT:
 * info, sha, part), so tools can find boards without knowing IP or
 * hostname: avahi-browse -rt _easyota._tcp, or just flash.sh.
 *
 * Rollback: the bootloader boots fresh uploads in "pending verify" state and
 * EasyOTA.begin() marks them valid; EasyOTA also overrides the core's weak
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
 *    boot the previous image. Override the window with -DEASYOTA_WD_FAIL_MS.
 *
 * Crash diagnostics on / and /status: reset reason, core-dump summary of the
 * last crash (task, PC, cause, backtrace, which build) and its wall-clock
 * time. The clock is SNTP-synced in begin() (UTC; override the server with
 * -DEASYOTA_NTP_SERVER, default pool.ntp.org); crash timestamps work because
 * system time survives panic resets in the RTC domain and the stamp is
 * persisted to NVS on the next boot.
 *
 * Usage:
 *   void setup() { EasyOTA.beginNetwork(); EasyOTA.begin("my-app v1"); }
 *   void loop()  { EasyOTA.handle(); }
 *
 * Built on ESPAsyncWebServer: requests are served from the async_tcp task,
 * so a busy loop() can't stall HTTP; handle() only runs deferred reboots.
 */

#include <Arduino.h>

class AsyncWebServer;
class AsyncWebServerRequest;

class EasyOTAClass {
public:
    /* Crash-loop accounting runs here, before setup(). */
    EasyOTAClass();

    /* Bring the network up, blocking until the board has an IP. The build
     * flags select the interface (see EasyNet.cpp):
     *   -DEASYOTA_USE_ETH    WT32-ETH01 (LAN8720 pinout baked in)
     *   -DEASYOTA_USE_WIFI   station mode; EASYOTA_WIFI_SSID / _WIFI_PASS are
     *                        the compiled-in fallback credentials
     * GUI-configured WiFi credentials (NVS) are tried first, the compiled-in
     * ones second, so a typo saved in the web form can't strand the board.
     * The persisted hostname() is requested via DHCP. */
    void beginNetwork();

    /* Call once after the network is up. appInfo is shown on / and /status. */
    void begin(const char *appInfo = "", uint16_t port = 80);
    /* Call from loop(). Only services deferred reboots; HTTP is async. */
    void handle();

    /* The underlying server, for app-specific routes (include
     * ESPAsyncWebServer.h and register after begin()):
     *   EasyOTA.server()->on("/mine", HTTP_GET, ...);
     * Handlers match in registration order, so app routes can never shadow
     * the EasyOTA endpoints. Prefer sharing this server over running a
     * second AsyncWebServer: every instance is serviced by the same
     * async_tcp task anyway (a second port adds no failure isolation), and
     * the reachability watchdog guards this one. A server from a different
     * stack (e.g. the synchronous WebServer.h driven from loop()) on its
     * own port IS isolated from EasyOTA - if it wedges, this server keeps
     * serving and OTA stays available as the rescue path. */
    AsyncWebServer *server() { return _server; }

    /* Called right before an OTA/HTTP-triggered restart, e.g. to park pins
     * in a safe state (strapping pins!). */
    void onReboot(void (*cb)()) { _onReboot = cb; }

    /* Boot timeline: stamp a named event (tags longer than 15 chars are
     * truncated) at the current time-since-boot. EasyOTA records boot,
     * net-begin/net-up, validated, http-up, mdns-up, update-start/-done and
     * reboot-sched by itself; add your app's milestones from setup() etc.
     * The sequence shows on / and /status ("timeline"), and after a crash
     * the crashed boot's sequence survives into the rollback diagnostics
     * ("crash_timeline", with a last-alive heartbeat bracketing the crash
     * moment) - how far did bringup get, and when. */
    void event(const char *tag);

    /* Persisted config (NVS namespace "easyota"), editable via the GUI.
     * Falls back to the given default while unset. Read these in setup()
     * for your network bringup - and keep a compiled-in fallback so a bad
     * SSID entered in the GUI can't strand the board. */
    /* hostname's default is unique per board: esp32-easyota-<xxxxxx>, the
     * last three bytes of the chip's factory-burned MAC - so several
     * unconfigured boards remain distinguishable in discovery. */
    String hostname(const char *def = nullptr);
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
    bool updateBusy(AsyncWebServerRequest *req);
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
    /* owner of the running Update session + its last write, to tell a
     * concurrent second upload (reject) from a stale session whose client
     * vanished (take over); only accessed from the async_tcp task */
    AsyncWebServerRequest *_updateReq = nullptr;
    uint32_t _updateLastMs = 0;
    /* written from the async_tcp task, polled from loop() */
    volatile bool _rebootPending = false;
    volatile uint32_t _rebootAt = 0;
};

extern EasyOTAClass EasyOTA;
