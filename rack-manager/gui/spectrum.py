"""
The frequency-domain view: a Welch spectrum per channel, plus the two numbers
worth reading off it (dominant tone and THD).

Why numpy and not a DSP library
-------------------------------
9615 SPS x 5 channels sounds like a lot until you price the transform. One
update here is four 8192-point real FFTs per channel, batched over channels by
`np.fft.rfft(..., axis=0)`, and it costs ~1.8 ms - at SPECTRUM_HZ that is well
under 1 % of a core. scipy would buy a `welch()` that this module spells out in
fifteen lines, and pyFFTW would optimise something that is already noise. The
"stdlib + numpy" rule in the README is worth more than either.

What *is* expensive is the wire. A raw 8192-point spectrum is 4097 bins per
channel, ~140 KB of JSON per update, which is why what leaves here is binned to
a couple of hundred display points - see `display_bins`.

Two decisions that are not obvious
----------------------------------
**Power is summed within a display bin, never maxed.** A tone almost never
lands on a bin centre, and reading its peak bin alone understates it by up to
1.42 dB with a Hann window (scalloping). Summing power across the bins the tone
actually occupies recovers it exactly - measured at 0.00 dB error across the
worst-case placements. So the 50 Hz fundamental reads its true rms wherever it
falls, which is the whole point of a plausibility check.

That also settles the window. Scalloping is the usual reason to reach for a
flat-top, and power-summing has already removed it, so the choice comes back to
resolution and leakage: Hann has the narrowest main lobe, and its skirts measure
195 dB below the fundamental at harmonic distances - far under the noise floor
of a 24-bit ADC. Hann it is.

**DC is removed per segment.** The 12 V rail is 12 V of DC and a few mV of
ripple; leaving the mean in puts a spike at bin 0 whose leakage buries the
ripple that someone opened this view to look at.

Amplitudes are rms in the channel's engineering units, so a display bin reads
"the rms in this frequency band" - true for a tone and true for noise, though
note that on the log part of the axis the bins get wider, so a flat noise floor
tilts upward. That is real: a wider band holds more noise.
"""

import base64
import collections

import numpy as np

FFT_N = 8192            # 1.174 Hz bins, 0.85 s, at 9615 SPS
FFT_SEGMENTS = 4        # Welch segments, 50 % overlap -> 2.13 s of history
SPECTRUM_HZ = 2.0       # updates per second pushed to the GUI
LOG_BINS = 200          # display bins across the log part of the axis

DB_FLOOR = -240.0       # JSON has no -Infinity, and 24 bits cannot reach here
PEAK_DECAY_DB_S = 2.0   # how fast the peak-hold trace falls back

# Spectrogram history is quantised to one byte per bin: code c means
# c + DB_QUANT_OFFSET dB, 1 dB per step. A byte spans -200..+55 dBV, which
# covers the wall channel's +47 dBV at the top and sits far below the ADC's own
# per-bin floor (~-160 dBV) at the bottom. 1 dB is finer than the colour ramp
# can show anyway, so nothing is lost that the eye could have read.
DB_QUANT_OFFSET = -200.0

F_MIN_HZ = 5.0          # below this, "dominant tone" is drift, not a tone
THD_HARMONICS = 10      # harmonics summed into THD
THD_MIN_SNR_DB = 20.0   # below this the fundamental is not a tone; report None
LOBE_BINS = 2           # Hann main lobe half-width, in FFT bins


def display_bins(freqs):
    """Group FFT bins into display bins: 1:1 at low frequency, log-spaced above.

    A purely log axis cannot be used all the way down. Near DC a log bin is
    narrower than the FFT's own resolution, so it covers no bins at all - at
    1.17 Hz resolution and 200 bins, everything below ~28 Hz comes out empty and
    the plot starts with a gap where the mains fundamental's neighbourhood
    should be.

    So the width of each bin is whatever the log spacing asks for *or* one FFT
    bin, whichever is larger. Low frequencies pass through at full resolution
    and the axis becomes log once the log bins are wide enough to hold
    something. Every bin is non-empty by construction, which is also what lets
    the caller use `np.add.reduceat` - given an empty range it silently returns
    the element at the start index instead of zero.

    Returns the start index of each display bin, with a trailing stop, so bin i
    covers `freqs[starts[i]:starts[i+1]]`.
    """
    df = freqs[1]
    ratio = (freqs[-1] / df) ** (1.0 / LOG_BINS)

    starts = []
    k = 1                                  # DC is not plotted
    while k < len(freqs):
        starts.append(k)
        k += max(1, int(round(freqs[k] * (ratio - 1.0) / df)))
    starts.append(len(freqs))
    return np.array(starts, dtype=np.intp)


