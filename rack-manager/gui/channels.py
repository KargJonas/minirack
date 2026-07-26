"""
The analog front end: what is wired to each ADC input, and how to turn volts at
the ADC pin into engineering units.

This is the reason the board's handshake carries no channel names, units or
scale factors. The board reports only how the chips are *configured* - PGA gain,
OSR, chop, chop delay, CLKIN, zero offsets - and the server does the rest.
Which signal reaches which pin, and what divider or shunt sits in front of it,
are properties of the wiring, so they live here and can be corrected without a
reflash. `adcVolts()` in the driver draws the same line: it returns volts at the
pin and calls front-end scaling "the caller's business".

Values below are the defaults, from system-design/design.md. Live values are
whatever `config.json` holds - edit them in the GUI, not here.
"""

# --------------------------------------------------------------------------
# Front-end types
#
# Each supplies `scale`: multiply volts-at-the-pin by it to get `unit`.
# `fields` drives the GUI's form, so adding a type here adds it to the UI with
# no frontend change.
# --------------------------------------------------------------------------

FRONTEND_TYPES = {
    "direct": {
        "label": "Direct (no front end)",
        "unit": "V",
        "help": "The ADC pin voltage as-is.",
        "fields": [],
    },
    "fixed": {
        "label": "Fixed ratio",
        "unit": "",
        "help": "One overall ratio, for a front end whose component values are "
                "not broken out. Set the unit yourself.",
        "fields": [
            {"key": "scale", "label": "Units per volt at pin", "unit": "", "default": 1.0},
            {"key": "unit", "label": "Unit", "type": "text", "default": "V"},
        ],
    },
    "divider": {
        "label": "Resistive divider",
        "unit": "V",
        "help": "Ratio is (R_top + R_bottom) / R_bottom.",
        "fields": [
            {"key": "r_top", "label": "R top", "unit": "Ω", "default": 11500.0, "min": 0.0},
            {"key": "r_bottom", "label": "R bottom", "unit": "Ω", "default": 1000.0, "min": 1e-9},
        ],
    },
    "shunt": {
        "label": "Current shunt",
        "unit": "A",
        "help": "Ratio is 1 / R_shunt. Low-side only - see design.md on why "
                "high-side would need a current-sense amplifier.",
        "fields": [
            {"key": "r_shunt", "label": "Shunt resistance", "unit": "Ω",
             "default": 0.001, "min": 1e-12},
        ],
    },
    "ct_burden": {
        "label": "Current transformer + burden",
        "unit": "A",
        "help": "Ratio is turns / R_burden. A 2000:1 CT into 100 Ω gives 20 A/V.",
        "fields": [
            {"key": "turns_ratio", "label": "CT turns ratio (N:1)", "unit": "",
             "default": 2000.0, "min": 1e-9},
            {"key": "r_burden", "label": "Burden resistor", "unit": "Ω",
             "default": 100.0, "min": 1e-9},
        ],
    },
    "pt_divider": {
        "label": "Potential transformer + divider",
        "unit": "V",
        "help": "Ratio is PT ratio × (R_top + R_bottom) / R_bottom.",
        "fields": [
            {"key": "pt_ratio", "label": "PT ratio (Vin:Vout)", "unit": "",
             "default": 100.0, "min": 1e-9},
            {"key": "r_top", "label": "R top", "unit": "Ω", "default": 0.0, "min": 0.0},
            {"key": "r_bottom", "label": "R bottom", "unit": "Ω", "default": 1000.0, "min": 1e-9},
        ],
    },
}

# --------------------------------------------------------------------------
# Fields every channel has, whatever its front-end type.
# --------------------------------------------------------------------------

COMMON_FIELDS = [
    {"key": "name", "label": "Name", "type": "text", "default": "Channel"},
    {
        "key": "zero_offset_v", "label": "Zero offset", "unit": "V at pin", "default": 0.0,
        "help": "Subtracted before scaling. Global chop cancels the ADC's own "
                "offset but not the front end's, so this is the one the hardware "
                "cannot fix for you.",
    },
    {
        "key": "gain_correction", "label": "Gain correction", "unit": "×", "default": 1.0,
        "help": "Multiplies the computed ratio. This is the second point of the "
                "two-point calibration design.md asks for on the shunt channel.",
    },
    {
        "key": "r_thevenin", "label": "Source impedance", "unit": "Ω", "default": 0.0,
        "min": 0.0,
        "help": "Thevenin impedance of the front end, used to correct for the "
                "ADC loading it. Zin is derived from the board's reported CLKIN "
                "(330k × 4.096M / fMOD ≈ 676k at 4 MHz). Valid at PGA gain "
                "1-4; leave at 0 on the gain-32 shunt channel.",
    },
    {
        "key": "invert", "label": "Invert polarity", "type": "bool", "default": False,
        "help": "For a CT or shunt wired backwards.",
    },
]

