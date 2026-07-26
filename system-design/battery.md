# Battery pack and BMS

Everything battery-related lives here: the pack configuration, the JK BMS setup, charger
settings, and the vendor documentation. `system-design/design.md` covers how the pack
attaches to the 24 V bus; `system-design/rack-manager.md` covers how the firmware reads it
and decides to shut the rack down.

## Pack configuration

**7S LiFePO4, 50 Ah** - 7x EVE LF50K 3.2 V grade-A cells, JK-B1A8S10P BMS.

Originally specced as 8S. Dropped to 7S because eight cells plus the BMS do not
physically fit the 10" rack enclosure with any workable margin. The BMS supports 3-8S
natively, so this costs nothing but voltage.

|                            | 8S (original) | **7S (built)**        |
| -------------------------- | ------------- | --------------------- |
| Nominal (3.2 V/cell)       | 25.6 V        | **22.4 V**            |
| Full (3.65 V/cell)         | 29.2 V        | **25.6 V**            |
| Charge CV (3.45 V/cell)    | 27.6 V        | **24.2 V**            |
| Orion-Tr 18 V floor equals | 2.25 V/cell   | **2.57 V/cell**       |
| Energy                     | 1.28 kWh      | **1.12 kWh** (87.5 %) |
| Runtime, full load         | ~27 h         | **~24 h**             |
| Runtime, shed              | ~38 h         | **~33 h**             |

Capacity in Ah is unchanged - 50 Ah either way. Only energy drops.

### Consequences of 7S

**The Orion-Tr's 18 V floor is now the binding low-end constraint, not the BMS.**
At 8S, 18 V corresponds to 2.25 V/cell, far below any sane cutoff, so the buck's input
range never mattered. At 7S it corresponds to **2.57 V/cell**, which is inside the range
people set UV cutoffs to. So:

> **The BMS cell UV cutoff must stay above ~2.6 V/cell**, or the 12 V rail dies from buck
> dropout before the BMS protects anything - an unannounced hard power yank with the BMS
> still reporting normal. The planned 2.8-3.0 V/cell (see `rack-manager.md` §4) satisfies
> this, but the margin is now ~0.2-0.4 V/cell instead of ~0.6, and pack sag under load
> eats into it. Do not lower the UV setpoint without re-checking this.

**Same power means more current.** At 7S the pack delivers the same watts at 87.5 % of the
voltage, so currents are ~14 % higher than the 8S design assumed. The 20 A main fuse and
2.5 mm² branch wiring still cover the actual ~50 W / 90 W peak load with large margin.

**ADC3 Ch0 range shrinks slightly.** The 24 V bus divider (~26k/1k, ÷27) was sized so
28.8 V lands at 1.07 V of the 1.2 V FSR. Peak bus voltage is now 25.6 V -> ~0.95 V. Still
fine, just less of the range used. Boards are already manufactured; no change.

## Sense harness wiring for 7S

JK documents reduced cell counts directly - compare the 4S and 8S diagrams in
[Wiring-Diagram.pdf](JK-B1A8S-10P-V19.3.1-product-docs/Wiring-Diagram.pdf). Header pinout
is `B+ B8 B7 B6 B5 B4 B3 B2 B1 B-`.

In the 4S diagram, `B+` moves down to the **top cell's positive** and pins `B8 B7 B6 B5`
are left **completely unconnected**. Unused channels are not bridged to anything.

So for 7S:

| Pin         | Connect to                                    |
| ----------- | --------------------------------------------- |
| `B-`        | pack total negative                           |
| `B1`...`B7` | positive terminal of cells 1...7              |
| `B+`        | **cell 7's positive** (= pack total positive) |
| `B8`        | **nothing**                                   |

Two conditions make this safe:

1. **Set 单体数量 (Number of cells) = 7 in the app.** The parameter guide is explicit:
   *"Please set the value accurately before use, otherwise the protection board cannot
   work normally."* This is what tells the board to ignore the open channel instead of
   reading it as a 0 V cell and latching undervoltage.
2. **Dispose of the unused harness wire properly.** Either depin it from the connector
   housing, or leave it pinned at the BMS end (harmless - high-impedance input) and cut +
   heatshrink the free end. Never the reverse: a wire bolted to a cell but loose at the
   other end is live and is a short waiting to happen.

Per the vendor safety notes: connect from negative to positive, and **verify the whole
harness with a multimeter at the connector before plugging it into the board** - each
adjacent pair 3.2-3.4 V, monotonically increasing, `B-`->`B+` equal to pack voltage.
Miswiring this is the most common way these boards die.

## Cable sizing

Inter-cell links are **wires, not busbars** - none were on hand, and flexible links have a
real advantage: prismatic LFP cells swell under compression, and wire doesn't transmit
that stress into the terminal studs.

Sized to the BMS's **100 A design ceiling** rather than the actual load, for upgrade
headroom. Every series link carries full pack current, so they all get the same gauge.

| Max sustained current   | Copper     | ~AWG    |
| ----------------------- | ---------- | ------- |
| 30 A                    | 6 mm²      | 10      |
| 50 A                    | 10 mm²     | 8       |
| 80 A                    | 16 mm²     | 6       |
| **100 A (BMS ceiling)** | **25 mm²** | **4-3** |

Note the system is currently protected at **20 A** (main fuse at battery +) with 2.5 mm²
branches. The 25 mm² links are deliberately over-spec; going past 20 A in future means
changing the fuse and the branch wiring too, not just the links.

Details that matter:

- **Tap sense leads directly on the cell terminal bolt**, not at the far end of the link.
  At 100 A a 15 cm run of 25 mm² drops ~10 mV, and the BMS acquires to ±3 mV.
