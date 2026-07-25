#!/usr/bin/env python3
"""
Analyze a clock signal captured with a sigrok-compatible logic analyzer.

Designed for validating the ESP32 CLKIN that will drive the ADS131M02 ADCs
on the rack-manager board. Works on a raw sigrok "binary" capture (1 byte per
sample, bit i = channel Di) or captures live via sigrok-cli.

Examples
--------
Capture live from the Saleae/fx2lafw LA at 24 MS/s for 0.1 s and analyze:
    ./analyze_clock.py --driver fx2lafw --samplerate 24000000 --samples 2400000 \
                       --expect 8192000

Analyze an existing capture:
    ./analyze_clock.py --file cap.bin --samplerate 24000000 --expect 8192000

IMPORTANT resolution note
-------------------------
A logic analyzer quantizes edges to its sample clock. At 24 MS/s that is
41.67 ns. To measure *jitter/duty* meaningfully you want >~20 samples per
period. An 8.192 MHz clock is only ~2.9 samples/period, so this tool can
verify PRESENCE, AVERAGE FREQUENCY and STABILITY/DROPOUTS at that frequency,
but NOT fine jitter or edge quality -- that needs an oscilloscope, or run the
generator at a lower diagnostic frequency (e.g. 1.024 MHz => ~23 samples/period).
"""
import argparse
import subprocess
import sys
import tempfile
import os
import numpy as np


def capture_live(driver, samplerate, samples, outfile):
    cmd = ["sigrok-cli", "-d", driver,
           "-c", f"samplerate={samplerate}",
           "--samples", str(samples),
           "-O", "binary", "-o", outfile]
    print("running:", " ".join(cmd), file=sys.stderr)
    subprocess.run(cmd, check=True)


def load_bits(path, channel):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size == 0:
        sys.exit("ERROR: capture is empty (no samples).")
    return ((data >> channel) & 1).astype(np.int8)


def pick_channel(path):
    """Return the channel index (0-7) with the most transitions."""
    data = np.fromfile(path, dtype=np.uint8)
    best, best_edges = None, -1
    for ch in range(8):
        bits = (data >> ch) & 1
        edges = int(np.count_nonzero(np.diff(bits)))
        if edges > best_edges:
            best, best_edges = ch, edges
    return best, best_edges


