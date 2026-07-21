#include "EasyOTA.h"
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include "sdkconfig.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "lwip/sockets.h"
#include <time.h>
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#endif
/* Free-running RTC counter (us since power-on): keeps ticking across a panic
 * reset - so it can time the crashed boot - and, unlike the wall clock, is
 * immune to the NTP step in begin(), so the delta needs no NTP. */
#if __has_include("esp_private/esp_clk.h")
#include "esp_private/esp_clk.h"
#define EASYOTA_HAS_RTC_CLK 1
#endif

static inline uint64_t rtcNowUs()
{
#ifdef EASYOTA_HAS_RTC_CLK
    return esp_clk_rtc_time();
#else
    return 0; /* clock unavailable -> crash uptime marker is simply omitted */
#endif
}

EasyOTAClass EasyOTA;

#define EASYOTA_PREFIX "/easy-ota"

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
#ifndef EASYOTA_NTP_SERVER
#define EASYOTA_NTP_SERVER "pool.ntp.org"
#endif
/* epochs below this (Sep 2001) mean the clock still counts from 1970,
 * i.e. was never NTP-synced since power-on */
#define TIME_VALID_MIN 1000000000

RTC_NOINIT_ATTR static uint32_t guardMagic;
RTC_NOINIT_ATTR static uint32_t guardCrashes;
RTC_NOINIT_ATTR static uint32_t guardWdStage;
/* wall-clock stamp of the last crash, waiting to be persisted to NVS by the
 * next begin() - which may run in the OTHER image after a rollback, hence
 * the handoff through RTC memory */
RTC_NOINIT_ATTR static uint32_t crashStampMagic;
RTC_NOINIT_ATTR static uint32_t crashStampEpoch;

/* Boot timeline: millis()-relative stamps of the lifecycle events of THIS
 * boot (net up, server up, app events via EasyOTA.event()), kept in RTC
 * memory so that after a crash the next boot - possibly the other image -
 * can report how far the crashed boot got and when. bootlogHbMs is bumped
 * by every handle() as a cheap "loop was still alive at" marker (a lower
 * bound on the crash moment: within one loop iteration if loop() itself
 * crashed, but stale if some other task did). The crash moment itself is
 * timed after the fact from the RTC counter (bootRtcUs), giving an upper
 * bound - the two bracket it. */
#define BOOTLOG_MAGIC 0x52474432 /* "RGD2" */
#define BOOTLOG_MAX 16
#define BOOTLOG_TAGLEN 15
RTC_NOINIT_ATTR static uint32_t bootlogMagic;
RTC_NOINIT_ATTR static uint32_t bootlogCount;
RTC_NOINIT_ATTR static uint32_t bootlogHbMs;
/* RTC-counter value (us) at this boot's start. The crashed boot's value lets
 * the next boot compute how long it ran before crashing. */
RTC_NOINIT_ATTR static uint64_t bootRtcUs;
RTC_NOINIT_ATTR static struct {
    uint32_t ms;
    char tag[BOOTLOG_TAGLEN + 1];
} bootlogEv[BOOTLOG_MAX];

/* the crashed boot's serialized timeline, captured by the constructor
 * before the live log is reset; persisted to NVS by begin() */
static char crashTimeline[512];

/* Global-constructor time, i.e. before setup() - so a validated image
 * that crashes even in setup() still gets counted and escaped from. */
