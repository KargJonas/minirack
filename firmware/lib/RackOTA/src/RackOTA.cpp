#include "RackOTA.h"
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#endif

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

static String sha256Hex(const uint8_t *sha)
{
    char hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(hex + 2 * i, "%02x", sha[i]);
    return String(hex);
}

static String runningSha()
{
    return sha256Hex(esp_ota_get_app_description()->app_elf_sha256);
}

/* ------------------------------------------------------------------ */
/* Rollback diagnostics. After the bootloader reverts a bad upload,     */
/* three artifacts explain what happened, and we surface all of them:  */
/*  - the reset reason of the crash survives in RTC (panic/wdt/...)    */
/*  - the rejected image sits in its slot marked "aborted", with its   */
/*    per-build sha still readable from the image header               */
/*  - the panic handler wrote a core dump to the coredump partition:   */
/*    crashing task, PC, backtrace (addr2line-able against the .elf)   */
/* ------------------------------------------------------------------ */

static const char *resetReasonName()
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt-wdt";
    case ESP_RST_TASK_WDT:  return "task-wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep-wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static const char *excCauseName(uint32_t cause)
{
    switch (cause) {
    case 0:  return "illegal-instruction";
    case 3:  return "load-store-error";
    case 6:  return "divide-by-zero";
    case 9:  return "unaligned-access";
    case 28: return "load-prohibited";
    case 29: return "store-prohibited";
    default: return "";
    }
}

/* First OTA slot whose image the bootloader rejected, with the per-build
 * sha of the image still sitting in it. */
static bool abortedSlot(String &slot, String &sha)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it != NULL; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) continue;
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(p, &st) != ESP_OK) continue;
        if (st != ESP_OTA_IMG_ABORTED && st != ESP_OTA_IMG_INVALID) continue;
        slot = p->label;
        esp_app_desc_t desc;
        sha = (esp_ota_get_partition_description(p, &desc) == ESP_OK)
                  ? sha256Hex(desc.app_elf_sha256) : String("?");
        esp_partition_iterator_release(it);
        return true;
    }
    esp_partition_iterator_release(it);
    return false;
}

struct CrashInfo {
    bool valid = false;
    String task, pc, bt, elf;
    uint32_t cause = 0, vaddr = 0;
};

/* Summary of the core dump the panic handler left in the coredump
 * partition. Persists until the next crash overwrites it, so "elf" (the
 * crashing build's truncated sha) tells whose crash this was. */
static CrashInfo crashInfo()
{
    CrashInfo c;
#if defined(CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) && defined(CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF)
    esp_core_dump_summary_t sum;
    if (esp_core_dump_get_summary(&sum) != ESP_OK) return c;
    c.valid = true;
    char task[sizeof(sum.exc_task) + 1] = { 0 };
    memcpy(task, sum.exc_task, sizeof(sum.exc_task));
    c.task = task;
    char buf[12];
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)sum.exc_pc);
    c.pc = buf;
    for (uint32_t i = 0; i < sum.exc_bt_info.depth && i < 16; i++) {
        snprintf(buf, sizeof(buf), "0x%08x", (unsigned)sum.exc_bt_info.bt[i]);
        if (c.bt.length()) c.bt += " ";
        c.bt += buf;
    }
    if (sum.exc_bt_info.corrupted) c.bt += " (corrupted)";
    c.elf = (const char *)sum.app_elf_sha256;
    c.cause = sum.ex_info.exc_cause;
    c.vaddr = sum.ex_info.exc_vaddr;
