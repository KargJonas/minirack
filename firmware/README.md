# minirack firmware

Firmware for the rack-monitor board (WT32-ETH01). No USB/UART on the final
hardware. Everything after the initial flash goes over ethernet via HTTP.

## Architecture

```
flash (4MB)                       partitions_4mb.csv (shared by all builds)
├── bootloader (rollback enabled, never rewritten)
├── ota_0    1.875M <- A/B slots; serial flash puts the base image here,
├── ota_1    1.875M <- every HTTP upload goes to the slot not running
└── coredump 192K   <- crash dump of the most recent panic (diagnostics)
```

Everything is a plain Arduino sketch built on the same two libraries — there
is exactly one implementation of the HTTP API, the OTA logic, and the network
bringup:

- **`lib/RackOTA`**: all HTTP endpoints (status, upload, config, slot boot,
  rollback diagnostics) plus the rollback handshake and the safeguards
  described below. Built on
  [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer):
  requests are served from the async_tcp task, so a busy `loop()` can't
  stall HTTP; `RackOTA.handle()` only runs deferred reboots.
- **`lib/RackNet`**: the one network bringup (`rackNetBegin()`), ethernet or
  WiFi via `-DRACK_USE_ETH` / `-DRACK_USE_WIFI`.
- **`base/`**: the minimal image (network + RackOTA, nothing else),
  serial-flashed into `ota_0` when a board is first commissioned so it is
  reachable over HTTP from day one. Afterwards it's an ordinary slot
  occupant — later uploads overwrite it.
- **`app/`**: demo app / template. Any Arduino firmware works, as long as it
  includes RackOTA so it stays updatable.

### Safe flashing

The bootloader is built with app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
the prebuilt arduino-esp32 bootloader ships with it enabled). A freshly
uploaded image boots in "pending verify" state; `RackOTA.begin()` marks it
valid. If it crashes anywhere before that, the next reset rolls back to the
previous image in the other slot.

RackOTA overrides the Arduino core's weak `verifyRollbackLater()`. Otherwise,
the core would auto-validate the image before `setup()` even runs, and a
crash in `setup()` would boot-loop forever instead of rolling back.

**Always include RackOTA.** An app without it validates itself at boot,
serves no endpoints, and can only be replaced via serial.

### Safeguards: escaping bad-but-VALIDATED images

Bootloader rollback only covers images that crash *before* validating. An
image that validates and *then* turns bad (crash loop after an hour, heap
exhaustion, wedged server task) would be booted forever and lock us out of a
board with no UART. RackOTA adds two escape hatches, both landing on the
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
`-DRACKOTA_WD_FAIL_MS=60000` to shorten the drill).

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
`/status` also reports the safeguard state (`guard.crash_resets`,
`guard.wd_stage`).

### Verified flashing: flash.sh

```sh
./flash.sh <host> firmware.bin   # exit 0: uploaded build verifiably running
```

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
                                                      # content type required - curl's
                                                      # form default would be refused)
curl -X POST "http://<ip>/boot?part=ota_0"            # or ota_1
curl -X POST http://<ip>/reboot
curl -d "hostname=rack1&ssid=&pass=" http://<ip>/config  # empty = default/unchanged
```

The device requests the configured hostname (default `minirack`) via DHCP.

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
pio run -e lolin32                # or -e wt32-eth01
../flash.sh <ip> .pio/build/lolin32/firmware.bin
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
