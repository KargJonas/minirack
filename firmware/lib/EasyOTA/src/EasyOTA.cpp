#include "EasyOTA.h"
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <Preferences.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "lwip/sockets.h"
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#endif

EasyOTAClass EasyOTA;

/* ------------------------------------------------------------------ */
/* Safeguards. A/B rollback only covers images that crash before        */
/* validating; a bad-but-VALIDATED image would be booted forever and    */
/* lock us out of a board with no UART. Two layers, both escaping via   */
/* otadata to the previous image:                                       */
/*  - crash-loop guard (constructor, pre-setup): 3 crash resets in a    */
/*    row without 5 min of stable uptime -> mark this image invalid     */
/*  - reachability watchdog (own task): HTTP server must answer a       */
/*    loopback probe; 5 min of failures -> reboot (cures heap leaks,    */
/*    wedged tasks); still failing after that reboot -> mark invalid    */
/* State lives in RTC memory: survives resets, cleared on power-on.     */
/* ------------------------------------------------------------------ */

#define GUARD_MAGIC       0x52474431 /* "RGD1" */
#define GUARD_CRASH_LIMIT 3
#define GUARD_STABLE_MS   (5 * 60 * 1000)
#define WD_PROBE_MS       15000
#ifndef EASYOTA_WD_FAIL_MS
#define EASYOTA_WD_FAIL_MS (5 * 60 * 1000)
#endif

RTC_NOINIT_ATTR static uint32_t guardMagic;
RTC_NOINIT_ATTR static uint32_t guardCrashes;
RTC_NOINIT_ATTR static uint32_t guardWdStage;

/* Global-constructor time, i.e. before setup() - so a validated image
 * that crashes even in setup() still gets counted and escaped from. */
EasyOTAClass::EasyOTAClass()
{
    if (guardMagic != GUARD_MAGIC) { /* power-on: RTC RAM is garbage */
        guardMagic = GUARD_MAGIC;
        guardCrashes = 0;
        guardWdStage = 0;
    }
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
        r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        if (++guardCrashes >= GUARD_CRASH_LIMIT) {
            guardCrashes = 0; /* don't re-fire every boot if rollback is impossible */
            esp_rom_printf("[EasyOTA] %d crash resets in a row, rolling back\n",
                           GUARD_CRASH_LIMIT);
            esp_ota_mark_app_invalid_rollback_and_reboot(); /* no return on success */
            esp_rom_printf("[EasyOTA] rollback impossible (no valid other slot)\n");
        }
    }
}

static bool haveIp()
{
    for (esp_netif_t *n = esp_netif_next(NULL); n; n = esp_netif_next(n)) {
        esp_netif_ip_info_t ip;
        if (esp_netif_is_netif_up(n) &&
            esp_netif_get_ip_info(n, &ip) == ESP_OK && ip.ip.addr)
            return true;
    }
    return false;
}

/* Full round trip through lwip and the async_tcp task: if this answers,
 * a client on the wire would be served too (modulo link/PHY, which the
 * haveIp() check approximates). */
static bool probeSelf(uint16_t port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = false;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        const char req[] = "GET /status HTTP/1.0\r\n\r\n";
        if (send(s, req, sizeof(req) - 1, 0) > 0) {
            char c;
            ok = recv(s, &c, 1, 0) > 0;
        }
    }
    close(s);
    return ok;
}

void EasyOTAClass::wdEntry(void *self)
{
    ((EasyOTAClass *)self)->watchdogTask();
}

void EasyOTAClass::watchdogTask()
{
    uint32_t firstFail = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WD_PROBE_MS));
        if (haveIp() && probeSelf(_port)) {
            firstFail = 0;
            guardWdStage = 0;
            continue;
        }
        if (!firstFail) firstFail = millis();
        if (millis() - firstFail < EASYOTA_WD_FAIL_MS) continue;
        if (Update.isRunning()) continue; /* never yank the rug mid-upload */
        if (guardWdStage == 0) {
            guardWdStage = 1;
            Serial.println("[EasyOTA] watchdog: server unreachable, rebooting");
        } else {
            Serial.println("[EasyOTA] watchdog: unreachable across a reboot, rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            /* only reached if there is no valid other slot: reboot and retry */
        }
        if (_onReboot) _onReboot();
        ESP.restart();
    }
}

/* The Arduino core auto-validates a pending image in initArduino() -- before
 * setup() runs -- which would defeat rollback for apps that crash in setup().
 * This override (of the core's weak symbol) defers validation until
 * EasyOTA.begin() is reached. */
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