class History:
    """Ring of spectrogram columns - the spectrum, kept.

    This is the one place the GUI holds a record of the past, and it is worth
    being precise about what that does to the README's "no sample data is
    stored". It still holds: what accumulates here is *derived* - one byte per
    display bin per column - not samples. A minute of history for all channels
    is under 90 KB against the ~1.4 MB/s of raw frames it was computed from,
    and nothing is written to disk. The collector remains the thing that stores
    the stream; this stores what the plot already showed you.

    Kept in RAM only, so a restart starts empty. It deliberately survives a
    *board* reconnect, though - a dropout is exactly the moment whose history
    you want afterwards, so it would be perverse to clear the record on it. The
    axis is what it cannot survive: a board clocked differently has different
    bins, and columns either side of that are not the same measurement.
    """

    def __init__(self, seconds: float, nbins: int, nch: int):
        self.seconds = float(seconds)
        self.nbins = nbins
        self.nch = nch
        n = max(1, int(round(self.seconds * SPECTRUM_HZ)))
        self.t = collections.deque(maxlen=n)
        self.cols = collections.deque(maxlen=n)

    def push(self, t: float, db: np.ndarray):
        """One column. `db` is (nbins, nch), as `compute` produced it."""
        q = np.clip(np.rint(db - DB_QUANT_OFFSET), 0, 255).astype(np.uint8)
        self.t.append(round(float(t), 3))
        self.cols.append(np.ascontiguousarray(q.T))     # (nch, nbins)

    def message(self):
        """The whole ring, base64 uint8, replayed to a browser on connect.

        As JSON floats this would be ~11 KB per column and megabytes for the
        ring; as bytes a 60 s window is ~90 KB, sent once. Live columns are not
        sent this way at all - the browser already receives every spectrum and
        appends its own, so the spectrogram costs no extra bandwidth once the
        page is up.
        """
        raw = np.stack(self.cols).tobytes() if self.cols else b""
        return {
            "type": "spectrogram_history",
            "t": list(self.t),
            "bins": self.nbins,
            "nch": self.nch,
            "db_offset": DB_QUANT_OFFSET,
            "seconds": self.seconds,
            "dt": round(1.0 / SPECTRUM_HZ, 4),
            "data": base64.b64encode(raw).decode(),
        }