#endif
    return c;
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
    const esp_partition_t *running = esp_ota_get_running_partition();
    _isLoader = running && running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;

    /* We survived until here: keep this image across reboots. (The loader
     * partition has no otadata state to validate.) */
    if (!_isLoader) esp_ota_mark_app_valid_cancel_rollback();

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
    _server->on("/boot", HTTP_POST, [this](AsyncWebServerRequest *req) { handleBoot(req); });
    if (!_isLoader)
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
Last reset: %LAST_RESET%
%DIAG%
        </pre>

        <h2>Upload Firmware</h2>
        <p>
            This board keeps two application partitions: the running image and a spare.<br>
            Uploading firmware writes the spare and reboots into it.<br>
            If the new application crashes before validating itself, the bootloader rolls<br>
            back to the previous image, ultimately to the built-in loader, so a bad<br>
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
        %LOADER_FORM%
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
    if (var == "LAST_RESET") return resetReasonName();
    if (var == "DIAG") {
        String out, slot, sha;
        if (abortedSlot(slot, sha))
            out += "Rolled back: " + slot + " holds an aborted image (sha " +
                   sha.substring(0, 16) + "...)\n";
        CrashInfo c = crashInfo();
        if (c.valid) {
            out += "Last crash:  task '" + htmlEscape(c.task) + "' at " + c.pc +
                   ", cause " + String(c.cause);
            if (*excCauseName(c.cause)) out += " " + String(excCauseName(c.cause));
            out += "\n             backtrace: " + c.bt + "  (crashing build " + c.elf + ")";
        }
        if (out.endsWith("\n")) out.remove(out.length() - 1);
        return out;
    }
    if (var == "LOADER_FORM")
        return _isLoader ? String() :
            String("<form method='POST' action='/loader'>"
                   "<input type='submit' value='Reboot into Loader'></form>");
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
    String s = String("{\"role\":\"") + (_isLoader ? "loader" : "app") + "\",";
    s += "\"info\":\"" + jsonEscape(_info) + "\",";
    s += "\"partition\":\"" + String(running ? running->label : "?") + "\",";
    s += "\"elf_sha256\":\"" + runningSha() + "\",";
    s += "\"uptime_s\":" + String(millis() / 1000) + ",";

    /* rollback diagnostics; flat keys so grep-based clients (flash.sh) can
     * extract them without a JSON parser */
    s += "\"last_reset\":\"" + String(resetReasonName()) + "\",";
    String abSlot, abSha;
    if (abortedSlot(abSlot, abSha)) {
        s += "\"aborted_slot\":\"" + abSlot + "\",";
        s += "\"aborted_sha\":\"" + abSha + "\",";
    }
    CrashInfo crash = crashInfo();
    if (crash.valid) {
        char vaddr[12];
        snprintf(vaddr, sizeof(vaddr), "0x%08x", (unsigned)crash.vaddr);
        s += "\"crash_task\":\"" + jsonEscape(crash.task) + "\",";
        s += "\"crash_pc\":\"" + crash.pc + "\",";
        s += "\"crash_cause\":" + String(crash.cause) + ",";
        s += "\"crash_vaddr\":\"" + String(vaddr) + "\",";
        s += "\"crash_elf\":\"" + crash.elf + "\",";
        s += "\"crash_bt\":\"" + crash.bt + "\",";
    }

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

/* Reject an image that cannot fit the spare slot before burning any flash,
 * with a message that names both sizes. bodySize is exact for raw uploads;
 * for multipart it is the whole request, a close upper bound of the file. */
bool RackOTAClass::updateTooBig(size_t bodySize, size_t slack)
{
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || bodySize <= dst->size + slack) return false;
    _updateErr = "image (" + String(bodySize - slack) + " B) does not fit OTA slot " +
                 dst->label + " (" + String(dst->size) + " B)";
    Serial.printf("[RackOTA] update rejected: %s\n", _updateErr.c_str());
    return true;
}

void RackOTAClass::handleUpdateData(AsyncWebServerRequest *req, uint8_t *data,
                                    size_t len, size_t index, size_t total)
{
    if (index == 0) {
        _updateErr = "";
        if (updateTooBig(total, 0)) return;
        Serial.println("[RackOTA] update started");
        if (Update.isRunning()) Update.abort(); /* client of a previous upload vanished */
        Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (_updateErr.length()) return;
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
        _updateErr = "";
        /* multipart framing adds < 1 KiB on top of the file itself */
        if (updateTooBig(req->contentLength(), 4096)) return;
        Serial.printf("[RackOTA] form update started: %s\n", filename.c_str());
        if (Update.isRunning()) Update.abort();
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (_updateErr.length()) return;
    if (!Update.hasError()) Update.write(data, len);
    if (final) {
        Update.end(true);
        Serial.printf("[RackOTA] form update finished, %u bytes\n", (unsigned)(index + len));
    }
}

void RackOTAClass::handleUpdateDone(AsyncWebServerRequest *req)
{
    if (_updateErr.length()) {
        req->send(400, "text/plain", "update failed: " + _updateErr + "\n");
        _updateErr = "";
        return;
    }
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

void RackOTAClass::handleBoot(AsyncWebServerRequest *req)
{
    if (!req->hasParam("part")) {
        req->send(400, "text/plain", "use /boot?part=loader|ota_0|ota_1\n");
        return;
    }
    String part = req->getParam("part")->value();
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, part.c_str());
    if (!p) {
        req->send(400, "text/plain", "no such app partition\n");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(p); /* validates the image */
    if (err != ESP_OK) {
        req->send(500, "text/plain", String(esp_err_to_name(err)) + "\n");
        return;
    }
    req->send(200, "text/plain", "OK, rebooting into " + part + "\n");
    scheduleReboot();
}

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
