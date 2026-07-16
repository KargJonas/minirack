#include "RackOTA.h"
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

RackOTAClass RackOTA;

/* The Arduino core auto-validates a pending image in initArduino() -- before
 * setup() runs -- which would defeat rollback for apps that crash in setup().
 * This override (of the core's weak symbol) defers validation until
 * RackOTA.begin() is reached. */
extern "C" bool verifyRollbackLater() { return true; }

static const char *otaStateName(const esp_partition_t *part)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(part, &st) != ESP_OK) return "unknown";
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

static String runningSha()
{
    const esp_app_desc_t *desc = esp_ota_get_app_description();
    char sha[65];
    for (int i = 0; i < 32; i++)
        sprintf(sha + 2 * i, "%02x", desc->app_elf_sha256[i]);
    return String(sha);
}

static String slotList()
{
    String slots;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (slots.length()) slots += ", ";
        slots += String(p->label) + "=";
        slots += (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) ? "loader" : otaStateName(p);
    }
    esp_partition_iterator_release(it);
    return slots;
}

static String htmlEscape(String s)
{
    s.replace("&", "&amp;");
    s.replace("<", "&lt;");
    s.replace(">", "&gt;");
    s.replace("\"", "&quot;");
    return s;
}

static String jsonEscape(String s)
{
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    return s;
}

String RackOTAClass::configValue(const char *key, const char *def)
{
    Preferences prefs;
    if (!prefs.begin("rackota", true)) return String(def); /* namespace not created yet */
    String v = prefs.isKey(key) ? prefs.getString(key, def) : String(def);
    prefs.end();
    return v;
}

void RackOTAClass::begin(const char *appInfo, uint16_t port)
{
    /* We survived until here: keep this image across reboots. */
    esp_ota_mark_app_valid_cancel_rollback();

    _info = appInfo;
    _server = new AsyncWebServer(port);
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *req) { handleRoot(req); });
    _server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *req) { handleStatus(req); });
    _server->on("/update", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleUpdateDone(req); },
                nullptr,
                [this](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                       size_t index, size_t total) {
                    handleUpdateData(req, data, len, index, total);
                });
    _server->on("/update-form", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleUpdateDone(req); },
                [this](AsyncWebServerRequest *req, const String &filename, size_t index,
                       uint8_t *data, size_t len, bool final) {
                    handleUpdateForm(req, filename, index, data, len, final);
                });
    _server->on("/config", HTTP_POST, [this](AsyncWebServerRequest *req) { handleConfig(req); });
    _server->on("/loader", HTTP_POST, [this](AsyncWebServerRequest *req) { handleLoader(req); });
    _server->on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "OK, rebooting\n");
        scheduleReboot();
    });
    _server->begin();
}

void RackOTAClass::handle()
{
    if (_rebootPending && (int32_t)(millis() - _rebootAt) >= 0) {
        if (_onReboot) _onReboot();
        ESP.restart();
    }
}

void RackOTAClass::scheduleReboot()
{
    _rebootAt = millis() + 750; /* let the response drain first */
    _rebootPending = true;
}

static const char ROOT_TMPL[] PROGMEM = R"html(<!DOCTYPE html>
<html>
    <head>
        <title>%HOST%</title>
    </head>
    <body>
        <h1>%HOST%</h1>
        <pre>