def analyze(bits, fs, expect=None):
    t_res = 1.0 / fs
    n = bits.size
    dur = n * t_res
    diff = np.diff(bits.astype(np.int8))
    rising = np.where(diff == 1)[0]
    falling = np.where(diff == -1)[0]
    n_edges = rising.size + falling.size

    print(f"\n  samples captured : {n:,}  ({dur*1e3:.3f} ms @ {fs/1e6:.3f} MS/s)")
    print(f"  sample resolution: {t_res*1e9:.2f} ns")
    print(f"  total edges      : {n_edges:,}  (rising {rising.size:,}, falling {falling.size:,})")

    if rising.size < 3:
        print("\n  !! No periodic signal detected on this channel.")
        return

    # --- average frequency: the most trustworthy metric on an LA ---
    span = rising[-1] - rising[0]
    n_periods = rising.size - 1
    f_avg = n_periods / (span * t_res)
    samples_per_period = fs / f_avg

    # --- per-period stats (jitter) ---
    periods = np.diff(rising) * t_res            # seconds
    high = None
    if falling.size:
        # match each rising edge to the next falling edge for duty
        idx = np.searchsorted(falling, rising)
        valid = idx < falling.size
        high = (falling[idx[valid]] - rising[valid]) * t_res
    p_med = np.median(periods)
    duty = np.mean(high) / p_med * 100 if high is not None and high.size else float("nan")

    # dropouts (missing pulses) and glitches (runt pulses)
    dropouts = int(np.count_nonzero(periods > 1.5 * p_med))
    glitches = int(np.count_nonzero(periods < 0.5 * p_med))

    print(f"\n  AVERAGE frequency: {f_avg/1e6:.5f} MHz   ({samples_per_period:.2f} samples/period)")
    if expect:
        err = (f_avg - expect) / expect * 1e6
        print(f"  target           : {expect/1e6:.5f} MHz   error {err:+.1f} ppm ({(f_avg-expect):+.1f} Hz)")
    print(f"  duty cycle (approx): {duty:.1f} %")
    print(f"  period min/med/max : {periods.min()*1e9:.1f} / {p_med*1e9:.1f} / {periods.max()*1e9:.1f} ns")
    # histogram of period lengths in whole samples (reveals clean vs dithered)
    psamp = np.round(periods * fs).astype(int)
    vals, cnts = np.unique(psamp, return_counts=True)
    hist = "  ".join(f"{v}smp:{c/psamp.size*100:.2f}%" for v, c in zip(vals, cnts))
    print(f"  period histogram   : {hist}")
    print(f"  period std (jitter): {periods.std()*1e9:.1f} ns  (see caveat below)")
    print(f"  dropouts (>1.5x)   : {dropouts}")
    print(f"  runt pulses (<0.5x): {glitches}")

    # --- caveats & verdict ---
    print("\n  === assessment ===")
    under = samples_per_period < 20
    if under:
        print(f"  !! Only {samples_per_period:.1f} samples/period. Period-jitter and duty")
        print(f"    numbers above are dominated by the {t_res*1e9:.0f} ns sample quantization,")
        print("    NOT the real signal. Trust AVERAGE frequency + dropout counts only.")
        print("    For real jitter, use a scope or run the generator lower")
        print(f"    (e.g. 1.024 MHz -> {fs/1.024e6:.0f} samples/period).")
    verdict = []
    if expect:
        ok_freq = abs(f_avg - expect) / expect < 0.005  # 0.5%
        verdict.append(("frequency within 0.5% of target", ok_freq))
    verdict.append(("no dropouts (missing pulses)", dropouts == 0))
    verdict.append(("no runt/glitch pulses", glitches == 0))
    if not under:
        verdict.append(("duty 45–55%", 45 <= duty <= 55))
        # A clean clock whose period isn't an integer # of samples MUST show a
        # ±1-sample spread (pure quantization). Only flag jitter if the spread
        # exceeds that unavoidable floor.
        spread_samples = (periods.max() - periods.min()) * fs
        verdict.append(("period spread at/below LA quantization floor",
                        spread_samples <= 1.5))
    print()
    allok = True
    for label, ok in verdict:
        print(f"    [{'PASS' if ok else 'FAIL'}] {label}")
        allok = allok and ok
    print()
    print("  =>", "Clock looks good for what this LA can measure."
          if allok else "Clock has issues - see failed checks above.")
    if under:
        print("    (Reminder: fine jitter/edge quality still unverified without a scope.)")


def main():
    ap = argparse.ArgumentParser(description="Analyze a captured clock signal.")
    ap.add_argument("--file", help="existing sigrok binary capture")
    ap.add_argument("--driver", help="sigrok driver to capture live, e.g. fx2lafw or demo")
    ap.add_argument("--samplerate", type=float, default=24e6, help="sample rate in Hz")
    ap.add_argument("--samples", type=int, default=2_400_000, help="samples to capture live")
    ap.add_argument("--channel", type=int, default=None, help="LA channel 0-7 (default: auto)")
    ap.add_argument("--expect", type=float, default=None, help="expected clock freq in Hz")
    args = ap.parse_args()

    fs = args.samplerate
    tmp = None
    path = args.file
    if args.driver:
        tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        tmp.close()
        path = tmp.name
        capture_live(args.driver, int(fs), args.samples, path)
    if not path:
        sys.exit("provide --file or --driver")

    ch = args.channel
    if ch is None:
        ch, edges = pick_channel(path)
        print(f" auto-selected channel D{ch} ({edges:,} edges)", file=sys.stderr)
    bits = load_bits(path, ch)
    print(f"=== clock analysis: channel D{ch} ===")
    analyze(bits, fs, args.expect)

    if tmp:
        os.unlink(tmp.name)


if __name__ == "__main__":
    main()