/* ------------------------------------------------------------------  */
/* Rollback diagnostics. After the bootloader reverts a bad upload,    */
/*  - the reset reason of the crash survives in RTC (panic/wdt/...)    */
/*  - the rejected image sits in its slot marked "aborted", with its   */
/*    per-build sha still readable from the image header               */
/*  - the panic handler wrote a core dump to the coredump partition:   */
/*    crashing task, PC, backtrace (addr2line-able against the .elf)   */
/* ------------------------------------------------------------------  */

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
        slots += otaStateName(p);
    }
    esp_partition_iterator_release(it);
    return slots;
}

static String pct(uint32_t used, uint32_t total)
{
    return total ? String(used * 100 / total) + "%" : String("?");
}

static String htmlEscape(String s)
{
    s.replace("&", "&amp;");
    s.replace("<", "&lt;");
    s.replace(">", "&gt;");
    s.replace("\"", "&quot;");
    return s;
}

/* For values substituted into ROOT_TMPL: a literal '%' inside a value gets
 * rescanned by the template processor and re-pairs the placeholder
 * delimiters, garbling the rest of the page - emit it as an entity. */
static String tmplEscape(String s)
{
    s = htmlEscape(s);
    s.replace("%", "&#37;");
    return s;
}

static String jsonEscape(String s)
{
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    return s;
}

String EasyOTAClass::configValue(const char *key, const char *def)
{
    Preferences prefs;
    if (!prefs.begin("easyota", true)) return String(def); /* namespace not created yet */
    String v = prefs.isKey(key) ? prefs.getString(key, def) : String(def);
    prefs.end();
    return v;
}

/* curl --data-binary defaults to Content-Type: application/x-www-form-urlencoded,
 * which ESPAsyncWebServer parses by accumulating the WHOLE body in a heap
 * String (the streaming body callback is bypassed) - for a firmware-sized
 * upload that is a guaranteed OOM abort mid-request. This handler catches
 * such requests first; it reports "trivial" (the default), so the library
 * discards the body instead of parsing it, and the client gets told what to
 * send. flash.sh and the docs use application/octet-stream. */
class PlainPostUpdateGuard : public AsyncWebHandler {
public:
    bool canHandle(AsyncWebServerRequest *req) const override
    {
        return req->method() == HTTP_POST && req->url() == "/update" &&
               req->contentType().startsWith("application/x-www-form-urlencoded");
    }
    void handleRequest(AsyncWebServerRequest *req) override
    {
        req->send(400, "text/plain",
                  "body would be parsed as a form and exhaust RAM; resend as:\n"
                  "curl -H 'Content-Type: application/octet-stream' "
                  "--data-binary @firmware.bin http://<ip>/update\n");
    }
};

void EasyOTAClass::begin(const char *appInfo, uint16_t port)
{
    /* We survived until here: keep this image across reboots. */
    esp_ota_mark_app_valid_cancel_rollback();

    _info = appInfo;
    _port = port;
    _server = new AsyncWebServer(port);
    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *req) { handleRoot(req); });
    _server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *req) { handleStatus(req); });
    _server->addHandler(new PlainPostUpdateGuard()); /* must precede /update */
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
    _server->on("/reboot", HTTP_POST, [this](AsyncWebServerRequest *req) {
        sendActionPage(req, "OK, rebooting");
        scheduleReboot();
    });
    _server->begin();

    xTaskCreate(wdEntry, "easyota_wd", 4096, this, 5, NULL);
}

void EasyOTAClass::handle()
{
    /* survived long enough: a later crash streak counts from zero */
    if (!_stableMarked && millis() > GUARD_STABLE_MS) {
        _stableMarked = true;
        guardCrashes = 0;
    }
    if (_rebootPending && (int32_t)(millis() - _rebootAt) >= 0) {
        if (_onReboot) _onReboot();
        ESP.restart();
    }
}

void EasyOTAClass::scheduleReboot()
{
    _rebootAt = millis() + 750; /* let the response drain first */
    _rebootPending = true;
}

/* ------------------------------------------------------------------ */
/* Responses to actions that reboot the board. curl gets plain text;   */
/* browsers get a page that polls until the board answers again and    */
/* then reloads the GUI.                                               */
/* ------------------------------------------------------------------ */

static bool wantsHtml(AsyncWebServerRequest *req)
{
    return req->hasHeader("Accept") &&
           req->getHeader("Accept")->value().indexOf("text/html") >= 0;
}

