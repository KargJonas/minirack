# ESP32 CLKIN test (ADS131M02 master clock)

Bench test to check whether an ESP32 can generate a clean clock on
**GPIO14** to drive the ADS131M02 ADCs on the rack-manager board.

## Decision: fCLKIN = 4.000 MHz (ADC in Low-Power mode)
`4.000 MHz = 80 MHz / 20` is an **exact integer division of the ESP32 APB clock**, so
the LEDC output is **dither-free** (measured below) — no external oscillator needed.
It sits inside TI's Low-Power mode (nominal 4.096 MHz; running 2.3 % slow just scales
the data rate) and gives up to 31 kSPS / ~8 kHz bandwidth — ample for 50/60 Hz mains
metrology with harmonics. Use OSR 512–1024 (~2–4 kSPS, 0.5–1 kHz BW) as the metering
sweet spot. Avoid 8.192/4.096/2.048 MHz: all are non-integer divisions of 80 MHz and
therefore dither (~±12.5 ns).

## Hardware
- ESP32 dev module (Wemos), USB serial on `/dev/ttyUSB0` (CP210x)
- Logic analyzer probe on **GPIO14**, GND common. Saleae-compatible (sigrok `fx2lafw`), 24 MS/s
- LA channel used here: **D0**

## Build / flash (inside the `anvil-dev` distrobox — has pio + network)
```bash
distrobox enter anvil-dev -- bash -lc 'cd tools/esp32-clock-test && pio run -t upload'
```

## Runtime serial console (115200)
The firmware prints a 1 Hz `ALIVE` heartbeat and accepts commands:
- `<number>` — set target frequency in Hz (e.g. `8192000`, `1024000`)
- `A` — APLL / I2S-MCLK method  ⚠ **no output on GPIO14** (see note)
- `L` — LEDC method (default; works on any GPIO)
- `?` — status

Reading serial resets the board via DTR/RTS, so to change frequency *and keep it*
during a capture, hold the port open (see the pyserial snippet in git history) or
just re-flash with a new default.

## Capture + analyze
```bash
python3 analyze_clock.py --driver fx2lafw --samplerate 24000000 \
        --samples 4800000 --expect 4000000
```
Auto-detects the active channel and reports average frequency, ppm error, duty,
period spread and dropout/glitch counts with a pass/fail verdict.

## Key finding: MCLK cannot come out on GPIO14 (classic ESP32)
On the original ESP32, the I2S **MCLK** (the clean, APLL-sourced clock) is routed
through the CLK_OUT mux and is only available on **GPIO0 / GPIO1 / GPIO3** — *not*
through the GPIO matrix. So `mck_io_num = GPIO14` produces **no output**, which is
why the first APLL capture showed a dead line. On GPIO14 the only on-pin option is
**LEDC**, whose 80 MHz/fractional divider *dithers* (~±12.5 ns) because
80 MHz / 8.192 MHz = 9.7656 is non-integer.

## Results (24 MS/s LA, LEDC on GPIO14)
All captures at 24 MS/s. "Cycles" = clock periods observed (rising edges − 1).
Period histogram = fraction of periods measured at each whole-sample length; a clean
clock collapses to a single bin, a dithered one smears across two.

| Freq set | Window / cycles | Avg measured | Error | Duty | Period histogram | Drop/glitch |
|---|---|---|---|---|---|---|
| **4.000 MHz** ✅ | 200 ms / 799,955 | 3.99978 MHz | −54.8 ppm | 50.3 % | **6 smp: 99.91 %** (5:0.03, 7:0.06) | 0 / 0 |
| 2.000 MHz | 200 ms / 399,977 | 1.99989 MHz | −54.8 ppm | 50.2 % | **12 smp: 99.90 %** (11:0.02, 13:0.08) | 0 / 0 |
| 8.192 MHz | 100 ms / 819,155 | 8.19155 MHz | −54.5 ppm | ~50 % | 2 smp / 3 smp smeared (std 10.7 ns) | 0 / 0 |
| 1.024 MHz | 200 ms / 204,788 | 1.02394 MHz | −54.6 ppm | 51.1 % | 23 smp / 24 smp (straddles 976.6 ns) | 0 / 0 |

Interpretation:
- **4.000 & 2.000 MHz are dither-free**: 99.9 % of periods land in one bin (σ ≈ 1.2–1.3 ns).
  The rare ±1-sample slips (~0.1 %) are the crystal-offset phase walk, not jitter.
- **8.192 MHz is inherently smeared** across two sample bins — the LEDC fractional
  divider plus the non-integer 24/8.192 sample ratio.
- The **−54.8 ppm error is identical at every frequency** → fixed offset between the
  ESP32 crystal and the LA crystal, *not* a generation error.
- Zero dropouts and zero runt/glitch pulses across ~0.2–0.8 M cycles per capture.

## Limitation (important)
The LA resolves edges to 41.67 ns, so at 4.000 MHz it sees 6 samples/period (at
8.192 MHz only 2.9). It can confirm the clock is present, on-frequency, ~50 % duty,
dither-free (single-bin period histogram) and free of dropouts/glitches over hundreds
of thousands of cycles — but it **cannot** see sub-42 ns edge quality/ringing, nor
rare events beyond the ~0.2 s capture window (thermal drift, hiccups when other ESP
tasks/WiFi fire). The one quantifiable defect of an ESP-generated clock — LEDC divider
dither — is eliminated by the integer-divisor choice (4.000 MHz), which this LA *can*
show. For final sign-off, a scope confirms edge quality, and a longer capture with the
real firmware load confirms long-term stability.
