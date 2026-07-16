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
- **`lib/RackOTA`**: Arduino lib providing the same `/update` endpoint in the   app itself (direct app-to-app updates), `/loader` to get back to the loader, and the rollback handshake.

### Safety net

The bootloader is built with app rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A freshly uploaded app boots in "pending verify" state; `RackOTA.begin()` marks it valid. If an app crashes before that, the next reset rolls back to the previous image (ultimately the loader). Consequence: **an app that doesn't call `RackOTA.begin()` is reverted on its next reboot** - by design.

## HTTP API (same on loader and apps)

```sh
curl http://<ip>/                                     # info / partition states
curl --data-binary @firmware.bin http://<ip>/update   # flash + boot new app
curl -X POST http://<ip>/reboot

# loader only:
curl -X POST "http://<ip>/boot?part=factory"          # or ota_0 / ota_1

# apps only:
curl -X POST http://<ip>/loader                       # reboot into loader
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