EasyOTAClass::EasyOTAClass()
{
    if (guardMagic != GUARD_MAGIC) { /* power-on: RTC RAM is garbage */
        guardMagic = GUARD_MAGIC;
        guardCrashes = 0;
        guardWdStage = 0;
        crashStampMagic = 0;
        bootlogMagic = 0;
    }
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
        r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
        /* system time lives in the RTC domain and keeps running across a
         * panic reset, so "now" is within ~1 s of the moment of the crash;
         * 0 records "crashed, but the clock was never synced" */
        time_t now = time(NULL);
        crashStampMagic = GUARD_MAGIC;
        crashStampEpoch = now > TIME_VALID_MIN ? (uint32_t)now : 0;
        /* keep the crashed boot's event sequence before the log restarts
         * (no Strings here: global ctors of other units may not have run) */
        if (bootlogMagic == BOOTLOG_MAGIC) {
            /* -32: room for one entry, keeps off < sizeof so the size_t
             * subtractions below can't underflow */
            size_t off = 0;
            for (uint32_t i = 0; i < bootlogCount && i < BOOTLOG_MAX &&
                                 off < sizeof(crashTimeline) - 32; i++)
                off += snprintf(crashTimeline + off, sizeof(crashTimeline) - off,
                                "%s%s@%ums", off ? " " : "",
                                bootlogEv[i].tag, (unsigned)bootlogEv[i].ms);
            if (off < sizeof(crashTimeline) - 32)
                off += snprintf(crashTimeline + off, sizeof(crashTimeline) - off,
                                "%slast-alive@%ums", off ? " " : "", (unsigned)bootlogHbMs);
            /* the crash itself: uptime of the crashed boot from the RTC
             * counter (bootRtcUs was stamped at its start). Measured here,
             * so it includes the panic+reboot latency to reach this ctor -
             * an upper bound, a few hundred ms over the true moment. */
            uint64_t now_us = rtcNowUs();
            if (off < sizeof(crashTimeline) - 32 && bootRtcUs && now_us > bootRtcUs)
                snprintf(crashTimeline + off, sizeof(crashTimeline) - off,
                         "%scrash@%ums", off ? " " : "",
                         (unsigned)((now_us - bootRtcUs) / 1000));
        }
        if (++guardCrashes >= GUARD_CRASH_LIMIT) {
            guardCrashes = 0; /* don't re-fire every boot if rollback is impossible */
            esp_rom_printf("[EasyOTA] %d crash resets in a row, rolling back\n",
                           GUARD_CRASH_LIMIT);
            esp_ota_mark_app_invalid_rollback_and_reboot(); /* no return on success */
            esp_rom_printf("[EasyOTA] rollback impossible (no valid other slot)\n");
        }
    }
    /* fresh timeline for this boot; stamp the RTC counter as its zero so a
     * later crash can be timed against it */
    bootlogMagic = BOOTLOG_MAGIC;
    bootlogCount = 0;
    bootlogHbMs = 0;
    bootRtcUs = rtcNowUs();
    event("boot");
}

/* Record a named event at the current relative-to-boot time. The count is
 * bumped last so a crash mid-call can't expose a half-written entry. */
void EasyOTAClass::event(const char *tag)
{
    if (bootlogMagic == BOOTLOG_MAGIC && bootlogCount < BOOTLOG_MAX) {
        bootlogEv[bootlogCount].ms = millis();
        strlcpy(bootlogEv[bootlogCount].tag, tag, BOOTLOG_TAGLEN + 1);
        bootlogCount++;
    }
    bootlogHbMs = millis();
}

/* The live timeline of this boot, "boot@2ms net-up@1204ms ..." */
static String timelineNow()
{
    String t;
    for (uint32_t i = 0; i < bootlogCount && i < BOOTLOG_MAX; i++) {
        if (t.length()) t += " ";
        t += String(bootlogEv[i].tag) + "@" + String(bootlogEv[i].ms) + "ms";
    }
    return t;
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
        const char req[] = "GET " EASYOTA_PREFIX "/status HTTP/1.0\r\n\r\n";
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

static String hexAddr(uint32_t a)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "0x%08x", (unsigned)a);
    return String(buf);
}

static String fmtUtc(uint32_t epoch)
{
    time_t t = epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return String(buf) + " UTC";
}

static String fmtDuration(uint32_t s)
{
    if (s >= 86400) return String(s / 86400) + "d " + String(s % 86400 / 3600) + "h";
    if (s >= 3600)  return String(s / 3600) + "h " + String(s % 3600 / 60) + "m";
    if (s >= 60)    return String(s / 60) + "m " + String(s % 60) + "s";
    return String(s) + "s";
}

/* Crash timestamp persisted by begin(); 0 = none recorded / clock was
 * never synced when it happened. */
static uint32_t crashEpochStored()
{
    Preferences prefs;
    if (!prefs.begin("easyota", true)) return 0;
    uint32_t v = prefs.getULong("crashtime", 0);
    prefs.end();
    return v;
}