- **Crimped copper lugs**, hydraulic crimper, heatshrink over the barrel. No solder-only
  lugs - solder wicks into the strands and creates a stiff stress riser exactly where the
  wire flexes.
- **Unequal link lengths do not cause imbalance** in a series string; the same current
  flows through every link by definition. Length only affects drop and heating. (This is
  only a real concern for parallel links.)
- Balance leads carry up to **1 A** during active balancing. If fusing them, use ~2 A -
  the commonly-suggested 200 mA fuses will nuisance-blow.
- Do not modify the BMS's own P-/B- power leads. Vendor warning: *"will cause uneven
  overcurrent of the protection board and burn the protection board."*

## Charger configuration (NPB-360-24TB)

The DIP-selected "Li 2-stage, CV 28.8 V" profile is **for 8S and must not be used as-is**
- 28.8 V across 7S is 4.11 V/cell, which destroys the cells.

Measured pot range bottoms out at 18 V, so 7S is comfortably reachable.

| Setting            | Value       | = V/cell |
| ------------------ | ----------- | -------- |
| Charger CV         | **24.2 V**  | 3.45     |
| BMS cell OV cutoff | 25.55 V     | 3.65     |
| BMS cell UV cutoff | 19.6-21.0 V | 2.8-3.0  |
| Orion-Tr dropout   | 18 V        | 2.57     |

CV is set at 3.45 V/cell rather than 3.55+ for the same reason as the original 8S note:
the NPB holds CV continuously, and holding LiFePO4 at full ages it.

**Set the pot by measurement, not knob position.** The pot spans 18 V to ~29 V, so the
gap between 3.45 and 3.65 V/cell is a sliver of rotation. Set it open-circuit with a
multimeter and re-check once warm. Verify the profile's other thresholds (float, taper,
re-charge) in V/cell terms too - they derive from CV and the DIP profile assumes a 24 V
nominal pack.

### Activation needs the K+/K- switch at 7S

The board has no power switch. Default activation is by charger voltage **>= 2 V above
pack voltage** (manual §6.2). At 7S with CV correctly set to 24.2 V, a freshly
top-balanced pack rests around 23.5 V - only ~0.7 V of headroom. **Charge-activation will
not reliably work.**

Fit the activation switch across **P2 pins 1/2 (`K-`/`K+`)** instead. This sidesteps the
problem entirely and is listed in the vendor accessories. Alternative if no switch is
available: activate once while the pack is at mid-SoC (~3.2 V/cell = 22.4 V), where the
2 V rule is satisfied, before topping it off.

## Commissioning checklist

Before the pack goes in the rack, in order:

1. **Check each cell** - OCV and internal resistance. Reject outliers now, not after
   they're bolted into a string.
2. **Top-balance before series assembly.** Parallel all 7 cells, charge to 3.65 V, hold
   until current tails off. Not optional: the BMS balances at 1 A, which takes days to
   correct a spread that top-balancing fixes in one pass.
3. **Compression fixture**, per the EVE LF50K datasheet.
4. **Torque terminals to the cell datasheet spec** (typically 4-6 Nm for M6). The BMS
   docs give no figure. Over-torquing strips the aluminium insert and kills the cell.
5. **Assemble with insulated tools.** One wrench at a time, rings and watch off.
6. **Verify the sense harness with a multimeter** (see above) before plugging it in.
7. **Main fuse** (20 A) at battery positive.
8. **Set BMS parameters before connecting any load**: cell count = 7, capacity = 50 Ah,
   OV/UV cutoffs, temperature limits. Change the default passwords (`1234` /
   `123456`) while in there.
9. **Confirm the pack NTCs are on the cells**, not floating in air - the sub-0 °C charge
   block is the protection that actually matters for LiFePO4.

## Open items

- **Board variant unconfirmed.** Labelled "CDH"; the manual's variant table has no `D`.
  Suffixes are `C` = CAN, `R` = RS485, `H` = heating (plus `HC`/`HR`/`HCR`). Most likely
  `-10PHC` (CAN + heating). Confirm from the label - it decides whether RS485 exists.
- **Transport for telemetry.** `rack-manager.md` §4 assumes RS485 on UART0. If the board
  turns out to be CAN-only, note that the **display port (P2, `A`/`B`) carries RS485 and
  is standard on every variant** - so the display protocol is the guaranteed data path
  regardless of which optional interfaces are fitted. UART is also listed as standard.
- **Heating.** If the board is an `H` variant, see
  [the heating guide](JK-B1A8S-10P-V19.3.1-product-docs/JK-Protection-Board-Heating-Function-Guide-V1.4.1.pdf).
  Relevant only if the rack sits somewhere that goes below 0 °C.

## Vendor documentation

Four packages, one per hardware/firmware revision. **Use
[JK-B1A8S-10P-V19.3.1-product-docs/](JK-B1A8S-10P-V19.3.1-product-docs/)** - 2025
production is the V19.x generation, and 19.3.1 is the later of the two.

The other three are kept for reference: the two V15.3.3 folders are the previous
generation (one of them is the **10PS** series, a different board), and V19.1.1 is the
earlier V19 revision.

It mostly does not matter which you open. Verified by checksum:

- All five protocol PDFs in `Communication-Protocols/` are **byte-identical across all
  four packages**, as are the parameter-settings and dry-contact guides.
- V19.1.1 and V19.3.1 additionally share an identical heating guide and 3D STEP model.
- Only the **user manual, outline drawing and wiring diagram** differ between the two V19
  revisions.
