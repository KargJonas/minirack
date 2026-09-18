# PCA9555 GPIO expander — hardware validation

Logic-analyzer validation of the `gpioexp` driver
([firmware/src/gpioexp.cpp](../../firmware/src/gpioexp.cpp)) driving U6, the
PCA9555PW at address `0x20` on the rack-manager board.

There are two independent tests:

- **Write self-test** (`/gpio-selftest`) — the firmware drives a known sequence
  out of port 0 and [`analyze_gpioexp.py`](analyze_gpioexp.py) decodes a logic
  analyzer capture: does every bit land on the pin it should, and how fast can
  the port be updated. Needs an LA.
- **Loopback test** (`/gpio-loopback`) — with J24 wired 1:1 to J25 one port
  drives the other, so the **read and interrupt** paths can be tested against a
  real external edge. Self-validating; needs no instrument.

The loopback test is what closed out the write self-test's open question. A
port driving itself can only ever prove the chip latched what we sent; it
cannot produce an input edge, so `~INT` and the read path were unmeasurable
until the loom existed.

## Wiring

### Logic analyzer (write self-test)

| LA | to |
|---|---|
| D0–D7 | J24 pins 1–8 |
| GND | J24 pin 10 (pin 9 is +3V3 — don't) |

**J24 interleaves the two nibbles across its rows.** It is an
`IDC-Header_2x06`, so its pin order is `IO0_0, IO0_4, IO0_1, IO0_5, IO0_2,
IO0_6, IO0_3, IO0_7`. Probing pins 1–8 in physical order therefore gives:

```
IO0_0→D0   IO0_1→D2   IO0_2→D4   IO0_3→D6
IO0_4→D1   IO0_5→D3   IO0_6→D5   IO0_7→D7
```

This is expected and the analyzer recognizes it by name. A capture that decodes
as *identity* means the probes were moved to match logical bit order instead.

### Loopback loom (read/interrupt test)

J24 pin *N* → J25 pin *N*, eight wires. **Do not connect pin 9 (+3V3) or
pin 10 (GND)** — the two headers already share both rails, and bridging them
through a loom achieves nothing.

Because J24 and J25 are both `IDC-Header_2x06` and interleave their nibbles
*identically*, wiring them in physical pin order cancels the permutation out:
`IO0_i` lands on `IO1_i`. The test discovers the mapping rather than assuming
it, and reports whether it came out as identity.

> **Never configure both ports as outputs with this loom fitted.** The
> PCA9555's outputs are push-pull, so that is eight drivers shorted against
> eight drivers. The firmware routes every direction change through one helper
> that releases both ports to inputs first; the chip tolerates it (latch-up is
> specified past 100 mA, JESD 78 Class II) but nothing about it is good.

## Running it

```sh
# --- loopback: read + interrupt paths, no instrument needed ---
curl http://<board>/gpio-loopback-run   # arm; runs from loop(), ~540 ms
curl http://<board>/gpio-loopback       # results

# --- write self-test: needs the LA ---
# arm the capture (waits for the falling edge that starts the test)
./analyze_gpioexp.py --driver fx2lafw --samplerate 4000000 \
                     --samples 5000000 --trigger &

# then trigger the run; results land on both stdout and the endpoint
curl http://<board>/gpio-selftest-run
curl http://<board>/gpio-selftest      # firmware-side numbers
curl http://<board>/loop-stats         # loop() period, read while idle
```

The loopback test checks the loom before anything else and stops harmlessly if
it is not there, so it is safe to hit on a stock board — a `wiring.ok` of
`false` comes back with the raw walking-1/walking-0 responses as evidence. It
restores both ports to inputs, the polarity register to 0, and the bus clock
when it finishes.

`--file cap.bin` re-analyzes an existing capture. The self-test takes ~570 ms,
runs from `loop()` (never from the async handler), and restores the bus clock
and port 0 to inputs when it finishes — it benches 100 kHz as well as 400 kHz,
so it changes the shared bus speed while running.

The capture is a raw sigrok `binary` dump: one byte per sample, bit *i* =
channel D*i*, so each sample byte *is* the port-0 value.

---

# Measurements

Taken 2026-07-25 on `3d53820` + the 400 kHz change, WT32-ETH01 + PCA9555PW @
0x20, platform `espressif32@6.9.0` (Arduino `Wire`), fx2lafw LA at 4 MS/s,
sigrok-cli 0.7.2. Port 0 outputs unloaded apart from the LA probes.

**The bus default is now 400 kHz** (`I2C_HZ`, which must match in both
`gpioexp.cpp` and `i2cmux.cpp` — they share one `Wire`). The 100 kHz rows below
are kept as the comparison, since the self-test benches both regardless of the
default.

Loopback figures added 2026-07-25 with the J24↔J25 loom fitted; three
consecutive runs agreed to within 1 µs on every latency term.

## Correctness — PASS

### Write path (LA)

Verified two independent ways. The readback proves the chip latched what we
sent; the LA proves the pins actually moved.

| Check | Result |
|---|---|
| Firmware readback (input register vs driven byte), 256 steps | **0 mismatches** |
| LA: distinct values on the wire | **255/255** |
| LA: steps matching driven byte under the J24 permutation | **0 mismatches** |
| Bit-to-pin mapping | `[0,2,4,6,1,3,5,7]` — J24 pin order, as expected |

No glitches, no stuck bits, no dropped writes. The ramp alternates its readback
between `gpioExpRead()` and `gpioExpReadBoth()` by step parity, so both read
paths are covered — 128 steps through each, all matching.

### Read and interrupt paths (loopback)

| Check | Result |
|---|---|
| Loom mapping, walking-1 **and** walking-0 over 8 bits | `[0..7]` — **identity**, as predicted |
| Ramp port 0 → port 1, 256 steps | **0 mismatches** |
| Ramp port 1 → port 0, 256 steps | **0 mismatches** |
| Polarity inversion on the reading port (`0xA5` → `0x5A`) | **PASS** |
| `~INT` cleared per port, not per chip | **PASS** |
| Single-transaction read returns identical data | **PASS** |
| Read mismatches / `~INT` timeouts over 100 timed edges | **0 / 0** |

The reverse ramp is not a duplicate of the forward one: it is the first time
port 1 has driven anything and the first time port 0 has been read as a true
input. Both ports are now proven in both directions.

**Walking-0 matters.** A disconnected pin floats high on the PCA9555's ~100 µA
input pull-up, so an open wire passes walking-1 and only fails walking-0.
Requiring both patterns — and checking the mapping is onto, which catches two
port-0 pins shorted to one port-1 pin — is what makes the loom check meaningful
rather than decorative.

**`~INT` is cleared per port** (datasheet §8.4.1), confirmed directly: after a
port 1 change, reading port 0 leaves `~INT` still asserted and only reading
port 1 releases it. This is worth having proven — an application that polls
only `gpioExpRead(PORT_0)` would wedge `~INT` low forever the first time port 1
moved. `gpioExpService()` reads both, so it is immune.

## Throughput

### Writes

LA-measured, with the firmware's own `micros()` agreeing to under 1%.

| Mode | µs/write | Updates/s | Payload |
|---|---|---|---|
| 8-bit @ 100 kHz *(old default)* | 344.9 | 2,900 | 23.2 kbit/s |
| 8-bit @ 400 kHz *(current default)* | 122.1 | **8,192** | 65.5 kbit/s |
| 16-bit both ports @ 400 kHz | 145.9 | 6,855 | **109.7 kbit/s** |

**Ceiling: ~8.2 kHz per port** (a 4.1 kHz square wave on every pin), or
~6.9 kHz for all 16 pins at once.

Against the theoretical floor — 29 bits of bus time for an 8-bit write at
400 kHz is 72.5 µs, so 13.8 kHz — the driver reaches **59% of what the chip
allows**. The rest is the software overhead below.

### Where the time goes

Bus time for an 8-bit write at 400 kHz is 29 bits ≈ 72.5 µs, but it measures
122.1 µs. Same gap at 100 kHz: 290 µs theoretical vs 344.9 measured. That is a
**constant ~52 µs of Arduino `Wire`/ESP32 per-transaction overhead** — at
400 kHz it is 41% of every write, and it is software, not the bus. It comes
from the ESP-IDF I2C driver building a command link per call, taking a mutex,
and blocking on a semaphore with a FreeRTOS task switch.

Because the cost is *per transaction*, transaction count matters more than
payload size. That is why both paired calls win.

**Writing both ports:** `gpioExpWriteBoth()` costs one extra data byte — 19%
more time — for twice the payload. **1.67× throughput.** Never use two
single-port writes when both ports move.

### Reads

Sustained rates, measured by the loopback test at 400 kHz.

| Path | Transactions | µs/read | Reads/s |
|---|---|---|---|
| `gpioExpReadBothFast()` — both ports | **1** | **200.1** | **5,000** |
| single port, no park (port 1) | 1 | 166.9 | 5,993 |
| `gpioExpReadBoth()` — both ports + errata park | 2 | 304.3 | 3,286 |
| `gpioExpReadBoth()` @ 100 kHz | 2 | 805.9 | 1,241 |

Two results worth carrying forward:

**Reading both ports costs 33 µs more than reading one.** Same single
transaction, one extra data byte. There is essentially never a reason to read a
single port — take both and ignore the half you don't want.

**Dropping the errata park saves 104 µs, a 1.52× speedup**, and is free. See
*Done* below. The old two-transaction figure reproduced exactly (304.3 µs, vs
304.3 µs measured last session), which is a useful cross-check that nothing
else drifted between the two sessions.

### Jitter

| Mode | median | p99 | max | stalls >1.5× median |
|---|---|---|---|---|
| 8-bit @ 100 kHz | 344.2 µs | 352.2 | 352.5 | 0 / 143 |
| 8-bit @ 400 kHz | 121.5 µs | 129.5 | 130.5 | 0 / 409 |
| 16-bit @ 400 kHz | 144.8 µs | 152.5 | 402.0 | 1 / 344 (0.29%) |

The 16-bit outlier is the Ethernet task preempting `loop()`.

## Latency

### Output — command → pin moves

| Bus speed | Avg | Range |
|---|---|---|
| 100 kHz *(old default)* | 345 µs | 344–352 |
| 400 kHz *(current default)* | **123 µs** | 121–129 |

Single-shot latency and the sustained per-write period are **identical**,
confirming `Wire` is fully blocking with no pipelining: throughput here is just
1/latency, there is no queue to hide behind. The pin settles within `tpv` =
200 ns of the data byte's ACK, ~2–3 µs before the call returns.

### Input — pin changes → change callback fires

**Now measured end to end** against a real external edge, replacing last
session's component sum. Port 0 drives the loom, port 1 sees the edge, and the
timer runs from before the stimulus write until the application's change
callback is entered. 50 edges per run, 3 runs.

| Term | Measured @ 400 kHz | Range |
|---|---|---|
| Stimulus write (`gpioExpWrite`) — not part of input latency | 121.6 µs | 121–129 |
| Pin edge → `~INT` observed low | **~1 µs** | 0–1 |
| `~INT` → new input byte in hand (`readInputsFast`) | **201.2 µs** | 190–213 |
| ISR + `gpioExpService()` dispatch + callback entry | **~0 µs** | — |
| **Pin edge → callback fires** | **~202 µs** | |

Round-trip write→callback measures **323.3 µs**, and 323.3 − 121.6 ≈ 202 µs is
the input side. (Strictly the pin moves ~2–3 µs *before* the write call
returns, so the true figure is nearer 204 µs — within the run-to-run spread.)

**Against last session's estimate of ~313 µs, the measured figure is ~202 µs.**
The estimate was not wrong, it described the old code: dropping the errata park
took 104 µs off, and `tiv` turned out to be far below its datasheet bound.

Three things fall out of this:

**`~INT` asserts before the write that caused it even returns.** The measured
0–1 µs is not a resolution artifact — the pin settles within `tpv` of the data
byte's ACK, several microseconds before `Wire` hands control back, and the
datasheet's `tiv` = 4 µs max elapses inside that window. So `~INT` is *already
low* by the time any code could look at it.

**Dispatch is free.** The end-to-end number through `gpioExpService()` and the
callback (323.3 µs) is indistinguishable from the hand-rolled poll-and-read
(323.7 µs). `loop()` idles at a median 5 µs period, so the level-check safety
net runs ~200,000 times a second and `~INT` is serviced essentially the instant
it asserts.

**Input latency is one I2C read and nothing else.** Every term except the read
is at or below 1 µs. There is no software cost left to remove here — the only
remaining lever is bus time itself.

## Done

**Bus raised to 400 kHz** (2026-07-25). Output latency 345 → 123 µs, input
latency ~815 → ~313 µs, throughput 2,900 → 8,192 writes/s. `I2C_HZ` had to
change in **both** `gpioexp.cpp` and `i2cmux.cpp`: they share one `Wire`, and
`bmp280DebugJson()` restores the clock to its own copy after a speed sweep, so
changing only one would have been silently undone.

**`gpioExpReadBoth()` added** — 1.79× over two `gpioExpRead()` calls.

**The errata park is gone from the read path** (2026-07-25), the fix proposed
last session. `gpioExpService()` and the new `gpioExpReadBothFast()` address the
input pair from the **port 1 end**: 2 bytes requested at `REG_INPUT_1` return
port 1 then port 0 and toggle the pointer back to `0x01`. Command byte `0x00` is
never written and the pointer never rests there, so the §8.4.1.1 errata — whose
*only* trigger is a pointer left at `0x00` — cannot fire, and the second
transaction that existed purely to undo that is unnecessary. **304.3 → 200.1 µs,
1.52×**, and input latency 313 → ~202 µs.

The caveat from last session stands and is worth restating: the errata race
itself cannot be tested for absence. What changed is the argument, not the
evidence — the datasheet states the trigger condition explicitly ("the last I2C
command byte written to the device was 00h") and this construction never
satisfies it. The loopback test confirms the data comes back identical
(`fast_read.ok`), which is the half that *can* be tested.

**`gpioExpRead(PORT_1)` no longer parks either** — a port 1 read already ends at
`0x01`. Only `gpioExpRead(PORT_0)` still needs the park, and it keeps it.

**Read and interrupt paths validated** via the J24↔J25 loopback, including both
ports as input and as output, the polarity register, and per-port `~INT`
clearing. Write self-test re-run afterwards as a regression check: 0/256
mismatches, 122.0 µs/write, 8,197 writes/s — unchanged.

## Still worth acting on

**Nothing blocking.** The driver is validated in both directions with both
paths measured.

If input latency ever needs to go below ~200 µs, the remaining costs are, in
order: the ~52 µs of Arduino `Wire` per-transaction overhead (fixable only by
dropping to the ESP-IDF I2C driver directly, or by pre-building a command
link), and then bus time itself. A read of a *single* port is 167 µs, so
nothing meaningful is left in payload size — 33 µs separates one port from two.

**The pull-ups are the constraint on 400 kHz, and J13 is the risk.** R17/R18
are **4.7k**. Fast-mode needs tr ≤ 300 ns and tr ≈ 0.847 × Rp × Cb, so 4.7k
allows only **~75 pF of total bus capacitance**. On-board load is ~30–45 pF —
PCA9555 contributes 8 pF (SCL) / 9.5 pF (SDA), TCA9548A and ESP32 a few each,
traces the rest — which is why 400 kHz runs clean here.

But **J13 sits on this same bus**. Anything plugged in with a cable adds
30–100 pF and blows the budget, and the failure mode is occasional NACKs from
slow rise times, not a clean stop. If J13 is ever populated at 400 kHz, drop
R17/R18 to **2.2k** (budget → ~161 pF). Minimum legal value is ~970 Ω
(`(3.3 − 0.4 V) / 3 mA`), so 2.2k or even 1.5k is safe.

## Shared-bus note

The AHT20 + BMP280 board on **mux channel 0 does not respond** — address-NACK
(code 2) at 0x38, 0x76 and 0x77. Verified at **50 kHz, 100 kHz and 400 kHz**
via `/bmp-debug`, so this is not a consequence of the speed change; the sweep
now probes the AHT20 alongside the BMP280 precisely so that question stays
answerable. Clean NACKs rather than timeouts mean the bus itself is healthy,
and the PCA9555 at 0x20 answers normally. Most likely the sensor board is
unplugged — worth a look before trusting channel 0.

## Not measured

- ~~**End-to-end input latency.**~~ **Done** — see *Input* above. The J24↔J25
  loom supplied the external edge; the round-trip write→callback was measured
  directly and the input side extracted from it.
- **Interrupt behaviour under a burst of simultaneous edges.** Every timed edge
  here moved all eight pins at once, but always with the port quiescent
  beforehand. Not tested: a second edge arriving *during* the service read,
  which is the case the `s_intPending`-cleared-before-read ordering and the
  `~INT` level re-check exist to handle. The loom can generate it — drive port 0
  from a hardware timer rather than from `loop()` — but it needs a stimulus
  source independent of the code under test.
- **Fine edge quality / rise times.** Needs a scope. The LA quantizes to 250 ns
  at 4 MS/s and says nothing about slew or overshoot. Relevant if J24 ever
  drives long cabling — the internal input pull-ups are only ~100 µA.
- **Behaviour under load.** All outputs were unloaded. Sinking near the 25 mA
  spec may shift levels enough to matter for the readback check.

## Gotcha for future captures

The analyzer drops runs shorter than 2 µs as sampling artifacts. When all eight
outputs switch at once, probe-lead skew lets the LA latch one intermediate
sample — e.g. `0xFE` for 250 ns between `0x00` and `0xFF`. The 100 kHz capture
had 4 of these, the 400 kHz one 9 (faster edges, more chances to land mid-
transition). They are not real glitches, but they *do* break naive run-length
segmentation. Lower the threshold if you are ever hunting for genuine short
glitches.

`--samples` also needs to cover the whole run: the self-test shortened from
739 ms to 571 ms when the bus went to 400 kHz, because the ramp's readbacks got
faster. 5 M samples at 4 MS/s (1.25 s) leaves comfortable margin.
