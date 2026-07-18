# EasyOTA firmware

Generic ESP32 firmware that stays updatable over the network: after the
initial serial flash, everything goes over ethernet (or WiFi) via HTTP — no
USB/UART needed. Built for the WT32-ETH01, but nothing here is tied to any
particular application; drop your own sketch on top and it stays flashable.

## Architecture

```
flash (4MB)                       partitions_4mb.csv (shared by all builds)
├── bootloader (rollback enabled, never rewritten)
├── ota_0    1.875M <- A/B slots; serial flash puts the base image here,
├── ota_1    1.875M <- every HTTP upload goes to the slot not running
└── coredump 192K   <- crash dump of the most recent panic (diagnostics)
```

Everything is a plain Arduino sketch built on the same library — there is
exactly one implementation of the HTTP API, the OTA logic, and the network
bringup:

- **`lib/EasyOTA`**: the whole framework in one library. Network bringup
  (`EasyOTA.beginNetwork()`, ethernet or WiFi via `-DEASYOTA_USE_ETH` /
  `-DEASYOTA_USE_WIFI`) plus all HTTP endpoints (status, upload, config, slot
  boot, rollback diagnostics), the rollback handshake, and the safeguards
  described below. Built on
  [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer):
  requests are served from the async_tcp task, so a busy `loop()` can't
  stall HTTP; `EasyOTA.handle()` only runs deferred reboots.
- **`base/`**: the minimal image (EasyOTA and nothing else),
  serial-flashed into `ota_0` when a board is first commissioned so it is
  reachable over HTTP from day one. Afterwards it's an ordinary slot
  occupant — later uploads overwrite it.
- **`app/`**: demo app / template. Any Arduino firmware works, as long as it
  includes EasyOTA so it stays updatable.

### Safe flashing

The bootloader is built with app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
the prebuilt arduino-esp32 bootloader ships with it enabled). A freshly
uploaded image boots in "pending verify" state; `EasyOTA.begin()` marks it
valid. If it crashes anywhere before that, the next reset rolls back to the
previous image in the other slot.

EasyOTA overrides the Arduino core's weak `verifyRollbackLater()`. Otherwise,
the core would auto-validate the image before `setup()` even runs, and a
crash in `setup()` would boot-loop forever instead of rolling back.

**Always include EasyOTA.** An app without it validates itself at boot,
serves no endpoints, and can only be replaced via serial.

### Safeguards: escaping bad-but-VALIDATED images

Bootloader rollback only covers images that crash *before* validating. An
image that validates and *then* turns bad (crash loop after an hour, heap
exhaustion, wedged server task) would be booted forever and lock us out of a
board with no UART. EasyOTA adds two escape hatches, both landing on the
previous image in the other slot:

- **Crash-loop guard** (runs before `setup()`): 3 crash resets (panic/WDT)
  in a row without reaching 5 minutes of uptime → the image is marked
  invalid, the bootloader boots the other slot. Counted in RTC memory
  (survives resets, cleared on power-on).
- **Reachability watchdog** (own FreeRTOS task, priority above `loop()`):
  probes the board's own HTTP server through lwIP loopback every 15 s.
  Unreachable for 5 minutes → reboot (cures leaks and wedged tasks); *still*
  unreachable for 5 minutes after that reboot → mark invalid, boot the other
  slot. Never fires while an upload is in progress.

Drills (flash, watch it happen, board comes back on the previous image):
`pio run` with `-DCRASH_TEST` (crash before validation → plain rollback),
`-DCRASH_LOOP_TEST` (validate, then crash-loop → crash guard),
`-DWD_TEST` (validate, then kill the network → watchdog escalation; add
`-DEASYOTA_WD_FAIL_MS=60000` to shorten the drill).

### Rollback diagnostics

A rolled-back flash is not a black box. Four artifacts survive the rollback
and are reported in `/status` (and on the GUI):

- `last_reset` — the crash's reset reason lives in RTC memory: `panic`,
  `interrupt-wdt`, `brownout`, ...
- `aborted_slot` / `aborted_sha` — the rejected image stays in its slot
  marked "aborted"; its per-build SHA identifies *which* build was rejected.
- `crash_task`, `crash_pc`, `crash_cause`, `crash_bt`, `crash_elf` — the
  panic handler writes a core dump to the `coredump` partition; this is its
  summary. `crash_elf` is the crashing build's (truncated) SHA, so stale
  dumps are distinguishable. Decode the backtrace against your build:

  ```sh
  xtensa-esp32-elf-addr2line -e .pio/build/<env>/firmware.elf <crash_bt addresses>
  ```

- `crash_timeline` — the crashed boot's event sequence with
  time-since-boot stamps, e.g.
  `boot@2ms net-begin@66ms net-up@1430ms last-alive@1430ms`: how far
  bringup got and when, plus a `last-alive` heartbeat (bumped by every
  `loop()`) bracketing the crash moment. Recorded in RTC memory, no NTP
  involved. The running boot's sequence is always in `/status` as
  `timeline` (spot slow bringup); apps add their own milestones with
  `EasyOTA.event("sensors-up")` (built-ins: `boot`, `net-begin`,
  `net-up`, `validated`, `http-up`, `mdns-up`, `update-start`,
  `update-done`, `reboot-sched`).

