# minirack firmware

Firmware for the rack-monitor board (WT32-ETH01). No USB/UART on the final
hardware. Everything after the initial flash goes over ethernet via HTTP.

## Architecture

```
flash (4MB)                       partitions_4mb.csv (shared by all builds)
├── bootloader (IDF, rollback enabled)
├── factory  1M    <- loader/  ESP-IDF, flashed ONCE over serial
├── ota_0    1.4M  <- apps, uploaded over HTTP (Arduino framework)
└── ota_1    1.4M  <-   "
```

- **`loader/`**: the program is programmed into `factory` flash section. It is never overwritten by OTA updates. The loader brings up network hardware/software and serves an HTTP API endpoint that can be used to easily flash software over the network. This part of the code uses the ESP-IDF framework, which is not required in the user-flashed applications.
- **`app/`**: demo app / template. Any Arduino-framework firmware works, as long as it includes **`lib/RackOTA`** so it stays updatable.
- **`lib/RackOTA`**: Arduino lib providing the same `/update` endpoint in the   app itself (direct app-to-app updates), `/loader` to get back to the loader, and the rollback handshake. Built on [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer): requests are served from the async_tcp task, so a busy `loop()` can't stall HTTP; `RackOTA.handle()` only runs deferred reboots.

### Safe flashing

The bootloader is built with app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A freshly uploaded app boots in "pending verify" state; `RackOTA.begin()` marks it valid. If the app crashes anywhere before that, the next reset rolls back to the previous image (ultimately the loader). Verified end-to-end with a deliberately crashing app (`pio run` with `-DCRASH_TEST`).

RackOTA overrides the Arduino core's weak `verifyRollbackLater()`. Otherwise, the core would auto-validate the image before `setup()` even runs, and a crash in `setup()` would boot-loop forever instead of rolling back.

**Always include RackOTA.** An app without it validates itself at boot, serves no endpoints, and can only be replaced via serial.

### Verified flashing: flash.sh

```sh
./flash.sh <host> firmware.bin   # exit 0: uploaded build successfully running
```

Uploads, waits for the reboot, then proves *the exact build you uploaded* is what runs: every ESP32 image embeds a unique per-build SHA-256 (at file offset 176), which the device reports via `GET /status` (JSON: role, partition, elf_sha256, uptime, slot states). Exit 1 = device came back with a different image (rolled back), 2 = device never came back.

## HTTP API (same on loader and apps)

```sh
curl http://<ip>/status                               # one JSON object: image hash,
                                                      # slot states, memory, config
curl --data-binary @firmware.bin http://<ip>/update   # flash + boot new app
curl -X POST http://<ip>/reboot

# loader only:
curl http://<ip>/                                     # text info page
curl -X POST "http://<ip>/boot?part=factory"          # or ota_0 / ota_1

# apps only:
# GET / serves a tiny no-js web GUI: status, firmware upload form, and
# config (hostname / wifi ssid / wifi password, persisted in NVS; the demo
# app falls back to compiled-in wifi creds if the configured ones fail).
curl -X POST http://<ip>/loader                       # reboot into loader
curl -d "hostname=rack1&ssid=&pass=" http://<ip>/config  # empty = default/unchanged
```

The device requests hostname `minirack-loader` (loader) / `minirack` (app)
via DHCP.

## Loader: build + initial serial flash (once per board)

```sh
cd loader
pio run -e lolin32 -t upload      # dev board on /dev/ttyACM0
pio run -e wt32-eth01 -t upload   # real board, needs UART adapter + IO0 low at reset
```

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

## WT32-ETH01 pinout facts (encoded in the sources)

LAN8720 PHY addr 1, MDC=GPIO23, MDIO=GPIO18, RMII clock in on GPIO0 from the external 50MHz oscillator, oscillator enable on GPIO16.
