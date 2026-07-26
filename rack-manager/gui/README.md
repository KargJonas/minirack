# Rack manager GUI

Live view of the rack-monitor's ADC channels. Terminates the board's raw TCP
stream, block-averages 9615 SPS down to 10 Hz, and serves a page that plots it
and lets you configure the analog front end.

This is a **plausibility check**, not the collector: no sample data is stored,
and the only DSP is the windowed average. The front-end config *is* persisted.

## Running

Python 3.10+ and numpy. Nothing else: the browser link is Server-Sent Events,
so there is no web framework and no WebSocket library, and the UI library is
vendored, so there is no npm and no build step.

```
python3 server.py                 # stream on :9000, GUI on http://localhost:8080/
```

Point the board at it:

```
curl "http://<board>/stream/collector?host=<server-ip>&port=9000"
curl -X POST http://<board>/easy-ota/reboot
```

Without hardware, `../dev-tools/fakeboard.py` sends the same handshake and wire
format with synthetic 50 Hz signals:

```
python3 ../dev-tools/fakeboard.py 127.0.0.1 9000
```

## Layout

| | |
|---|---|
| `server.py` | stream terminator, DSP, HTTP + SSE, config API |
| `channels.py` | front-end **types** and the **defaults** for each channel |
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

Each card has a gear button. It swaps the chart for a form, and swaps itself for
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

## Notes

- A reconnecting board **takes over** from the previous connection rather than
  being rejected. The board only redials after its own send failed, so it is the
  authority on whether the old socket is dead; refusing it would lock the stream
  out for a backoff cycle.
- A slow browser is dropped from the fan-out rather than blocked on, the same
  rule the board applies to this server.
- Invalid config is rejected per field with a 422 rather than silently coerced -
  a mistyped shunt value should not quietly become a plausible-looking graph.
- The page makes no external requests.