# --------------------------------------------------------------------------
# Defaults, from design.md's ADC channel map. Frame order is fixed by wire.h:
# five int24 codes per frame in ADC order, ADC3 ch1 omitted.
# --------------------------------------------------------------------------

DEFAULTS = [
    {
        "idx": 0, "adc": 1, "ch": 0,
        "name": "Wall current",
        # design.md specifies only "size the burden to ~1.0 V at max", so there
        # is no ratio to apply yet. Direct until someone enters the real CT and
        # burden in the GUI - inventing a number here would be worse than
        # showing volts at the pin and saying so.
        "type": "direct",
        "params": {},
        "zero_offset_v": 0.0, "gain_correction": 1.0, "r_thevenin": 0.0, "invert": False,
        "note": "CT + burden, transformer-isolated. Burden not sized in design.md.",
    },
    {
        "idx": 1, "adc": 1, "ch": 1,
        "name": "Wall voltage",
        # design.md gives the overall figure (~1.0 V at 325 V peak) but not the
        # split between PT ratio and divider, so this is a fixed ratio.
        "type": "fixed",
        "params": {"scale": 325.0, "unit": "V"},
        "zero_offset_v": 0.0, "gain_correction": 1.0, "r_thevenin": 0.0, "invert": False,
        "note": "PT + divider, transformer-isolated. Provisional: design.md gives "
                "~1.0 V at 325 V peak, not component values.",
    },
    {
        "idx": 2, "adc": 2, "ch": 0,
        "name": "12 V rail voltage",
        "type": "divider",
        "params": {"r_top": 11500.0, "r_bottom": 1000.0},
        "zero_offset_v": 0.0, "gain_correction": 1.0, "r_thevenin": 900.0, "invert": False,
        "note": "",
    },
    {
        "idx": 3, "adc": 2, "ch": 1,
        "name": "12 V rail current",
        "type": "shunt",
        "params": {"r_shunt": 0.001},
        "zero_offset_v": 0.0, "gain_correction": 1.0, "r_thevenin": 0.0, "invert": False,
        "note": "Low-side 1 mΩ shunt, PGA gain 32.",
    },
    {
        "idx": 4, "adc": 3, "ch": 0,
        "name": "24 V bus voltage",
        "type": "divider",
        "params": {"r_top": 26000.0, "r_bottom": 1000.0},
        "zero_offset_v": 0.0, "gain_correction": 1.0, "r_thevenin": 1000.0, "invert": False,
        "note": "",
    },
]

NCH = len(DEFAULTS)
assert [c["idx"] for c in DEFAULTS] == list(range(NCH)), "one entry per frame slot"


# --------------------------------------------------------------------------

def adc_input_impedance(clkin_hz):
    """ADS131M0x input impedance at PGA gain 1-4, from design.md:
    330 kOhm x 4.096 MHz / fMOD, and fMOD is half of CLKIN."""
    f_mod = clkin_hz / 2.0
    return 330e3 * 4.096e6 / f_mod


def channel_unit(cfg):
    if cfg["type"] == "fixed":
        return cfg.get("params", {}).get("unit") or "V"
    return FRONTEND_TYPES[cfg["type"]]["unit"]


def channel_scale(cfg, clkin_hz=4_000_000):
    """Units per volt at the ADC pin, for the whole chain."""
    t = cfg["type"]
    p = cfg.get("params", {})

    if t == "direct":
        s = 1.0
    elif t == "fixed":
        s = float(p.get("scale", 1.0))
    elif t == "divider":
        s = (float(p["r_top"]) + float(p["r_bottom"])) / float(p["r_bottom"])
    elif t == "shunt":
        s = 1.0 / float(p["r_shunt"])
    elif t == "ct_burden":
        s = float(p["turns_ratio"]) / float(p["r_burden"])
    elif t == "pt_divider":
        s = float(p["pt_ratio"]) * (float(p["r_top"]) + float(p["r_bottom"])) / float(p["r_bottom"])
    else:
        raise ValueError(f"unknown front-end type {t!r}")

    # The divider is loaded by the ADC, so what reaches the pin is low by
    # Zin/(Zin+Rth). Correcting it needs the source impedance, which is the one
    # thing the board cannot tell us.
    rth = float(cfg.get("r_thevenin", 0.0) or 0.0)
    if rth > 0:
        zin = adc_input_impedance(clkin_hz)
        s *= (zin + rth) / zin

    s *= float(cfg.get("gain_correction", 1.0) or 1.0)
    return -s if cfg.get("invert") else s
