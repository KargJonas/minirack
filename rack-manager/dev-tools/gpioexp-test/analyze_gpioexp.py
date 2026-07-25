#!/usr/bin/env python3
"""
Validate PCA9555 port-0 writes with a sigrok-compatible logic analyzer.

Companion to firmware gpioExpSelfTest*(): the firmware drives a known sequence
out of port 0 (header J24) and this decodes the capture to prove that (a) every
bit lands on the pin it should and (b) how fast the port can really be updated.

Wiring: LA D0..D7 -> J24 IO0_0..IO0_7, LA GND -> board GND.

Expected capture layout (see gpioexp.cpp for the authoritative version):
    preamble  0xFF 2 ms, 0x00 2 ms
    ramp      0x00..0xFF, ~1.6 ms per step
    gap       0x00 5 ms
    burst A   0x00/0xFF alternating, 8-bit writes @ 100 kHz I2C
    gap, burst B   same @ 400 kHz
    gap, burst C   16-bit writes (both ports) @ 400 kHz
    end       port 0 back to inputs -> pull-ups park it at 0xFF

Examples
--------
Capture live, triggering on the falling edge that starts the test:
    ./analyze_gpioexp.py --driver fx2lafw --samplerate 4000000 \
                         --samples 6000000 --trigger

Re-analyze an existing capture:
    ./analyze_gpioexp.py --file cap.bin --samplerate 4000000

Note the capture is a raw sigrok "binary" dump: one byte per sample, bit i =
channel Di, so each sample byte *is* the port-0 value.
"""
import argparse
import subprocess
import sys

import numpy as np