void EasyOTAClass::sendActionPage(AsyncWebServerRequest *req, const String &msg)
{
    if (!wantsHtml(req)) {
        req->send(200, "text/plain", msg + "\n");
        return;
    }
    /* first poll after 2 s: the reboot fires at +750 ms, so by then a
     * response can only come from the freshly booted image */
    req->send(200, "text/html",
              "<!DOCTYPE html><html><body><h1>" + htmlEscape(msg) + "</h1>"
              "<p>Waiting for the board to come back&hellip;</p>"
              "<script>async function poll(){"
              "try{await fetch('/status',{cache:'no-store',signal:AbortSignal.timeout(1500)});"
              "location.replace('/');}"
              "catch(e){setTimeout(poll,1000);}}"
              "setTimeout(poll,2000);</script></body></html>");
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
Heap:       %HEAP_FREE% KiB free of %HEAP_SIZE% KiB (%HEAP_PCT% used; min. ever %HEAP_MIN%, largest block %HEAP_BLOCK%)
Flash:      app uses %FLASH_USED% KiB of %FLASH_SLOT% KiB slot (%FLASH_PCT% used)
Tasks:      %TASKS% (loop stack headroom %STACK% B)
Last reset: %LAST_RESET%
%DIAG%
        </pre>

        <h2>Upload Firmware</h2>
        <form method='POST' action='/update-form' enctype='multipart/form-data'>
            <input type='file' name='fw'> <input type='submit' value='Flash'>
        </form>
        <p>
            This board keeps two application partitions: the running image and a spare.<br>
            Uploading firmware writes the spare and reboots into it.<br>
            If the new application crashes before validating itself, the bootloader<br>
            rolls back to the previous image; images that break later are caught by<br>
            a crash-loop guard and a reachability watchdog, so a bad upload cannot<br>
            lock you out of the board.
        </p>

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
        <form method='POST' action='/boot'>
            <select name='part'>%BOOT_OPTS%</select>
            <input type='submit' value='Boot Selected Slot'>
        </form>
    </body>
</html>
)html";

/* Substitution is a single streaming pass over the template; returned values
 * are never re-scanned, so a stored "%TOKEN%" in hostname/ssid is inert. */
String EasyOTAClass::rootToken(const String &var)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (var == "HOST" || var == "CFG_HOST") return tmplEscape(hostname());
    if (var == "INFO")       return tmplEscape(_info);
    if (var == "CFG_SSID")   return tmplEscape(wifiSsid());
    if (var == "SHA")        return runningSha();
    if (var == "PART")       return running ? running->label : "?";
    if (var == "SLOTS")      return slotList();
    if (var == "UPTIME")     return String(millis() / 1000);
    if (var == "HEAP_FREE")  return String(ESP.getFreeHeap() / 1024);
    if (var == "HEAP_SIZE")  return String(ESP.getHeapSize() / 1024);
    if (var == "HEAP_PCT")   return tmplEscape(pct(ESP.getHeapSize() - ESP.getFreeHeap(), ESP.getHeapSize()));
    if (var == "HEAP_MIN")   return String(ESP.getMinFreeHeap() / 1024);
    if (var == "HEAP_BLOCK") return String(ESP.getMaxAllocHeap() / 1024);
    if (var == "FLASH_USED") return String(ESP.getSketchSize() / 1024);
    if (var == "FLASH_SLOT") return String(running ? running->size / 1024 : 0);
    if (var == "FLASH_PCT")  return tmplEscape(pct(ESP.getSketchSize(), running ? running->size : 0));
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
            out += "Last crash: task '" + tmplEscape(c.task) + "' at " + c.pc +
                   ", cause " + String(c.cause);
            if (*excCauseName(c.cause)) out += " " + String(excCauseName(c.cause));
            out += "\n             backtrace: " + c.bt + "  (crashing build " + c.elf + ")";
        }
        if (out.endsWith("\n")) out.remove(out.length() - 1);
        return out;
    }
    if (var == "BOOT_OPTS") {
        /* one <option> per slot: state + build sha; the persistent boot
         * target is preselected. Rollback/safeguards still override the
         * selection if the chosen image fails. */
        const esp_partition_t *boot = esp_ota_get_boot_partition();
        String out;
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                         ESP_PARTITION_SUBTYPE_ANY, NULL);
        for (; it != NULL; it = esp_partition_next(it)) {
            const esp_partition_t *p = esp_partition_get(it);
            out += "<option value='" + String(p->label) + "'";
            if (p == boot) out += " selected";
            out += ">" + String(p->label) + " - " + otaStateName(p);
            esp_app_desc_t d;
            if (esp_ota_get_partition_description(p, &d) == ESP_OK)
                out += ", " + sha256Hex(d.app_elf_sha256).substring(0, 8);
            else
                out += ", empty";
            if (p == running) out += " (running)";
            out += "</option>";
        }
        esp_partition_iterator_release(it);
        return out;
    }
    return String();
}