`flash.sh` prints all of this automatically when an upload gets rolled back.
`/status` also reports the safeguard state (`guard.crash_resets`,
`guard.wd_stage`).

### Verified flashing: flash.sh

```sh
app/flash.sh [host] [firmware.bin]   # exit 0: uploaded build verifiably running
```

Without a host it discovers boards via mDNS and always asks which to
flash — also with a single candidate, so a board that silently dropped
off the network can't redirect an upload to the wrong survivor.
Without an explicit `.bin` it asks which environment to build
(enter = `default_envs`), runs `pio run -e <env>` itself (offering to
re-flash the existing image instead, if one is present), and uploads
that image. The env choice is always explicit so a wt32-eth01 image
can't silently end up on the lolin32 dev board (it would
brick-and-roll-back: the ETH bringup watchdog-resets on a board with no
PHY). So a plain `flash.sh` builds, discovers, and flashes.

Uploads, waits for the reboot, then proves *the exact build you uploaded* is
what runs: every ESP32 image embeds a unique per-build SHA-256 (at file
offset 176), which the device reports via `GET /status`. Exit 1 = device
came back with a different image (rolled back — diagnostics get printed),
2 = device never came back.

## HTTP API

```sh
curl http://<ip>/                                     # web GUI: status, rollback
                                                      # diagnostics, upload form, config
                                                      # (JS only to auto-reload the GUI
                                                      # after reboot actions)
curl http://<ip>/status                               # one JSON object with everything
curl -H 'Content-Type: application/octet-stream' \
     --data-binary @firmware.bin http://<ip>/update   # flash other slot + boot it
                                                      # (rejects images > slot size;
                                                      # 409 while another upload is
                                                      # actively running - but a stale
                                                      # session whose client vanished
                                                      # is taken over after 10 s;
                                                      # content type required - curl's
                                                      # form default would be refused)
curl -X POST "http://<ip>/boot?part=ota_0"            # or ota_1
curl -X POST http://<ip>/reboot
curl -d "hostname=device1&ssid=&pass=" http://<ip>/config  # empty = default/unchanged
```

### Adding your own endpoints

The app shares EasyOTA's server instead of running its own:

```cpp
#include <ESPAsyncWebServer.h>

void setup() {
    EasyOTA.beginNetwork();
    EasyOTA.begin("my-app v1");
    EasyOTA.server()->on("/clocks", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", "{\"gpio0\":1000}\n");
    });
}
```

This is safe by construction: handlers match in registration order and
`begin()` registers all EasyOTA routes first, so app routes (even greedy
catch-alls) can never shadow `/update` & co. It is also no less robust
than a second server — *all* ESPAsyncWebServer instances are serviced by
the same `async_tcp` task, so a second port would share every failure
mode anyway — and the reachability watchdog guards the shared server:
if app code renders it unusable, the board reboots and, if that doesn't
cure it, rolls back.

The genuinely isolated alternative is a server from a *different* stack
on its own port (e.g. the synchronous `WebServer.h` driven from
`loop()`): if that one wedges, EasyOTA keeps serving from `async_tcp`
and OTA remains the rescue path.

The device requests the configured hostname via DHCP; the default is
`esp32-easyota-<xxxxxx>` with the last three bytes of the board's
factory MAC, so unconfigured boards get distinct names out of the box.
It also advertises the mDNS service `_easyota._tcp` (TXT: app info, ELF
SHA prefix, running partition) for IP-less discovery:

```sh
avahi-browse -rt _easyota._tcp
```

## Base image: build + initial serial flash (once per board)

```sh
cd base
pio run -e lolin32 -t upload      # dev board on /dev/ttyACM0
pio run -e wt32-eth01 -t upload   # real board, needs UART adapter + IO0 low at reset
```

This flashes bootloader + partition table + blank otadata (so the board
boots `ota_0`, where the base image lands) + the base image.

## App: build + upload over HTTP

```sh
cd app
./flash.sh                        # builds (asks which env), discovers, uploads
```

## Dev setup notes (LOLIN32)

> NOTE: WiFi is only used for testing. The final board *can* use it but doesn't have to.

```sh
cp secrets.ini.example secrets.ini      # WiFi creds, LOLIN32 dev setup only
```

- WiFi instead of ethernet; credentials are baked in at build time from `secrets.ini` (base image *and* demo app) - reflash/re-upload after changing them.
- IO0 tied to GND keeps the chip in the serial bootloader on every reset: fine for flashing, but **lift IO0 to actually run the firmware**.
- Boards flashed with an older partition table (three slots with a `loader`/`factory` partition) need one serial `pio run -t upload` from `base/` — the layout changed to plain A/B on 2026-07-16.

## WT32-ETH01 pinout facts (encoded in the sources)

LAN8720 PHY addr 1, MDC=GPIO23, MDIO=GPIO18, RMII clock in on GPIO0 from the external 50MHz oscillator, oscillator enable on GPIO16.
