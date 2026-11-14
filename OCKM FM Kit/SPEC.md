# OCKM FM Kit — specification (v1)

> **Governing rule: keep it lean.** Eight FM drums, five knobs each, one routing switch, one
> master level. No per-operator editing. Everything else goes in section 9 ("Not in v1").

Status: **built (v1), not yet tried on the MPC.** Simon approved this spec as written (multi-out kept, these fifth knobs, bronze). Decisions taken while building are in section 12 and marked "(build)".

OCKM FM Kit is a drum-kit **instrument** for the MPC One. Each pad plays one synthesised drum
built from two or three sine-wave "operators" that bend each other's pitch (FM). It is the
drum half of an all-OCKM track: OCKM FM Kit for drums, OCKM-03 for the bass, OCKM Wave for
chords and pads, OCKM Gate for pumping. Naming rule: no other companies' product or brand
names anywhere (see ../HOUSE-STYLE.md); presets describe the sound.

| Item | Value |
|---|---|
| Plugin name | `OCKM FM Kit` (permanent) |
| Manufacturer | `OCKM` |
| uid | `O0OH` (from `new_plugin.py`; permanent) |
| Kind | instrument, 0 in / **4 out** (Main 1-2, Aux 3-4) |
| Output folder | `OCKM - VST - OCKM FM Kit/` |
| Project folder | `OCKM FM Kit/` in the MPC-Plugins repo |
| Licence | MIT, `Copyright (c) OCKM` |
| Look | brushed metal, **bronze** tint (`make_skin.py` copied from OCKM Gate, `--tint bronze`; champagne is OCKM Wave's) |
| Voices | 8 (one per drum; a drum retriggers itself) |
| CPU budget | section 7: about 1–2% of one MPC One core in normal use, under 5% worst case |
| Inspiration | the SDK's MIT `examples/DrumSynth` (note map, wrapping, multi-out). Ideas only |

## 1. What FM means here (in plain English)

An **operator** is a sine wave with its own volume envelope. In FM, one operator (the
**modulator**) doesn't go to the speakers; it wobbles the pitch of another (the **carrier**)
thousands of times a second. That wobble turns a pure sine into a richer tone.

- **How much** it wobbles (the **index**, our `Tone` knob) sets brightness: 0 = pure sine,
  a little = warm and knocky, a lot = buzzy, clangy or noisy.
- **How fast** the modulator runs compared with the carrier (the **ratio**) sets the kind of
  tone: whole-number ratios (1, 2, 3) sound pitched and musical; odd ratios (1.41, 3.7) sound
  metallic, like bells, rims and cymbals.
- A **fast envelope on the modulator** makes the bright part a short click at the start of the
  hit while the body rings on pure. That is the key FM drum trick.

## 2. The kit (notes, recipes, the fifth knob)

Notes 36–43 play the eight drums. Every other note wraps onto the same eight (44 = Kick again,
35 = Perc), so any pad bank or a keyboard plays the kit. Notes only choose the drum; they don't
change its pitch (use `Tune`). Note-offs are ignored: each drum is a one-shot.

| Note | Drum | 5th knob | Operators | Recipe |
|---|---|---|---|---|
| 36 | **Kick** | `Punch` | 2 | Carrier sine at ~48 Hz with a fast pitch drop from high to low (Punch = how far it drops). A modulator at ratio 1 with a 15 ms envelope adds the "knock" at the start (Tone = how hard). Body stays a clean sine for the sub. |
| 37 | **Snare** | `Snap` | 2 + noise | Carrier ~185 Hz, modulator at ratio 1.6 (inharmonic, gives the drum-shell ring), short pitch drop. White noise through a cheap high-pass gives the wires (Snap = noise amount and length). |
| 38 | **Clap** | `Spread` | 1 + noise | A carrier at ~1.2 kHz whose pitch is wobbled by **noise** instead of a sine: that's FM making band-limited noise, no filter needed (Tone = how wide the noise band is). Played as three quick bursts then a tail (Spread = gap between bursts, tight to loose). |
| 39 | **Closed Hat** | `Metal` | 3 | Classic FM metal: op 3 wobbles op 2, which wobbles op 1, all at high, odd ratios, then a high-pass. Tone = index (shimmer to hiss); Metal = ratio (glassy to clangy). Short. **Chokes the open hat.** |
| 40 | **Open Hat** | `Metal` | 3 | Same as the closed hat, longer decay. |
| 41 | **Low Tom** | `Bend` | 2 | Like the kick but higher (~95 Hz) with a slower, smaller pitch drop (Bend = how much). Modulator at ratio 1 for the stick attack. |
| 42 | **High Tom** | `Bend` | 2 | Same, ~160 Hz. Also good as a pitched techno stab. |
| 43 | **Perc** | `Ratio` | 2 | Carrier ~800 Hz with a short-decay modulator. Ratio sweeps from woody (rim/block) through bell to clangy metal. Short decays give a rim click, longer ones a bell. |

Per-drum knobs (all eight):

- `Tune` −24…+24 semitones (default 0). The pitch of the whole drum; the ratios follow it.
- `Decay` 0–100%. Length to −60 dB, mapped to a range that suits each drum (kick 80–1500 ms,
  closed hat 15–300 ms, open hat 150–2000 ms, and so on).
- `Tone` 0–100%. The FM index: how bright, knocky or noisy (section 1).
- `Level` 0–100% (default 80).
- 5th knob 0–100%, as in the table.

Velocity sets loudness (soft hits about −10 dB) and a little brightness (harder = slightly more
Tone), like a real drum. Not adjustable in v1.

Each drum has a fixed position in the stereo field, close to centre (kick and snare centre, hats
slightly apart, toms left and right), so the kit sounds like a kit without pan knobs.

## 3. Outputs and routing

`numOutputs = 4`: the MPC sees a Main pair (outputs 1-2) and an Aux pair (3-4) and can send
each to its own mixer channel. `Routing` chooses what goes to Aux:

| Routing | Main 1-2 | Aux 3-4 | Use |
|---|---|---|---|
| **All Main** (default) | everything | silent | normal use, one channel |
| **Kick > Aux** | everything but the kick | kick | sidechain or compress the kick on its own |
| **Hats > Aux** | everything but the hats | both hats | send hats to their own effects |
| **Split** | kick, snare, clap | hats, toms, perc | two buses |

If the host only gives two outputs, everything goes to Main. Master level applies to both pairs.

## 4. Signal flow

```
MIDI note -> drum = (note - 36) wrapped to 0..7 -> (closed hat? kill open hat) -> trigger drum
each drum: 2-3 sine operators (one shared sine table, phase modulation) + noise where noted
         x exponential envelopes (amp, pitch, modulator) -> fixed pan -> Main or Aux bus
buses -> Master (smoothed) -> soft safety limiter -> outputs
```

- **Envelopes** are plain exponential decays: one multiply per sample. Coefficients are worked
  out once per hit, never per sample.
- **Sine** comes from one 2048-point table built in the constructor, read with linear
  interpolation. FM is done as phase modulation (the modulator's output is added to the
  carrier's read position), which is cheap and stays in tune.
- **No clicks on retrigger or choke:** when a ringing drum is restarted or choked, its last
  sample fades out over about 1.5 ms instead of jumping to zero.
- **Never clips:** levels are set so a single drum peaks near −6 dBFS, and a soft limiter on
  each output (a rational curve, no `tanh`, only above about −3 dBFS) keeps the sum below 0 dBFS
  even when everything hits at once at full level. The harness checks this on every preset.
- **No denormals:** voices stop when their envelopes drop below −70 dB; filter states are reset
  on each hit.

## 5. Parameters (order and ids permanent once built)

Parameter 0 is Master, which is harmless when the MPC nudges it on load. Per-drum parameters
are added in a loop: index = 2 + drum × 5 + knob. New parameters only ever go on the end.

| # | id | Name (≤ 12 chars) | Range / choices | Default |
|---|---|---|---|---|
| 0 | `master` | Master | −40…+6 dB | −3 dB |
| 1 | `routing` | Routing | All Main / Kick>Aux / Hats>Aux / Split | All Main |
| 2–6 | `kick_tune`, `kick_decay`, `kick_tone`, `kick_level`, `kick_punch` | Kick Tune / Decay / Tone / Level / Punch | ±24 st, 0–100% ×4 | 0, 50, 30, 80, 50 |
| 7–11 | `snare_tune` … `snare_snap` | Snare … / Snare Snap | | 0, 45, 35, 80, 55 |
| 12–16 | `clap_tune` … `clap_spread` | Clap … / Clap Spread | | 0, 45, 50, 80, 40 |
| 17–21 | `chat_tune` … `chat_metal` | CHat … / CHat Metal | | 0, 30, 60, 70, 40 |
| 22–26 | `ohat_tune` … `ohat_metal` | OHat … / OHat Metal | | 0, 50, 60, 65, 40 |
| 27–31 | `ltom_tune` … `ltom_bend` | LTom … / LTom Bend | | 0, 50, 25, 75, 40 |
| 32–36 | `htom_tune` … `htom_bend` | HTom … / HTom Bend | | 0, 50, 25, 75, 40 |
| 37–41 | `perc_tune` … `perc_ratio` | Perc … / Perc Ratio | | 0, 30, 45, 70, 30 |

42 parameters. The 5th-knob ids are `kick_punch`, `snare_snap`, `clap_spread`, `chat_metal`,
`ohat_metal`, `ltom_bend`, `htom_bend`, `perc_ratio`. The defaults above shipped unchanged (build).

## 6. UI (three pages; every Q-Link bank is a column of four)

The MPC One has 4 Q-Links and 16 slots per page; each column below is one Q-Link bank.

**KIT 1**

| Kick | Snare | Clap | Closed Hat |
|---|---|---|---|
| Tune | Tune | Tune | Tune |
| Decay | Decay | Decay | Decay |
| Tone | Tone | Tone | Tone |
| Level (fader) | Level (fader) | Level (fader) | Level (fader) |

**KIT 2**

| Open Hat | Low Tom | High Tom | Perc |
|---|---|---|---|
| Tune | Tune | Tune | Tune |
| Decay | Decay | Decay | Decay |
| Tone | Tone | Tone | Tone |
| Level (fader) | Level (fader) | Level (fader) | Level (fader) |

**SHAPE** (build: renamed from CHARACTER, which is too long for the MPC's tab)

| Drums 1–4 | Drums 5–8 | | Output |
|---|---|---|---|
| Kick Punch | OHat Metal | *OCKM FM KIT badge* | Routing (menu) |
| Snare Snap | LTom Bend | *empty* | *empty* |
| Clap Spread | HTom Bend | *empty* | *empty* |
| CHat Metal | Perc Ratio | *empty* | Master (fader) |

Tune knobs are drawn bipolar (centre = 0).

## 7. CPU budget (MPC One)

Cost per drum per sample, while it rings:

| Drum | Work per sample |
|---|---|
| Kick, toms, perc | 2 table reads, ~3 envelope multiplies |
| Snare | 2 table reads, 1 noise sample, 1 one-pole filter, ~4 multiplies |
| Clap | 1 table read, 1 noise sample, ~3 multiplies |
| Hats | 3 table reads, 1 one-pole filter, ~2 multiplies |

Roughly 30–60 CPU cycles per ringing drum per sample on a 32-bit ARM core. Budget at 48 kHz:

- **Typical groove** (kick + hat + one more ringing at once): ~150 cycles × 48,000 ≈ 7 M
  cycles/s, **about 1% of one core**.
- **Worst case** (all 8 ringing at full length): ~400 cycles × 48,000 ≈ 20 M cycles/s, **under
  2–5% of one core** depending on clock speed.
- Silent drums cost nothing: a drum switches itself off at −70 dB. Master smoothing and the
  limiter add a few multiplies per output sample.

host_sim's CPU figure is checked against this; if the worst case comes out above 5% of a block
I'll simplify before going further.

## 8. Presets (built-in programs, 8, descriptive names only)

1. **Init Kit**: the defaults (the MPC loads program 1 first).
2. **Deep House Kit**: round, medium kick; soft snare with little snap; wide loose clap; gentle,
   dark hats; woody perc.
3. **Warehouse Techno**: long, low, punchy kick; tight metallic closed hat; trashy open hat;
   rim-like perc; short toms.
4. **Minimal Clicks**: very short, dry everything; clicky kick, tiny hats, high woodblock perc.
5. **Industrial Clang**: high Tone everywhere; clangy hats and bell-metal perc; distorted-
   sounding snare; detuned toms.
6. **Rumble Techno**: deep kick with a long tail, low tom tuned down as a rumble layer, crisp
   short hats.
7. **Lo-Fi House**: dull, low-Tone hats; fat snare; slow loose clap; detuned-down kit.
8. **Acid Partner Kit**: tight, bright kit that sits under OCKM-03: short kick, snappy clap,
   ringing open hat, bell perc.

## 9. Not in v1

- Per-operator editing (ratios, envelopes and levels for each operator), algorithms, operator
  feedback.
- More than eight drums, layering two drums on one pad, or user-defined choke groups (only
  closed hat → open hat).
- Pitch from the note played (notes only choose the drum).
- Per-drum pan, filter, drive or send knobs; velocity curve and velocity-to-tone amount.
- Built-in sequencer, accent, swing, or effects (use the MPC's and OCKM Gate).
- Sample layers, user samples.
- More than one Aux pair.

## 10. How it will be tested

1. **Build:** `python3 $SDK/tools/build.py "OCKM FM Kit"` → 0 validator errors.
2. **host_sim:** `host_sim ... test --wav build/test.wav` → no FAIL; every WARN explained (its
   generic test plays notes near middle C, which wrap onto drums here, so it should still hear
   sound).
3. **tests/harness.cpp** (`#include`s the plugin source, ends with `ALL PASS`):
   - every drum (notes 36–43) makes sound, and stops by itself (one-shot, no endless tail);
   - wrapping: notes 44, 35, 60 and 127 play the same drum as their wrapped note (identical
     output);
   - choke: closed hat after open hat cuts the open hat's tail, without a click (largest
     sample-to-sample jump stays small);
   - retriggering a ringing kick doesn't click;
   - **no clipping**: for every preset, a dense pattern with all eight drums at velocity 127
     peaks below 0 dBFS, and the Init Kit with all levels and Master at maximum and every drum
     struck together also stays below 0 dBFS;
   - routing: under each Routing choice, each drum lands on the bus the table in section 3
     says, and with only 2 outputs everything lands on Main;
   - Master at −40 dB is much quieter; parameter 0 changes cause no other side effect;
   - no NaN/denormal values; CC 120 silences everything;
   - Tone and the 5th knob actually change the sound (output differs at 0% and 100%).
4. **Demo WAV:** `build/demo.wav`, a 4-bar house/techno pattern (four-to-the-floor kick,
   offbeat open hat choked by 16th closed hats, clap on 2 and 4, perc and tom fills) played
   through each preset in turn.
5. **Package:** `python3 $SDK/tools/package.py "OCKM FM Kit"`.
6. Skin: `make_skin.py --tint bronze`, re-run after any `ui.json` change.

## 11. Open questions (answered)

Simon said "build it" to the spec as written: multi-out with `Routing` kept, the fifth knobs as
listed, bronze tint.

## 12. Build notes

- (build) Third page renamed **SHAPE**; `gen_ui.py` warns that tab names over 8 characters may be cut off.
- (build) Hat base pitch 2400 Hz (was going to be 3200): the first render's brightness centre sat at
  13 kHz, which sounded thin and fizzy on paper.
- (build) Balance: kick ×1.4 so it leads (−11 dBFS for one Init hit), clap ×0.55 (it was the loudest
  drum, −8 dBFS); hats sit around −18 dBFS.
- (build) Limiter knee at 0.75 (−2.5 dBFS). Results: the groove peaks −4.6 to −5.6 dBFS on every preset
  (the limiter never acts); all eight drums at once at velocity 127 reach −0.8 to −2.5 dBFS; everything
  at maximum reaches −0.18 dBFS.
- (build) CPU: all eight drums ringing at maximum Decay and Tone took 0.34% of one Apple M3 core at
  48 kHz, which works out at roughly 2–3.5% of an MPC One core. host_sim: average 0.04% per block.
- (build) Panel font gained W, 1, 2, 4, 5 and 8; panel file names use underscores (`panel_kit_1.png`).
- (build) Tests: build 0 errors / 0 warnings, host_sim 19/19 PASS with no WARN, harness ALL PASS
  (17 checks). Spectrum check: kick and toms are pure and low (centre 170–300 Hz), snare and clap
  noisy, hats bright, perc metallic.