/* The crashed boot's event timeline persisted by begin(); "" = none. */
static String crashLogStored()
{
    Preferences prefs;
    if (!prefs.begin("easyota", true)) return String();
    String v = prefs.getString("crashlog", "");
    prefs.end();
    return v;
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

/* Xtensa EXCCAUSE values, phrased for humans; the CamelCase term in
 * parentheses is what the IDF panic handler prints, i.e. the googleable one. */
static const char *excCauseName(uint32_t cause)
{
    switch (cause) {
    case 0:  return "illegal instruction (corrupt code or wild jump)";
    case 2:  return "instruction fetch error";
    case 3:  return "load/store to an address the bus rejects (LoadStoreError)";
    case 6:  return "integer divide by zero";
    case 8:  return "privileged instruction";
    case 9:  return "unaligned load/store";
    case 20: return "jump to an invalid address (InstrFetchProhibited)";
    case 28: return "read from an invalid address, e.g. NULL deref (LoadProhibited)";
    case 29: return "write to an invalid address, e.g. NULL deref (StoreProhibited)";
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
    String task, pc, elf; /* elf: PREFIX of the crashing build's sha256 */
    uint32_t cause = 0, vaddr = 0;
    uint32_t bt[16] = { 0 };
    uint32_t depth = 0;
    bool corrupted = false;
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
    c.pc = hexAddr(sum.exc_pc);
    c.depth = sum.exc_bt_info.depth;
    if (c.depth > 16) c.depth = 16;
    for (uint32_t i = 0; i < c.depth; i++)
        c.bt[i] = sum.exc_bt_info.bt[i];
    c.corrupted = sum.exc_bt_info.corrupted;
    c.elf = (const char *)sum.app_elf_sha256;
    c.cause = sum.ex_info.exc_cause;
    c.vaddr = sum.ex_info.exc_vaddr;
#endif
    return c;
}

/* Single-line form for /easy-ota/status: flash.sh greps crash_bt as one string. */
static String backtraceOneLine(const CrashInfo &c)
{
    String bt;
    for (uint32_t i = 0; i < c.depth; i++) {
        if (bt.length()) bt += " ";
        bt += hexAddr(c.bt[i]);
    }
    if (c.corrupted) bt += " (corrupted)";
    return bt;
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

String EasyOTAClass::hostname(const char *def)
{
    /* Default: unique per board via the factory-burned base MAC. Only the
     * last three bytes vary (the first three are Espressif's OUI). */
    static char dh[24];
    if (!def) {
        if (!dh[0]) {
            uint8_t mac[6];
            esp_efuse_mac_get_default(mac);
            snprintf(dh, sizeof dh, "esp32-easyota-%02x%02x%02x",
                     mac[3], mac[4], mac[5]);
        }
        def = dh;
    }
    return configValue("hostname", def);
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
        return req->method() == HTTP_POST && req->url() == EASYOTA_PREFIX "/update" &&
               req->contentType().startsWith("application/x-www-form-urlencoded");
    }
    void handleRequest(AsyncWebServerRequest *req) override
    {
        req->send(400, "text/plain",
                  "body would be parsed as a form and exhaust RAM; resend as:\n"
                  "curl -H 'Content-Type: application/octet-stream' "
                  "--data-binary @firmware.bin http://<ip>" EASYOTA_PREFIX "/update\n");
    }
};

void EasyOTAClass::begin(const char *appInfo, uint16_t port)
{
    /* We survived until here: keep this image across reboots. */
    esp_ota_mark_app_valid_cancel_rollback();
    event("validated");

    /* wall clock via SNTP (retries in the background until the net answers);
     * feeds the status page clock and the crash timestamps */
    configTime(0, 0, EASYOTA_NTP_SERVER);

    /* a crash stamp left by the constructor - possibly by the image that
     * crashed, before a rollback - moves to NVS here so it survives
     * power-off, like the core dump it belongs to */
    if (crashStampMagic == GUARD_MAGIC) {
        crashStampMagic = 0;
        Preferences prefs;
        if (prefs.begin("easyota", false)) {
            prefs.putULong("crashtime", crashStampEpoch);
            /* the timeline the constructor captured belongs to the same
             * crash; empty (e.g. RTC layout of the crashed build differs)
             * removes the stale one rather than mislabeling it */
            crashTimeline[0] ? (void)prefs.putString("crashlog", crashTimeline)
                             : (void)prefs.remove("crashlog");
            prefs.end();
        }
    }

    _info = appInfo;
    _port = port;
    _server = new AsyncWebServer(port);
    /* All endpoints sit under EASYOTA_PREFIX. The dashboard is the bare prefix
     * ("/easy-ota"), but ESPAsyncWebServer's default matcher is prefix-with-/
     * (BackwardCompatible): "/easy-ota" would also match "/easy-ota/status" &
     * co. So the specific sub-routes are registered first and win by order;
     * the dashboard is registered last as the fall-through for the prefix. */
    _server->on(EASYOTA_PREFIX "/status", HTTP_GET,
                [this](AsyncWebServerRequest *req) { handleStatus(req); });
    _server->addHandler(new PlainPostUpdateGuard()); /* must precede /update */
    _server->on(EASYOTA_PREFIX "/update", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleUpdateDone(req); },
                nullptr,
                [this](AsyncWebServerRequest *req, uint8_t *data, size_t len,
                       size_t index, size_t total) {
                    handleUpdateData(req, data, len, index, total);
                });
    _server->on(EASYOTA_PREFIX "/update-form", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleUpdateDone(req); },
                [this](AsyncWebServerRequest *req, const String &filename, size_t index,
                       uint8_t *data, size_t len, bool final) {
                    handleUpdateForm(req, filename, index, data, len, final);
                });
    _server->on(EASYOTA_PREFIX "/config", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleConfig(req); });
    _server->on(EASYOTA_PREFIX "/boot", HTTP_POST,
                [this](AsyncWebServerRequest *req) { handleBoot(req); });
    _server->on(EASYOTA_PREFIX "/reboot", HTTP_POST, [this](AsyncWebServerRequest *req) {
        sendActionPage(req, "OK, rebooting");
        scheduleReboot();
    });
    _server->on(EASYOTA_PREFIX, HTTP_GET,
                [this](AsyncWebServerRequest *req) { handleRoot(req); });
    _server->begin();
    event("http-up");

    /* Advertise _easyota._tcp via mDNS so tools can discover boards without
     * knowing IP or hostname (flash.sh does; or:
     * avahi-browse -rt _easyota._tcp). TXT records identify the board when
     * several answer. */
    if (MDNS.begin(hostname().c_str())) {
        MDNS.addService("easyota", "tcp", port);
        MDNS.addServiceTxt("easyota", "tcp", "info", _info.c_str());
        MDNS.addServiceTxt("easyota", "tcp", "sha",
                           runningSha().substring(0, 12).c_str());
        MDNS.addServiceTxt("easyota", "tcp", "part",
                           esp_ota_get_running_partition()->label);
        event("mdns-up");
    } else {
        Serial.println("easyota: mDNS failed to start");
    }

    xTaskCreate(wdEntry, "easyota_wd", 4096, this, 5, NULL);
}