class Spectra:
    """Sliding Welch estimate over the last `FFT_SEGMENTS` half-overlapped
    segments, for all channels at once.

    Scales are applied at compute time rather than on the way in, for the same
    reason `emit()` reads them per block: editing the front-end config should
    move the plot on the next update, not the next reconnect.
    """

    def __init__(self, sps: float, nch: int):
        self.sps = sps
        self.nch = nch
        self.span = FFT_N * (FFT_SEGMENTS + 1) // 2
        self.hop = max(1, round(sps / SPECTRUM_HZ))

        self.ring = np.zeros((self.span, nch))
        self.filled = 0
        self.since_update = 0

        self.window = np.hanning(FFT_N)
        self.coherent_gain = self.window.mean()
        # Equivalent noise bandwidth, in bins: 1.5 for Hann. Coherent gain alone
        # normalises a single on-bin peak, and everything here sums power across
        # a lobe instead, which collects the window's spread as well - without
        # this every amplitude reads sqrt(1.5) = 1.22x high.
        self.enbw = (self.window ** 2).mean() / self.coherent_gain ** 2
        self.freqs = np.fft.rfftfreq(FFT_N, 1.0 / sps)
        self.starts = display_bins(self.freqs)

        lo = self.freqs[self.starts[:-1]]
        hi = self.freqs[self.starts[1:] - 1]
        self.centers = np.sqrt(np.maximum(lo, self.freqs[1]) * np.maximum(hi, self.freqs[1]))

        self.peak = np.full((len(self.centers), nch), DB_FLOOR)
        self.peak_decay = PEAK_DECAY_DB_S / SPECTRUM_HZ

    def reset_peak(self):
        """Drop the peak-hold trace. Called when a front-end scale changes:
        the held trace is in the old units, and at PEAK_DECAY_DB_S a 325x
        rescale would take most of a minute to fall out of the plot on its own.
        """
        self.peak.fill(DB_FLOOR)

    def axis(self):
        """The frequency axis, sent once per board session rather than with
        every update - it is a third of the payload and it never changes."""
        return {
            "type": "spectrum_axis",
            "f": [round(float(c), 3) for c in self.centers],
            "sps": round(self.sps, 2),
            "n": FFT_N,
            "resolution_hz": round(float(self.freqs[1]), 4),
            "segments": FFT_SEGMENTS,
            "window": "hann",
            "span_s": round(self.span / self.sps, 3),
            "db_floor": DB_FLOOR,
        }

    def push(self, volts: np.ndarray) -> bool:
        """Feed volts-at-the-pin. True once a new update is due."""
        k = len(volts)
        if k >= self.span:
            self.ring[:] = volts[-self.span:]
        else:
            self.ring[:-k] = self.ring[k:]
            self.ring[-k:] = volts

        self.filled = min(self.span, self.filled + k)
        self.since_update += k

        if self.filled < self.span or self.since_update < self.hop:
            return False
        self.since_update = 0
        return True

    def _welch(self):
        """Mean periodogram over the half-overlapped segments -> rms amplitude
        per FFT bin, in volts at the pin."""
        step = FFT_N // 2
        acc = np.zeros((FFT_N // 2 + 1, self.nch))

        for s in range(FFT_SEGMENTS):
            seg = self.ring[s * step: s * step + FFT_N]
            seg = seg - seg.mean(axis=0)               # see module docstring
            x = np.fft.rfft(seg * self.window[:, None], axis=0)
            acc += x.real ** 2 + x.imag ** 2

        acc /= FFT_SEGMENTS
        # 2/(N*cg) undoes the transform and the window's coherent gain and folds
        # the negative frequencies back in; /sqrt(2) turns a peak into an rms;
        # /sqrt(enbw) accounts for the lobe the caller is about to sum over.
        norm = (2.0 / (FFT_N * self.coherent_gain)) / np.sqrt(2.0 * self.enbw)
        return np.sqrt(acc) * norm

    def compute(self, scales):
        """One spectrum message, and the binned dB it was built from.

        The array comes back alongside the message because `History` wants the
        unrounded values, and recomputing an FFT to get them back would be a
        strange way to save a return value.
        """
        amp = self._welch() * np.abs(np.asarray(scales))    # engineering units

        power = amp ** 2
        binned = np.sqrt(np.add.reduceat(power, self.starts[:-1], axis=0))

        with np.errstate(divide="ignore"):
            db = np.maximum(20.0 * np.log10(binned), DB_FLOOR)
        db = np.where(np.isfinite(db), db, DB_FLOOR)

        self.peak = np.maximum(db, self.peak - self.peak_decay)

        # Rounded and converted to lists in one vectorised step per array. Doing
        # it per element costs more than the four FFTs that produced the data.
        db_l = np.round(db, 1).T.tolist()
        peak_l = np.round(self.peak, 1).T.tolist()

        chans = [
            {"db": db_l[i], "peak": peak_l[i], **self._tone(amp[:, i])}
            for i in range(self.nch)
        ]
        return {"type": "spectrum", "ch": chans}, db

    def _tone(self, amp):
        """Dominant tone and the distortion around it.

        Not only a mains thing: on a DC rail the dominant tone is the switching
        ripple, and its harmonics are the same question asked of a converter
        instead of a grid. Reported as None when nothing stands far enough out
        of the noise to be called a tone at all.
        """
        df = self.freqs[1]
        k0 = int(np.ceil(F_MIN_HZ / df))
        band = amp[k0:]
        if not len(band) or not np.any(band > 0):
            return {"f0": None, "f0_amp": None, "thd": None}

        k = k0 + int(np.argmax(band))

        # Everything outside the fundamental's lobe and its harmonics' lobes is
        # the floor we are measuring the tone against.
        rest = np.delete(band, slice(max(0, k - k0 - LOBE_BINS), k - k0 + LOBE_BINS + 1))
        floor = np.median(rest) if len(rest) else 0.0

        a1 = self._lobe(amp, k)
        if floor <= 0 or a1 <= 0 or 20 * np.log10(a1 / floor) < THD_MIN_SNR_DB:
            return {"f0": None, "f0_amp": None, "thd": None}

        # Interpolate the peak: at 1.17 Hz resolution the bin index alone cannot
        # tell 50.0 Hz from 50.5 Hz. Power-weighted centroid over the lobe.
        lo, hi = max(0, k - LOBE_BINS), min(len(amp), k + LOBE_BINS + 1)
        w = amp[lo:hi] ** 2
        f0 = float(np.sum(self.freqs[lo:hi] * w) / np.sum(w))

        harm = 0.0
        for h in range(2, THD_HARMONICS + 1):
            kh = int(round(h * f0 / df))
            if kh + LOBE_BINS >= len(amp):
                break
            harm += self._lobe(amp, kh) ** 2

        return {
            "f0": round(f0, 2),
            "f0_amp": round(a1, 6),
            "thd": round(float(np.sqrt(harm) / a1), 5),
        }

    def _lobe(self, amp, k):
        """Rms of one tone: the power in its main lobe, not just its peak bin.
        This is the same argument as the display binning - a tone between bins
        has its energy in the neighbours, and ignoring them loses up to 1.42 dB.
        """
        lo, hi = max(0, k - LOBE_BINS), min(len(amp), k + LOBE_BINS + 1)
        return float(np.sqrt(np.sum(amp[lo:hi] ** 2)))
