# Rack manager GUI

Live view of the rack-monitor's ADC channels. Finds the board on the link,
drains its raw TCP stream, block-averages 9615 SPS down to 10 Hz, runs a Welch
spectrum over the undecimated samples, and serves a page that plots the value,
its spectrum, its recent history and a spectrogram - and lets you configure the
analog front end.

This is a **plausibility check**, not the collector: no *samples* are stored,
and the DSP is a windowed average and an FFT. What is kept is a minute of
**derived** output - block statistics and spectrogram columns, in RAM, ~150 KB
for all channels - so that a page which has just been reloaded still has a past
to show. The front-end config *is* persisted, to disk.

## Running

Python 3.10+, numpy, and `avahi-utils` for discovery. Nothing else: the browser
link is Server-Sent Events, so there is no web framework and no WebSocket
library, and the UI library is vendored, so there is no npm and no build step.

```
python3 server.py                 # find the board, GUI on http://localhost:8080/
python3 server.py --history-seconds 300   # a longer spectrogram
```

`--history-seconds` sets how far back the spectrogram and the series reach; it
defaults to the 60 s the time series has always shown, so the two plots that
share an x axis start out covering the same span. It costs ~1.5 KB/s of RAM for
all five channels, so an hour is about 5 MB.

There is nothing to configure at either end. If discovery is in the way -
avahi missing, or a board on another link - pin it:

```
python3 server.py --board 10.42.0.110
```

Without hardware, `../dev-tools/fakeboard.py` listens like the board does and
sends the same handshake and wire format with synthetic 50 Hz signals. It does
not advertise itself, so pin it:

```
python3 ../dev-tools/fakeboard.py &
python3 server.py --board 127.0.0.1
```

## Layout

| | |
|---|---|
| `server.py` | stream terminator, DSP, HTTP + SSE, config API |
| `channels.py` | front-end **types** and the **defaults** for each channel |
| `spectrum.py` | the Welch spectrum, its display binning, and THD |
| `config.json` | live config, written by the GUI (created on first save) |
| `static/index.html` | shell |
| `static/style.css` | all styling; light and dark are both selected, not flipped |
| `static/app.js` | the Preact app |
| `static/assets/preact-htm.js` | preact + hooks + htm, 13 KB, vendored |
| `static/assets/*.svg` | source icons (inlined into `app.js`, see below) |

### Frontend