void EasyOTAClass::handle()
{
    bootlogHbMs = millis(); /* the loop was alive at this uptime */
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
    event("reboot-sched");
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
              "try{await fetch('" EASYOTA_PREFIX "/status',{cache:'no-store',signal:AbortSignal.timeout(1500)});"
              "location.replace('" EASYOTA_PREFIX "');}"
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
App:                    %INFO%
SHA-256:                %SHA%
Partition:              %PART%
Slots:                  %SLOTS%
Uptime:                 %UPTIME% s
Timeline:               %TIMELINE%
Clock:                  %CLOCK%
Heap:                   %HEAP_FREE% KiB free of %HEAP_SIZE% KiB (%HEAP_PCT% used; min. ever %HEAP_MIN%, largest block %HEAP_BLOCK%)
Flash:                  app uses %FLASH_USED% KiB of %FLASH_SLOT% KiB slot (%FLASH_PCT% used)
Tasks:                  %TASKS% (loop stack headroom %STACK% B)
Reason for last reset:  %LAST_RESET%
%DIAG%
        </pre>

        <h2>Upload Firmware</h2>
        <form method='POST' action='%OTA%/update-form' enctype='multipart/form-data' id='fwform'>
            <input type='file' name='fw'> <input type='submit' value='Flash'>
        </form>
        <p id='fwprog' style='display:none'>
            <progress id='fwbar' max='1000' value='0'></progress> <span id='fwpct'></span>
        </p>
        <script>
        /* XHR instead of a plain submit, only to render upload progress: the
           device flashes each chunk before ACKing more data (TCP
           backpressure), so sent bytes track the actual flash progress.
           Without JS the form still submits normally, just without a bar.
           NOTE: no literal percent signs in this template - they would pair
           with the substitution placeholders. */
        document.getElementById('fwform').addEventListener('submit', function(ev) {
            if (!this.fw.files.length) return; /* let the plain submit 400 */
            ev.preventDefault();
            var x = new XMLHttpRequest();
            var bar = document.getElementById('fwbar');
            var pct = document.getElementById('fwpct');
            x.open('POST', '%OTA%/update-form');
            x.setRequestHeader('Accept', 'text/html'); /* want the action page */
            x.upload.onprogress = function(e) {
                if (!e.lengthComputable) return;
                document.getElementById('fwprog').style.display = '';
                bar.value = Math.round(1000 * e.loaded / e.total);
                pct.textContent = Math.round(e.loaded / 1024) + ' / ' +
                                  Math.round(e.total / 1024) + ' KiB';
            };
            /* response is the usual poll-until-back action page (or an
               error page): hand the document over to it either way */
            x.onload = function() {
                document.open();
                document.write(x.responseText);
                document.close();
            };
            x.onerror = function() {
                document.getElementById('fwprog').style.display = '';
                pct.textContent = 'upload failed';
            };
            x.send(new FormData(this));
        });
        </script>
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

        <form method='POST' action='%OTA%/config'>
        <table>
            <tr><td>Hostname</td><td><input name='hostname' value='%CFG_HOST%'></td></tr>
            <tr><td>WiFi SSID</td><td><input name='ssid' value='%CFG_SSID%'></td></tr>
            <tr><td>WiFi Password</td><td><input type='password' name='pass' placeholder='(unchanged)'></td></tr>
        </table>
        <input type='submit' value='Save &amp; Reboot'>
        </form>

        <h2>Actions</h2>
        <form method='POST' action='%OTA%/reboot'><input type='submit' value='Reboot'></form>
        <form method='POST' action='%OTA%/boot'>
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

    if (var == "OTA")        return EASYOTA_PREFIX; /* endpoint prefix for the forms */
    if (var == "HOST" || var == "CFG_HOST") return tmplEscape(hostname());
    if (var == "INFO")       return tmplEscape(_info);
    if (var == "CFG_SSID")   return tmplEscape(wifiSsid());
    if (var == "SHA")        return runningSha();
    if (var == "PART")       return running ? running->label : "?";
    if (var == "SLOTS")      return slotList();
    if (var == "UPTIME")     return String(millis() / 1000);
    if (var == "TIMELINE")   return tmplEscape(timelineNow());
    if (var == "CLOCK") {
        time_t now = time(NULL);
        return now > TIME_VALID_MIN ? fmtUtc((uint32_t)now)
                                    : String("not NTP-synced yet");
    }
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
            out += "Rolled back:            " + slot + " holds an aborted image (sha " +
                   sha.substring(0, 16) + "...)\n";
        String clog = crashLogStored();
        if (clog.length())
            out += "Crashed boot's events:  " + tmplEscape(clog) + "\n";
        CrashInfo c = crashInfo();
        if (c.valid) {
            out += "Last crash:             task '" + tmplEscape(c.task) + "' at " + c.pc +
                   ", cause " + String(c.cause);
            if (*excCauseName(c.cause)) out += ": " + String(excCauseName(c.cause));
            out += ", vaddr " + hexAddr(c.vaddr) + "\n";
            uint32_t ce = crashEpochStored();
            time_t now = time(NULL);
            out += "Crash time:             ";
            if (ce) {
                out += fmtUtc(ce);
                if (now > TIME_VALID_MIN && (uint32_t)now >= ce)
                    out += " (" + fmtDuration((uint32_t)now - ce) + " ago)";
            } else {
                out += "unknown (clock was not NTP-synced when it happened)";
            }
            out += "\n";
            /* the core dump stores only the leading chars of the build sha;
             * the "..." marks it as a prefix of the full hashes shown above */
            out += "Crashing build:         " +
                   (c.elf.length() ? c.elf : String("?")) + "...";
            out += (c.elf.length() && runningSha().startsWith(c.elf))
                       ? " (this build)" : " (a previous build)";
            out += "\nBacktrace:              ";
            for (uint32_t i = 0; i < c.depth; i++) {
                if (i) out += "\n                        ";
                out += hexAddr(c.bt[i]);
            }
            if (c.corrupted) out += "\n                        (corrupted)";
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
/* GET /easy-ota/status - everything, machine-readable */
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
    s += "\"timeline\":\"" + jsonEscape(timelineNow()) + "\",";
    time_t now = time(NULL);
    s += "\"time\":" + String(now > TIME_VALID_MIN ? (uint32_t)now : 0) + ","; /* 0 = not synced */

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
    String clog = crashLogStored();
    if (clog.length())
        s += "\"crash_timeline\":\"" + jsonEscape(clog) + "\",";
    CrashInfo crash = crashInfo();
    if (crash.valid) {
        s += "\"crash_task\":\"" + jsonEscape(crash.task) + "\",";
        s += "\"crash_pc\":\"" + crash.pc + "\",";
        s += "\"crash_cause\":" + String(crash.cause) + ",";
        if (*excCauseName(crash.cause))
            s += "\"crash_cause_name\":\"" + String(excCauseName(crash.cause)) + "\",";
        s += "\"crash_vaddr\":\"" + hexAddr(crash.vaddr) + "\",";
        s += "\"crash_time\":" + String(crashEpochStored()) + ","; /* epoch, 0 = unknown */
        s += "\"crash_elf\":\"" + crash.elf + "\",";
        s += "\"crash_bt\":\"" + backtraceOneLine(crash) + "\",";
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
/* POST /easy-ota/config                                               */
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

/* Two uploads must not interleave chunks into one Update session. A second
 * upload while the owner is still actively writing is rejected (marked via
 * the request's _tempObject: a malloc'd flag the server frees with the
 * request); a session whose owner went quiet - the client vanished
 * mid-upload - is taken over instead, so an aborted curl doesn't lock the
 * board's updater until reboot. */
#define UPDATE_STALE_MS 10000
bool EasyOTAClass::updateBusy(AsyncWebServerRequest *req)
{
    if (Update.isRunning() && req != _updateReq &&
        millis() - _updateLastMs < UPDATE_STALE_MS) {
        req->_tempObject = malloc(1);
        Serial.println("[EasyOTA] update rejected: another one is in progress");
        return true;
    }
    if (Update.isRunning()) {
        Serial.println("[EasyOTA] stale update session (client vanished), taking over");
        Update.abort();
    }
    _updateReq = req;
    _updateLastMs = millis();
    return false;
}

void EasyOTAClass::handleUpdateData(AsyncWebServerRequest *req, uint8_t *data,
                                    size_t len, size_t index, size_t total)
{
    if (index == 0) {
        if (updateBusy(req)) return;
        _updateErr = "";
        if (updateTooBig(total, 0)) return;
        Serial.println("[EasyOTA] update started");
        event("update-start");
        Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (req->_tempObject || _updateErr.length()) return;
    _updateLastMs = millis();
    if (!Update.hasError()) Update.write(data, len);
    if (index + len == total) {
        Update.end(true);
        _updateReq = nullptr;
        event("update-done");
        Serial.printf("[EasyOTA] update finished, %u bytes\n", (unsigned)total);
    }
}

void EasyOTAClass::handleUpdateForm(AsyncWebServerRequest *req, const String &filename,
                                    size_t index, uint8_t *data, size_t len, bool final)
{
    if (index == 0) {
        if (updateBusy(req)) return;
        _updateErr = "";
        /* multipart framing adds < 1 KiB on top of the file itself */
        if (updateTooBig(req->contentLength(), 4096)) return;
        Serial.printf("[EasyOTA] form update started: %s\n", filename.c_str());
        Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
    }
    if (req->_tempObject || _updateErr.length()) return;
    _updateLastMs = millis();
    if (!Update.hasError()) Update.write(data, len);
    if (final) {
        Update.end(true);
        _updateReq = nullptr;
        Serial.printf("[EasyOTA] form update finished, %u bytes\n", (unsigned)(index + len));
    }
}

void EasyOTAClass::handleUpdateDone(AsyncWebServerRequest *req)
{
    if (req->_tempObject) {
        req->send(409, "text/plain",
                  "another update is already in progress; retry when it finishes\n");
        return;
    }
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
        req->send(400, "text/plain", "use " EASYOTA_PREFIX "/boot?part=ota_0|ota_1\n");
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