App:        %INFO%
SHA-256:    %SHA%
Partition:  %PART%
Slots:      %SLOTS%
Uptime:     %UPTIME% s
Heap:       %HEAP_FREE% KiB free of %HEAP_SIZE% KiB (min. ever %HEAP_MIN%, largest block %HEAP_BLOCK%)
Flash:      app uses %FLASH_USED% KiB of %FLASH_SLOT% KiB slot
Tasks:      %TASKS% (loop stack headroom %STACK% B)
        </pre>

        <h2>Upload Firmware</h2>
        <p>
            This board keeps two application partitions: the running image and a spare.<br>
            Uploading firmware writes the spare and reboots into it.<br>
            If the new application crashes before validating itself, the bootloader rolls<br>
            back to the previous image, ultimately to the built-in loader, so a bad<br
            upload cannot brick the board.
        </p>
        <form method='POST' action='/update-form' enctype='multipart/form-data'>
            <input type='file' name='fw'> <input type='submit' value='Flash'>
        </form>

        <h2>Configuration</h2>
        <p>
            The WiFi credentials are used to join an existing network (station mode).
        </p>

        <form method='POST' action='/config'>
        <table>
            <tr><td>Hostname</td><td><input name='hostname' value='%CFG_HOST%'></td></tr>
            <tr><td>WiFi SSID</td><td><input name='ssid' value='%CFG_SSID%'></td></tr>
            <tr><td>WiFi Password</td><td><input type='password' name='pass' placeholder='(unchanged)'></td></tr>
        </table>
        <input type='submit' value='Save &amp; Reboot'>
        </form>

        <h2>Actions</h2>
        <form method='POST' action='/reboot'><input type='submit' value='Reboot'></form>
        <form method='POST' action='/loader'><input type='submit' value='Reboot into Loader'></form>
    </body>
</html>
)html";

/* Substitution is a single streaming pass over the template; returned values
 * are never re-scanned, so a stored "%TOKEN%" in hostname/ssid is inert. */
String RackOTAClass::rootToken(const String &var)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (var == "HOST" || var == "CFG_HOST") return htmlEscape(hostname());
    if (var == "INFO")       return htmlEscape(_info);
    if (var == "CFG_SSID")   return htmlEscape(wifiSsid());
    if (var == "SHA")        return runningSha();
    if (var == "PART")       return running ? running->label : "?";
    if (var == "SLOTS")      return slotList();
    if (var == "UPTIME")     return String(millis() / 1000);
    if (var == "HEAP_FREE")  return String(ESP.getFreeHeap() / 1024);
    if (var == "HEAP_SIZE")  return String(ESP.getHeapSize() / 1024);
    if (var == "HEAP_MIN")   return String(ESP.getMinFreeHeap() / 1024);
    if (var == "HEAP_BLOCK") return String(ESP.getMaxAllocHeap() / 1024);
    if (var == "FLASH_USED") return String(ESP.getSketchSize() / 1024);
    if (var == "FLASH_SLOT") return String(running ? running->size / 1024 : 0);
    if (var == "TASKS")      return String(uxTaskGetNumberOfTasks());
    if (var == "STACK")      return String(uxTaskGetStackHighWaterMark(NULL));
    return String();
}

void RackOTAClass::handleRoot(AsyncWebServerRequest *req)
{
    req->send(200, "text/html", ROOT_TMPL,
              [this](const String &var) { return rootToken(var); });
}

/* ---------------------------------------------- */
/* GET /status - everything, machine-readable     */
/* ---------------------------------------------- */

void RackOTAClass::handleStatus(AsyncWebServerRequest *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    /* identity: elf_sha256 is the per-build hash the toolchain embeds in
     * every image; the same bytes sit at offset 176 of the .bin, so a client
     * (flash.sh) can prove the exact image it uploaded is what runs now */
    String s = "{\"role\":\"app\",";
    s += "\"info\":\"" + jsonEscape(_info) + "\",";
    s += "\"partition\":\"" + String(running ? running->label : "?") + "\",";
    s += "\"elf_sha256\":\"" + runningSha() + "\",";
    s += "\"uptime_s\":" + String(millis() / 1000) + ",";

    s += "\"slots\":{";
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    bool first = true;
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (!first) s += ",";
        first = false;
        s += "\"" + String(p->label) + "\":\"";
        s += (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) ? "loader" : otaStateName(p);
        s += "\"";
    }
    esp_partition_iterator_release(it);
    s += "},";

    /* min_free_heap: low-water mark since boot — sinking over days = leak.
     * largest_free_block vs free_heap gap = fragmentation. */
    s += "\"mem\":{";
    s += "\"heap_size\":" + String(ESP.getHeapSize()) + ",";
    s += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    s += "\"min_free_heap\":" + String(ESP.getMinFreeHeap()) + ",";
    s += "\"largest_free_block\":" + String(ESP.getMaxAllocHeap()) + ",";
    s += "\"loop_stack_free\":" + String(uxTaskGetStackHighWaterMark(NULL)) + ",";
    s += "\"tasks\":" + String(uxTaskGetNumberOfTasks());
    s += "},";

    s += "\"flash\":{";
    s += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
    s += "\"slot_size\":" + String(running ? running->size : 0);
    s += "},";

    s += "\"config\":{";
    s += "\"hostname\":\"" + jsonEscape(hostname()) + "\",";
    s += "\"wifi_ssid\":\"" + jsonEscape(wifiSsid()) + "\"";
    s += "}}\n";
    req->send(200, "application/json", s);
}