Preact + [htm](https://github.com/developit/htm) via the `htm/preact/standalone`
build, vendored into `static/assets/`. Real components, hooks and state, but no
npm, no bundler, no CDN and no network at runtime - `python3 server.py` still
serves the whole thing. htm gives JSX-like syntax through tagged template
literals, so `` html`<${Card} x=${y} />` `` instead of `<Card x={y} />`.

The icons are **inlined** in `app.js` rather than loaded as `<img>`. An `<img>`
cannot inherit `currentColor`, and `config.svg` hardcodes a stroke colour that
would be wrong in both themes. The path data is copied verbatim from the SVGs,
which stay as the source of truth.

## Configuring the analog front end

Each card has a gear button. It swaps the plots for a form, and swaps itself for
a checkmark (apply) and an x (discard); either asks for confirmation in the
centre of the card. Config lives in the card so that a channel whose data looks
wrong can be inspected where you noticed it.

Front-end types, each with its own fields:

| Type | Ratio |
|---|---|
| Direct | 1 - volts at the ADC pin |
| Fixed ratio | one number, for a front end whose components are not broken out |
| Resistive divider | (R_top + R_bottom) / R_bottom |
| Current shunt | 1 / R_shunt |
| CT + burden | turns / R_burden |
| PT + divider | PT ratio × (R_top + R_bottom) / R_bottom |

Adding a type to `FRONTEND_TYPES` in `channels.py` adds it to the UI with no
frontend change - the form is generated from the field schema.

Every channel also carries four corrections, which is most of what is easy to
forget:

- **Zero offset** (volts at pin), subtracted before scaling. Global chop cancels
  the ADC's own offset but *not* the front end's, so this is the one the
  hardware cannot fix for you.
- **Gain correction** (×). The second point of the two-point calibration
  design.md asks for on the shunt channel.
- **Source impedance** (Ω). The ADC loads the divider, so what reaches the pin
  is low by `Zin/(Zin+Rth)`. Zin is derived from the board's reported CLKIN
  (`330k × 4.096M / fMOD` ≈ 676 kΩ at 4 MHz), so only Rth is configured. On the
  12 V divider this is a 0.133 % correction, matching design.md's "~0.15 %".
  Valid at PGA gain 1-4 only; leave it at 0 on the gain-32 shunt channel.
- **Invert polarity**, for a CT or shunt wired backwards.

Changes take effect on the **next block**, not the next reconnect - scales are
read per block.

### Why the channel map lives here and not in the firmware

The board's handshake reports how the ADCs are *configured* - PGA gain, OSR,
chop, chop delay, CLKIN, zero offsets - and nothing about what is connected to
them. No names, no units, no scale factors, and not even the sample rate, which
is derived from the clock terms.

Which signal reaches which pin, and what divider or shunt sits in front of it,
are properties of the wiring. Keeping them here means recalibrating is an edit
in the GUI rather than a reflash, and the firmware never holds a second copy
that can drift. `adcVolts()` in the driver draws the same line: it returns volts
at the pin and calls front-end scaling "the caller's business".

**Two channels ship uncalibrated**, because design.md does not specify them:
`Wall current` has no CT or burden value ("size the burden to ~1.0 V at max"),
so it defaults to Direct and shows volts at the pin; `Wall voltage` uses a fixed
325 V/V from "~1.0 V at 325 V peak". Both are marked on the card. Set them
properly once the parts are known.

## The DSP

One point per `round(sps / 10)` frames - 962 at 9615.4 SPS, so 10.00 Hz.

Each point carries **mean, rms, min and max**. The mean is the requested
windowed average and is what the line plots. rms is there because the average of
a symmetric AC waveform is ~0 by construction: on a healthy mains input the two
wall channels would sit flat at zero, which is the opposite of a plausibility
check. The synthetic feed shows it plainly - wall voltage reads mean 0.07 V
against rms 225.16 V.

min/max become the shaded band behind the line, so a channel that is swinging
looks different from one that is flat even where the mean does not move.

### The frequency domain

Under each time series is a spectrum of the same channel, on a log frequency
axis in dB. It is fed the **undecimated** samples - averaging to 10 Hz first
would leave a 5 Hz Nyquist and nothing to look at - so it sees the whole band
the ADC delivers, DC to 4.8 kHz.

An 8192-point Welch estimate: four half-overlapped Hann-windowed segments,
1.17 Hz bins over 2.13 s of history, pushed at 2 Hz. `numpy.fft.rfft` batched
over channels does all five in **1.7 ms**, so the whole update costs 2.5 ms and
runs inline - well under 1 % of a core. This is why there is no scipy and no
pyFFTW here: the transform was never the expensive part.

The expensive part is the **wire**. 4097 bins x 5 channels is ~140 KB of JSON
per update, so what leaves the server is binned to 149 display points, 11 KB,
22 KB/s over SSE.

Three things in `spectrum.py` are not obvious and are worth stating:

- **The axis is not purely logarithmic.** Below ~40 Hz a log bin is narrower
  than the FFT's own 1.17 Hz resolution and would cover no bins at all, putting
  a gap exactly where the mains fundamental lives. So each display bin is one
  FFT bin or the log width, whichever is larger: full resolution low down, log
  above. Every bin is non-empty by construction, which is also what makes
  `np.add.reduceat` safe - given an empty range it returns the element at the
  start index rather than zero.
- **Power is summed within a bin, never maxed.** A tone almost never lands on a
  bin centre, and its peak bin alone reads up to 1.42 dB low with a Hann window.
  Summing the power it actually occupies recovers it exactly - measured flat to
  0.002 dB with a tone swept across two bins. That is what lets the 50 Hz
  fundamental read its true rms wherever it falls.
- **That normalisation needs the ENBW, not just the coherent gain.** Coherent
  gain is correct for a single on-bin peak; summing a lobe also collects the
  window's spread, which for Hann is 1.5 bins. Leave it out and every amplitude
  reads sqrt(1.5) = 1.22x high - which is precisely the bug the synthetic feed
  caught, reading 281 V on a 230 V input.

Amplitudes are rms in the channel's engineering units, so a bin reads "the rms
in this band" - true for a tone and true for noise. Note that the log part of
the axis has wider bins at the top end, so a flat noise floor tilts upward.
That is real: a wider band holds more noise.

Summing power also settles the window. Scalloping is the usual reason to reach
for a flat-top, and it is already gone, so the choice comes back to resolution
and leakage. Hann has the narrowest main lobe, and its skirts measure 195 dB
below the fundamental at harmonic distances, far under a 24-bit ADC's own floor.

### The spectrogram

Each card is a full-width row split into **now** on the left - the value and its
current spectrum - and **the past** on the right, where the time series sits
directly above a spectrogram of the same channel.

Those two are **one canvas**, not two stacked ones. They share an x axis so that
a mark at some instant in the line is directly above the same instant in the
spectrogram, and drawing them together makes that structural rather than an
agreement between two independently scaled plots. It also forced a fix the
series had got away with alone: x used to be the sample *index*, which only
equals time while nothing is missing. Under a dropout an index axis quietly
closes the gap, and the spectrogram below would have slid out of step with the
line above at exactly the moment you most wanted to read them together.

Both plots are replayed from server-side rings when a page connects, for the
same reason: a full spectrogram above a line that began at page load is not two
views of one minute. The spectrogram's ring is one byte per bin - `dB` is
quantised at 1 dB/LSB over -200..+55, which a byte spans exactly - so a minute
is ~90 KB, sent once. Live columns are not sent again: a spectrum message *is* a
spectrogram column, so the page appends the one it already received and the
spectrogram costs no extra bandwidth while it is open. The ring deliberately
survives a board reconnect - a dropout is precisely the event you want the
record of afterwards - and is discarded only if the axis changes, since columns
either side of that are not the same measurement.

Two things that had to be got right, both visible the moment they were wrong:

- **The colour floor is a percentile, not "peak minus a fixed range".** The
  noise floor is where most bins actually live, so anchoring the light end to it
  spends the whole ramp on the part that varies. Anchored to the peak instead,
  the floor sat mid-ramp and every channel washed out to a uniform mid-blue with
  the tones barely darker than the background.
- **A column covers the ground up to the next column, not a nominal `dt`.** The
  2 Hz cadence jitters by a few ms; testing coverage against `dt` exactly left
  whichever intervals ran long uncovered, scattering 1-pixel blanks across the
  plot that read as dropouts. It is capped at 1.5 dt so a *genuinely* missing
  column still shows as a gap instead of being smeared over by its neighbour -
  one dropped column is a 5 px gap, three are 22 px.

The ramp is sequential: one hue, blue, light to dark - not a rainbow, which
would invent boundaries the data does not have. Like the rest of `style.css` the
dark ramp is selected rather than flipped: `--spec-0` is always the quiet end,
and in each mode it is the step that recedes toward that mode's own surface.

Behind the live trace is a **peak hold** decaying at 2 dB/s, so a spike that
happened while you were reading a different card is still there when you get to
this one. It is reset when a front-end scale changes, because the held trace is
in the old units and a 325x rescale would otherwise take most of a minute to
fall out of the plot.

Under the plot are the **dominant tone and THD**. Not only a mains thing: on a
DC rail the dominant tone is the switching ripple, and its harmonics are the
same question asked of a converter instead of a grid. The frequency is
interpolated by a power-weighted centroid over the lobe, because at 1.17 Hz
bins the index alone cannot tell 50.0 Hz from 50.5 Hz. Both read `-` when
nothing stands 20 dB out of the noise, which is the honest answer for a quiet
DC rail rather than a number invented from the noise floor.

Against the synthetic feed, whose answers are known: 229.803 V rms read on
229.810 V in, 5.00 % THD read on 5 % in, and the wall voltage's 225.17 V
agreeing with the time-domain rms above to two decimal places.

## Discovery, and why the board listens

The board is the TCP **server** and this is the client, which is the opposite of
what a data flow board -> collector suggests. TCP does not care: throughput,
Nagle, keepalive and window are all the same whichever end called `connect()`.
What it decides is **which end has to be told where the other one is** - and
that is a thing worth having none of.

So the end that has to be found is the one already advertising itself. The
board publishes `_easyota._tcp` for EasyOTA regardless, and `flash.sh` already
browses it to find a board to flash; this browses the same record with the same
`avahi-browse -rtp`, and there is exactly one discovery mechanism in the
project. Neither side stores an address, so neither a DHCP lease nor a rename
can break the link, and the port is a constant both sides agree on
(`ADC_STREAM_PORT`) rather than a second thing to keep in sync.

Rediscovery happens on **every** connect cycle, not once at startup, which is
what lets a board that has just taken a new lease come back on its own.

What this gives up: any host on the link can connect to the board's stream port
and take the feed, where before the board only ever talked to an address it had
been given. On a trusted rack link that is a fair trade; it is also why the
board applies **newest connection wins** (see below).

## Notes

- A newly connecting collector **takes over** from the previous one at the
  board, checked between packets so a half-written packet is never handed to a
  new collector. The collector only redials after its own read failed, so it is
  the authority on whether the old socket is dead - the same argument that used
  to run in the other direction, before the roles swapped. Keepalive would
  notice the corpse within 11 s anyway, but a collector that has already
  restarted should not have to wait for that.
- A slow browser is dropped from the fan-out rather than blocked on, the same
  rule the board applies to this server.
- Invalid config is rejected per field with a 422 rather than silently coerced -
  a mistyped shunt value should not quietly become a plausible-looking graph.
- The page makes no external requests.