void EasyOTAClass::handleRoot(AsyncWebServerRequest *req)
{
    req->send(200, "text/html", ROOT_TMPL,
              [this](const String &var) { return rootToken(var); });
}

/* ---------------------------------------------- */
/* GET /status - everything, machine-readable     */
/* ---------------------------------------------- */

void EasyOTAClass::handleStatus(AsyncWebServerRequest *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    /* identity: elf_sha256 is the per-build hash the toolchain embeds in
     * every image; the same bytes sit at offset 176 of the .bin, so a client
     * (flash.sh) can prove the exact image it uploaded is what runs now */
    String s = "{\"api\":2,";
    s += "\"info\":\"" + jsonEscape(_info) + "\",";
    s += "\"partition\":\"" + String(running ? running->label : "?") + "\",";
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    s += "\"boot_partition\":\"" + String(boot ? boot->label : "?") + "\",";
    s += "\"elf_sha256\":\"" + runningSha() + "\",";
    s += "\"uptime_s\":" + String(millis() / 1000) + ",";

    /* safeguard state (see crash-loop guard / reachability watchdog) */
    s += "\"guard\":{\"crash_resets\":" + String(guardCrashes) +
         ",\"wd_stage\":" + String(guardWdStage) + "},";

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
        s += otaStateName(p);
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

void EasyOTAClass::handleConfig(AsyncWebServerRequest *req)
{
    Preferences prefs;
    if (!prefs.begin("easyota", false)) {
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
    sendActionPage(req, "config saved, rebooting");
    scheduleReboot();
}

/* ------------------------------------------------------------------ */
/* firmware upload: raw body (curl) and multipart (GUI form)           */
/* ------------------------------------------------------------------ */

/* Reject an image that cannot fit the spare slot before burning any flash,
 * with a message that names both sizes. bodySize is exact for raw uploads;
 * for multipart it is the whole request, a close upper bound of the file. */
bool EasyOTAClass::updateTooBig(size_t bodySize, size_t slack)
{
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst || bodySize <= dst->size + slack) return false;
    _updateErr = "image (" + String(bodySize - slack) + " B) does not fit OTA slot " +
                 dst->label + " (" + String(dst->size) + " B)";
    Serial.printf("[EasyOTA] update rejected: %s\n", _updateErr.c_str());
    return true;
}

void EasyOTAClass::handleUpdateData(AsyncWebServerRequest *req, uint8_t *data,
                                    size_t len, size_t index, size_t total)
{
    if (index == 0) {
        _updateErr = "";
        if (updateTooBig(total, 0)) return;
        Serial.println("[EasyOTA] update started");
        if (Update.isRunning()) Update.abort(); /* client of a previous upload vanished */
        Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (_updateErr.length()) return;
    if (!Update.hasError()) Update.write(data, len);
    if (index + len == total) {
        Update.end(true);
        Serial.printf("[EasyOTA] update finished, %u bytes\n", (unsigned)total);
    }
}

void EasyOTAClass::handleUpdateForm(AsyncWebServerRequest *req, const String &filename,
                                    size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        _updateErr = "";
        /* multipart framing adds < 1 KiB on top of the file itself */
        if (updateTooBig(req->contentLength(), 4096)) return;
        Serial.printf("[EasyOTA] form update started: %s\n", filename.c_str());
        if (Update.isRunning()) Update.abort();
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (_updateErr.length()) return;
    if (!Update.hasError()) Update.write(data, len);
    if (final) {
        Update.end(true);
        Serial.printf("[EasyOTA] form update finished, %u bytes\n", (unsigned)(index + len));
    }
}

void EasyOTAClass::handleUpdateDone(AsyncWebServerRequest *req)
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
    sendActionPage(req, "OK: update written, rebooting into it");
    scheduleReboot();
}

/* ------------------------------------------------------------------ */

void EasyOTAClass::handleBoot(AsyncWebServerRequest *req)
{
    /* query string (curl) or form body (the GUI's slot selector) */
    String part;
    if (req->hasParam("part"))
        part = req->getParam("part")->value();
    else if (req->hasParam("part", true))
        part = req->getParam("part", true)->value();
    if (!part.length()) {
        req->send(400, "text/plain", "use /boot?part=ota_0|ota_1\n");
        return;
    }
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
    sendActionPage(req, "OK, rebooting into " + part);
    scheduleReboot();
}