def capture_live(driver, samplerate, samples, outfile, trigger):
    cmd = ["sigrok-cli", "-d", driver,
           "-c", f"samplerate={samplerate}",
           "--samples", str(samples),
           "-O", "binary", "-o", outfile]
    if trigger:
        cmd += ["--triggers", "D0=f"]
    print("running:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)


# J24 is an IDC-Header_2x06: the two nibbles are interleaved across its two
# rows (pin1=IO0_0, pin2=IO0_4, pin3=IO0_1, pin4=IO0_5, ...). Probing header
# pins 1..8 with D0..D7 in physical order therefore lands IO0_i on this channel.
J24_PIN_ORDER = [0, 2, 4, 6, 1, 3, 5, 7]
IDENTITY = list(range(8))


def runs_of(data, fs, min_us=2.0):
    """Split the sample stream into (value, start, length) constant runs.

    Runs shorter than min_us are dropped as sampling artifacts: when all eight
    outputs switch at once, channel-to-channel skew in the probe leads lets the
    LA latch one intermediate sample (e.g. 0xFE between 0x00 and 0xFF). Real
    writes are held for tens of microseconds at minimum.
    """
    if data.size == 0:
        sys.exit("ERROR: capture is empty.")
    edges = np.flatnonzero(np.diff(data)) + 1
    starts = np.concatenate(([0], edges))
    lengths = np.diff(np.concatenate((starts, [data.size])))
    vals = data[starts]

    glitches = int(np.count_nonzero(lengths < min_us * fs / 1e6))
    keep = lengths >= min_us * fs / 1e6
    keep[0] = keep[-1] = True
    vals, starts = vals[keep], starts[keep]
    # Merge neighbours that became adjacent-and-equal once a glitch was removed.
    keep2 = np.concatenate(([True], vals[1:] != vals[:-1]))
    vals, starts = vals[keep2], starts[keep2]
    # Recompute lengths from the surviving starts so the dropped samples are
    # charged to the preceding run and burst durations stay exact.
    lengths = np.diff(np.concatenate((starts, [data.size])))
    return vals, starts, lengths, glitches


def find_ramp(lengths, fs, lo_us=500, hi_us=4000, want=255):
    """Bursts run ~125 us per step and the gaps are 5 ms, so the ramp sits in a
    wide band between them. The lower bound has deliberate headroom: the dwell
    is write + 600 us of delays + one readback, so it tracks bus speed (~1670 us
    at 100 kHz, ~1020 us at 400 kHz) and will shrink further if the driver gets
    faster. Return the start of the longest contiguous block in the band."""
    inband = (lengths / fs * 1e6 > lo_us) & (lengths / fs * 1e6 < hi_us)
    best, best_len, i, n = None, 0, 0, len(lengths)
    while i < n:
        if not inband[i]:
            i += 1
            continue
        j = i
        while j < n and inband[j]:
            j += 1
        if j - i > best_len:
            best, best_len = i, j - i
        i = j
    if best is None or best_len < want:
        return None, best_len
    return best, best_len


def infer_perm(ramp_vals):
    """ramp_vals[k] is what the LA saw while the firmware drove k+1.
    Returns driven-bit -> LA-channel, or None if a step wasn't a clean 1-hot."""
    perm = []
    for b in range(8):
        obs = int(ramp_vals[(1 << b) - 1])
        chans = [c for c in range(8) if obs >> c & 1]
        if len(chans) != 1:
            return None
        perm.append(chans[0])
    return perm if sorted(perm) == IDENTITY else None


def analyze_ramp(vals, starts, lengths, fs):
    print("\n=== write correctness: 0x00..0xFF ramp ===")
    at, blk = find_ramp(lengths, fs)
    if at is None:
        print(f"  [FAIL] no ramp found (longest steady block: {blk} runs).")
        return None

    # The 2 ms preamble pulses and the 5 ms gap also sit in the dwell band, so
    # slide inside the block for the window that actually looks like the ramp:
    # 255 distinct non-zero values whose walking-1 steps are one-hot. (Step
    # 0x00 merges into the preceding gap, so the ramp starts at 0x01.)
    at0 = at
    for o in range(blk - 255 + 1):
        cand = vals[at + o:at + o + 255]
        if len(set(cand.tolist())) == 255 and infer_perm(cand) is not None:
            at = at + o
            break
    else:
        print(f"  [FAIL] no 255-step ramp window inside the {blk}-run block.")
        return None

    ramp = vals[at:at + 255]
    step_us = lengths[at:at + 255] / fs * 1e6
    print(f"  ramp at run #{at} (block started #{at0}), "
          f"sample {starts[at]:,}, {len(ramp)} steps")
    print(f"  step dwell min/med/max: {step_us.min():.0f} / "
          f"{np.median(step_us):.0f} / {step_us.max():.0f} us")

    distinct = len(set(ramp.tolist())) == len(ramp)
    perm = infer_perm(ramp)
    if perm is None:
        print("  [FAIL] walking-1 steps are not one-hot; wiring is not a "
              "simple permutation.")
        return at

    pred = np.array([sum(1 << perm[b] for b in range(8) if v >> b & 1)
                     for v in range(1, 256)], dtype=np.uint8)
    bad = int(np.count_nonzero(pred != ramp))

    print(f"  driven bit -> LA channel: "
          f"{', '.join(f'{b}->D{perm[b]}' for b in range(8))}")
    if perm == IDENTITY:
        note = "identity (D_i probed IO0_i directly)"
    elif perm == J24_PIN_ORDER:
        note = ("J24 physical pin order - the 2x06 IDC header interleaves the "
                "nibbles across its rows, so D0..D7 on pins 1..8 gives this")
    else:
        note = "unexpected order - check the probe hookup"

    print()
    print(f"    [{'PASS' if distinct else 'FAIL'}] all 255 driven values "
          f"distinct on the wire")
    print(f"    [{'PASS' if bad == 0 else 'FAIL'}] every step matches the "
          f"driven byte under a fixed channel permutation ({bad} mismatches)")
    print(f"    [{'PASS' if perm in (IDENTITY, J24_PIN_ORDER) else 'FAIL'}] "
          f"channel order accounted for: {note}")
    return at


def analyze_bursts(vals, starts, lengths, fs, ramp_end):
    """Find regions of fast 0x00/0xFF alternation and measure the update rate."""
    print("\n=== throughput: max port update rate ===")
    n = len(vals)
    # A burst run is 0x00 or 0xFF and short (< 1 ms). The ramp uses ~1.6 ms
    # dwells and the gaps are 5 ms, so this cleanly isolates the bursts.
    short = lengths / fs < 1e-3
    binary = (vals == 0x00) | (vals == 0xFF)
    isburst = short & binary
    if ramp_end is not None:
        isburst[:ramp_end] = False

    groups, i = [], 0
    while i < n:
        if not isburst[i]:
            i += 1
            continue
        j = i
        while j < n and isburst[j]:
            j += 1
        if j - i >= 20:          # ignore stray short runs
            groups.append((i, j))
        i = j

    if not groups:
        print("  [FAIL] no throughput bursts found.")
        return []

    labels = ["8-bit writes @ 100 kHz I2C",
              "8-bit writes @ 400 kHz I2C",
              "16-bit writes (both ports) @ 400 kHz I2C"]
    bits = [8, 8, 16]
    results = []
    for k, (i, j) in enumerate(groups):
        label = labels[k] if k < len(labels) else f"burst {k}"
        nbit = bits[k] if k < len(bits) else 8
        seg = lengths[i:j] / fs                      # seconds per held value
        dur = seg.sum()
        writes = j - i
        rate = writes / dur
        us = seg * 1e6
        # Alternation must be strict for the burst to be a valid measurement.
        v = vals[i:j]
        alternating = bool(np.all(v[:-1] != v[1:]))
        print(f"\n  --- {label} ---")
        print(f"    writes observed  : {writes:,} over {dur*1e3:.2f} ms")
        print(f"    update rate      : {rate:,.0f} writes/s")
        print(f"    per write        : {us.mean():.1f} us "
              f"(min {us.min():.1f}, med {np.median(us):.1f}, max {us.max():.1f})")
        print(f"    square wave seen : {rate/2:,.0f} Hz on each port-0 pin")
        print(f"    payload rate     : {rate*nbit/1000:,.1f} kbit/s")
        med = float(np.median(us))
        stalls = int(np.count_nonzero(us > 1.5 * med))
        print(f"    p99 / stalls     : {np.percentile(us,99):.1f} us / "
              f"{stalls} of {writes} over 1.5x median "
              f"({100*stalls/writes:.2f}%)")
        if not alternating:
            print("    !! values are not strictly alternating - writes may be "
                  "dropping or the capture is truncated.")
        results.append(dict(label=label, writes=writes, dur=dur, rate=rate,
                            us_mean=float(us.mean()), bits=nbit))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", help="existing sigrok binary capture")
    ap.add_argument("--driver", help="sigrok driver for a live capture, e.g. fx2lafw")
    ap.add_argument("--samplerate", type=float, default=4e6)
    ap.add_argument("--samples", type=int, default=6_000_000)
    ap.add_argument("--trigger", action="store_true",
                    help="wait for the falling edge that starts the self-test")
    ap.add_argument("--out", default="cap.bin", help="where to write a live capture")
    args = ap.parse_args()

    path = args.file
    if args.driver:
        path = args.out
        capture_live(args.driver, int(args.samplerate), args.samples, path,
                     args.trigger)
    if not path:
        sys.exit("provide --file or --driver")

    fs = args.samplerate
    data = np.fromfile(path, dtype=np.uint8)
    vals, starts, lengths, glitches = runs_of(data, fs)
    print(f"=== capture: {data.size:,} samples @ {fs/1e6:.3f} MS/s "
          f"({data.size/fs*1e3:.1f} ms), {len(vals):,} constant runs "
          f"({glitches} sub-2us transition artifacts dropped) ===")

    ramp_at = analyze_ramp(vals, starts, lengths, fs)
    ramp_end = (ramp_at + 255) if ramp_at is not None else None
    analyze_bursts(vals, starts, lengths, fs, ramp_end)
    print()


if __name__ == "__main__":
    main()
