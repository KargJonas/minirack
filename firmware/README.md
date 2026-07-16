# minirack firmware

Firmware for the rack-monitor board (WT32-ETH01). No USB/UART on the final
hardware. Everything after the initial flash goes over ethernet via HTTP.

## Architecture

```
flash (4MB)                       partitions_4mb.csv (shared by all builds)
├── bootloader (rollback enabled)
├── loader   1.19M  <- loader/  minimal recovery image, flashed ONCE over serial
├── ota_0    1.31M  <- apps, uploaded over HTTP
├── ota_1    1.31M  <-   "
└── coredump 128K   <- crash dump of the most recent panic (rollback diagnostics)
```

Everything is a plain Arduino sketch built on the same two libraries — there
is exactly one implementation of the HTTP API, the OTA logic, and the network
bringup:

- **`lib/RackOTA`**: all HTTP endpoints (status, upload, config, boot
  selection, rollback diagnostics) plus the rollback handshake. Built on
  [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer):
  requests are served from the async_tcp task, so a busy `loop()` can't stall
  HTTP; `RackOTA.handle()` only runs deferred reboots. Detects at runtime
  whether it runs from the loader partition and adapts (role in `/status`,
  no "reboot into loader" action).
- **`lib/RackNet`**: the one network bringup (`rackNetBegin()`), ethernet or
  WiFi via `-DRACK_USE_ETH` / `-DRACK_USE_WIFI`.
- **`loader/`**: nothing but `rackNetBegin()` + `RackOTA.begin()`. Lives in
  the `loader` partition (subtype `factory` — what the bootloader ultimately
  falls back to), flashed once over serial, never overwritten by OTA.
- **`app/`**: demo app / template. Any Arduino firmware works, as long as it
  includes RackOTA so it stays updatable.

### Safe flashing

The bootloader is built with app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
the prebuilt arduino-esp32 bootloader ships with it enabled). A freshly
uploaded app boots in "pending verify" state; `RackOTA.begin()` marks it
valid. If the app crashes anywhere before that, the next reset rolls back to
the previous image (ultimately the loader).

RackOTA overrides the Arduino core's weak `verifyRollbackLater()`. Otherwise,
the core would auto-validate the image before `setup()` even runs, and a
crash in `setup()` would boot-loop forever instead of rolling back.

**Always include RackOTA.** An app without it validates itself at boot,
serves no endpoints, and can only be replaced via serial.

Note: rollback triggers on *resets* (crash, watchdog, power cycle) — an app
that hangs without crashing keeps hanging until power is cycled, after which
the pending-verify image is rolled back.

### Rollback diagnostics

A rolled-back flash is not a black box. Three artifacts survive the rollback
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

`flash.sh` prints all of this automatically when an upload gets rolled back.

### Verified flashing: flash.sh

```sh
./flash.sh <host> firmware.bin   # exit 0: uploaded build verifiably running
```

Uploads, waits for the reboot, then proves *the exact build you uploaded* is
what runs: every ESP32 image embeds a unique per-build SHA-256 (at file
offset 176), which the device reports via `GET /status`. Exit 1 = device
came back with a different image (rolled back — diagnostics get printed),
2 = device never came back.

## HTTP API (identical on loader and apps)

```sh
curl http://<ip>/                                     # no-js web GUI: status, rollback
                                                      # diagnostics, upload form, config
curl http://<ip>/status                               # one JSON object with everything
curl --data-binary @firmware.bin http://<ip>/update   # flash spare slot + boot it
                                                      # (rejects images > slot size)
curl -X POST "http://<ip>/boot?part=loader"           # or ota_0 / ota_1
curl -X POST http://<ip>/reboot
curl -d "hostname=rack1&ssid=&pass=" http://<ip>/config  # empty = default/unchanged

# apps only (loader hides it — it IS the loader):
curl -X POST http://<ip>/loader                       # = /boot?part=loader
```

`/status` reports `"role": "loader"` or `"app"`, so clients can tell which
answered. Loader and apps request the same configured hostname (default
`minirack`) via DHCP — after a rollback the board stays reachable under the
same name.

## Loader: build + initial serial flash (once per board)

```sh
cd loader
pio run -e lolin32 -t upload      # dev board on /dev/ttyACM0
pio run -e wt32-eth01 -t upload   # real board, needs UART adapter + IO0 low at reset
```

This flashes bootloader + partition table + blank otadata (so the board boots
the loader, not a stale OTA slot) + the loader image.

## App: build + upload over HTTP

```sh
cd app
pio run -e lolin32                # or -e wt32-eth01
curl --data-binary @.pio/build/lolin32/firmware.bin http://<ip>/update
```

Uploads go through either the loader or a running RackOTA app - same command.

## Dev setup notes (LOLIN32)

> NOTE: WiFi is only used for testing. The final board *can* use it but doesn't have to.

```sh
cp secrets.ini.example secrets.ini      # WiFi creds, LOLIN32 dev setup only
```

- WiFi instead of ethernet; credentials are baked in at build time from `secrets.ini` (loader *and* demo app) - reflash/re-upload after changing them.
- IO0 tied to GND keeps the chip in the serial bootloader on every reset: fine for flashing, but **lift IO0 to actually run the firmware**.
- Boards flashed with the pre-2026-07-16 partition table (partition `factory`, other offsets) need one serial `pio run -t upload` from `loader/` — the partitions moved.

## WT32-ETH01 pinout facts (encoded in the sources)

LAN8720 PHY addr 1, MDC=GPIO23, MDIO=GPIO18, RMII clock in on GPIO0 from the external 50MHz oscillator, oscillator enable on GPIO16.