/* ------------------------------------------------------------------ */
/* POST /config                                                        */
/* ------------------------------------------------------------------ */

void RackOTAClass::handleConfig(AsyncWebServerRequest *req)
{
    Preferences prefs;
    if (!prefs.begin("rackota", false)) {
        req->send(500, "text/plain", "NVS open failed\n");
        return;
    }
    /* empty field: hostname/ssid revert to default, password stays unchanged */
    if (req->hasParam("hostname", true)) {
        String v = req->getParam("hostname", true)->value();
        v.length() ? (void)prefs.putString("hostname", v) : (void)prefs.remove("hostname");
    }
    if (req->hasParam("ssid", true)) {
        String v = req->getParam("ssid", true)->value();
        v.length() ? (void)prefs.putString("ssid", v) : (void)prefs.remove("ssid");
    }
    if (req->hasParam("pass", true) && req->getParam("pass", true)->value().length()) {
        prefs.putString("pass", req->getParam("pass", true)->value());
    }
    prefs.end();
    req->send(200, "text/plain", "config saved, rebooting\n");
    scheduleReboot();
}

/* ------------------------------------------------------------------ */
/* firmware upload: raw body (curl) and multipart (GUI form)           */
/* ------------------------------------------------------------------ */

void RackOTAClass::handleUpdateData(AsyncWebServerRequest *req, uint8_t *data,
                                    size_t len, size_t index, size_t total)
{
    if (index == 0) {
        Serial.println("[RackOTA] update started");
        if (Update.isRunning()) Update.abort(); /* client of a previous upload vanished */
        Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (!Update.hasError()) Update.write(data, len);
    if (index + len == total) {
        Update.end(true);
        Serial.printf("[RackOTA] update finished, %u bytes\n", (unsigned)total);
    }
}

void RackOTAClass::handleUpdateForm(AsyncWebServerRequest *req, const String &filename,
                                    size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        Serial.printf("[RackOTA] form update started: %s\n", filename.c_str());
        if (Update.isRunning()) Update.abort();
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (!Update.hasError()) Update.write(data, len);
    if (final) {
        Update.end(true);
        Serial.printf("[RackOTA] form update finished, %u bytes\n", (unsigned)(index + len));
    }
}

void RackOTAClass::handleUpdateDone(AsyncWebServerRequest *req)
{
    if (Update.hasError()) {
        req->send(400, "text/plain",
                  String("update failed: ") + Update.errorString() + "\n");
        Update.clearError();
        return;
    }
    req->send(200, "text/plain", "OK: update written, rebooting into it\n");
    scheduleReboot();
}

/* ------------------------------------------------------------------ */

void RackOTAClass::handleLoader(AsyncWebServerRequest *req)
{
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory) {
        req->send(500, "text/plain", "no factory partition found\n");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        req->send(500, "text/plain", String(esp_err_to_name(err)) + "\n");
        return;
    }
    req->send(200, "text/plain", "OK, rebooting into loader\n");
    scheduleReboot();
}
