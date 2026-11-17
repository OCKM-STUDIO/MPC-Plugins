# OCKM FM Kit — specification (v1)

> **Governing rule: keep it lean.** Eight FM drums, five knobs each, one routing switch, one
> master level. No per-operator editing. Everything else goes in section 9 ("Not in v1").

Status: **v1.2.1 built** (Variation, Vel>Morph, Drive, section 14; Morph, section 13). v1 worked on Simon's MPC One (routing not yet tried). Simon approved this spec as written (multi-out kept, these fifth knobs, bronze). Decisions taken while building are in section 12 and marked "(build)".

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

## 2. The kit (notes, recipes, Morph)

Notes 36–43 play the eight drums. Every other note wraps onto the same eight (44 = Kick again,
35 = Perc), so any pad bank or a keyboard plays the kit. Notes only choose the drum; they don't
change its pitch (use `Tune`). Note-offs are ignored: each drum is a one-shot.

| Note | Drum | Operators | Recipe (at Morph 50%) |
|---|---|---|---|
| 36 | **Kick** | 2 | Carrier sine at ~48 Hz with a fast pitch drop. A modulator at ratio 1 with a short envelope adds the "knock" at the start (Tone = how hard). |
| 37 | **Snare** | 2 + noise | Carrier ~180 Hz, modulator at ratio 1.6 (the shell ring), fast pitch drop, short body; plus band-passed noise (the wires, about 1.2–9 kHz) that is louder and longer than the body, with an extra 12 ms burst at the start (the crack). v1.2 rework, section 14. |
| 38 | **Clap** | 1 + noise | A carrier at ~1.2 kHz whose pitch is wobbled by **noise**: FM making a band of noise, no filter needed (Tone = band width). Three quick claps then a tail. |
| 39 | **Closed Hat** | 3 | FM metal: op 3 wobbles op 2, which wobbles op 1, at odd ratios, then a high-pass. Tone = index. Short. **Chokes the open hat.** |
| 40 | **Open Hat** | 3 | Same, longer. |
| 41 | **Low Tom** | 2 | Like the kick but higher (~95 Hz) with a slower pitch bend; modulator at ratio 1 for the stick attack. |
| 42 | **High Tom** | 2 | Same, ~160 Hz. |
| 43 | **Perc** | 2 | Carrier ~800 Hz with an odd-ratio modulator and a little feedback: woodblock / cowbell. |

**Morph** (v1.1) blends each drum through three characters. It moves several hidden settings
at once (ratios, pitch-drop depth and speed, how long the bright click lasts, noise, feedback):

| Drum | 0% | 50% | 100% |
|---|---|---|---|
| Kick | round sub, almost no click | punchy classic kick | hard FM boom with a zap (big fast drop, carrier feedback) |
| Snare | tight, tuned snare (body-led) | classic snare (wire-led) | noisy crack with a metallic ring |
| Clap | one tight, higher snap | classic three-clap | wide, lower five-clap |
| Hats | soft glassy ping (a few pure partials) | classic metal | trashy, noisy, cymbal-like wash that rings longer |
| Toms | soft round tom | synth tom with a big bend | metallic conga / ping (ratio 3.5) |
| Perc | rim / clave click | woodblock / cowbell | long bell / chime |

Per-drum knobs (all eight):

- `Tune` −24…+24 semitones (default 0). The pitch of the whole drum; the ratios follow it.
- `Decay` 0–100%. Length to −60 dB, mapped to a range that suits each drum (kick 80–1500 ms,
  closed hat 15–300 ms, open hat 150–2000 ms, and so on).
- `Tone` 0–100%. The FM index: how bright, knocky or noisy (section 1).
- `Level` 0–100% (default 80).
- `Morph` 0–100% (default 50%), as in the table above.

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
| 2–6 | `kick_tune`, `kick_decay`, `kick_tone`, `kick_level`, `kick_punch` | Kick Tune / Decay / Tone / Level / Morph | ±24 st, 0–100% ×4 | 0, 50, 30, 80, 50 |
| 7–11 | `snare_tune` … `snare_snap` | Snare … / Snare Morph | | 0, 45, 35, 80, 50 |
| 12–16 | `clap_tune` … `clap_spread` | Clap … / Clap Morph | | 0, 45, 50, 80, 50 |
| 17–21 | `chat_tune` … `chat_metal` | CHat … / CHat Morph | | 0, 30, 60, 70, 50 |
| 22–26 | `ohat_tune` … `ohat_metal` | OHat … / OHat Morph | | 0, 50, 60, 65, 50 |
| 27–31 | `ltom_tune` … `ltom_bend` | LTom … / LTom Morph | | 0, 50, 25, 75, 50 |
| 32–36 | `htom_tune` … `htom_bend` | HTom … / HTom Morph | | 0, 50, 25, 75, 50 |
| 37–41 | `perc_tune` … `perc_ratio` | Perc … / Perc Morph | | 0, 30, 45, 70, 50 |

| 42 | `variation` | Variation | 0–100% | 0 (v1.2) |
| 43 | `velmorph` | Vel>Morph | 0–100% | 0 (v1.2) |
| 44 | `drive` | Drive | 0–100% | 0 (v1.2) |

45 parameters (42 until v1.1; the three kit-wide knobs were added at the end so no number moved). The Morph ids are the original v1 ids, kept because ids are permanent: `kick_punch`, `snare_snap`, `clap_spread`, `chat_metal`,
`ohat_metal`, `ltom_bend`, `htom_bend`, `perc_ratio`. Only their on-screen names changed (v1.1).

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

| Drums 1–4 | Drums 5–8 | Feel | Output |
|---|---|---|---|
| Kick Morph | OHat Morph | *OCKM FM KIT badge* | Routing (menu) |
| Snare Morph | LTom Morph | Variation | *empty* |
| Clap Morph | HTom Morph | Vel>Morph | *empty* |
| CHat Morph | Perc Morph | Drive | Master (fader) |

Tune knobs are drawn bipolar (centre = 0).

## 7. CPU budget (MPC One)

Cost per drum per sample, while it rings:

| Drum | Work per sample |
|---|---|
| Kick, toms, perc | 2 table reads, ~3 envelope multiplies |
| Snare | 2 table reads, 1 noise sample, 2 one-pole filters, ~6 multiplies |
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
   - Tone changes every drum; Morph changes every drum a lot (0% vs 100%: brightness or length
     moves by 1.5× or more; printed per drum).
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

## 13. v1.1: Morph (2026-11-14)

Simon asked for a knob that "really shifts the timbre", the kind of morph control some FM drum
machines have. Changes:

- The fifth knob on every drum is now **Morph** (table in section 2). On screen it's "<Drum> Morph".
  Its id, position and range are unchanged (`kick_punch` … `perc_ratio`), so saved projects load
  without errors, but drums saved with v1 sound different at the same knob position. Renaming the
  on-screen name bends the house rule "never rename a parameter". Simon chose it ("Let's call it
  Morph"); the rule exists to protect saved values, and they're still safe.
- New: operator **feedback** (an operator wobbling itself, from pure to buzzy to noisy), used by
  Morph on the kick (carrier), snare and hats (top operator) and perc (carrier). It costs two
  multiply-adds per sample.
- The clap's Morph sets the number of claps (1 to 5) as well as their spacing and pitch.
- The 8 kits were retuned around Morph: for example Minimal Clicks uses a single-snap clap, rim
  perc and conga toms, Industrial Clang has everything near 100%, Acid Partner Kit has a bell perc.
- Version 1.1.0 (`plugin.json`, `info.version` 1100).
- Results: build 0 errors / 0 warnings; host_sim 19/19, no WARN; harness ALL PASS (17 checks, the
  Morph check is new). Morph 0→100%: kick brightness 55 → 2359 Hz, snare 709 → 8629 Hz, clap
  1 → 5 claps (108 → 223 ms), toms ×6.6, perc 16 → 101 ms; hats go from a pure tone (spectral
  flatness 0.03) to noise (0.75) and ring 1.9× longer. Grooves peak −3.7 to −6.7 dBFS; everything
  at maximum reaches −0.14 dBFS.
- CPU: all eight ringing at maximum Decay and Tone now measures about 0.5% of an Apple M3 core
  (was 0.34%), roughly 3–5% of an MPC One core in that worst case. That's at the top of the budget,
  and normal grooves stay around 1%.

## 14. v1.2: Variation, Vel>Morph, Drive (2026-11-14)

Simon picked these from a list of next steps. Three kit-wide knobs, in the FEEL column of SHAPE
(parameters 42–44, appended so nothing moved). All default to 0%, where the kit is sample-for-sample
the same as 1.1, so saved projects sound exactly as before.

- **Variation**: every hit is a little different. At 100%: pitch ±35 cents, Tone ±25%, Decay ±12%,
  Level ±1.3 dB, Morph ±12%, drawn fresh per hit from the plugin's own random numbers. At 0% no
  random numbers are drawn.
- **Vel>Morph**: velocity also moves Morph. Velocity 95 leaves it alone; 127 adds 40% and 64 or
  below takes 40% away at full amount (straight line between). Clamped to 0–100%.
- **Drive**: blends in a saturated copy (smooth tanh-like curve, input boosted up to 10×, scaled so
  full scale stays full scale). Body and tail come up, peaks round off. Share per drum: kick 100%,
  snare and toms 80%, clap 70%, perc 60%, hats 50%. Set when a drum is hit (turning it changes the
  next hit, not one that is ringing). At 0% the code is skipped.
- Kits: Init all 0; Deep House 25/30/10; Warehouse 15/40/45; Minimal 10/50/0; Industrial 20/30/80;
  Rumble 15/25/35; Lo-Fi 40/20/25; Acid Partner 15/35/30 (Variation / Vel>Morph / Drive).
- Version 1.2.0 (`info.version` 1200).
- Results: build 0 errors / 0 warnings; host_sim 19/19; harness ALL PASS (20 checks, 3 new).
  Variation 100 spreads closed-hat brightness ×1.06 (0: identical hits). Vel>Morph 100 makes a hard
  snare 8× brighter than a soft one (×1.0 when off). Drive 100: every drum +6 to +9 dB louder on
  average; peaks between −2.9 and +4.2 dB (hats rise most, as they run quieter inside). Grooves peak
  −4.0 to −6.1 dBFS; everything at maximum including Drive reaches −0.11 dBFS.
- CPU: Drive adds one short sum per sample per ringing drum, within measurement noise (worst case
  still about 0.5–0.6% of an M3 core, roughly 3–5% of an MPC One core).

### v1.2.1: snare rework (2026-11-17)

On the MPC Simon found the snare "doesn't sound very snare-y". Measured: at Morph 50% the pitched
body was 89% of the energy and the noise only 11%, so it was a ping with a little hiss (at 0% it
was 100% body, a tom). Changes:

- The body has its own shorter decay (Decay maps 40–250 ms) and a faster pitch drop (30 ms), so
  it's a thump rather than a ring. The wires keep the longer 80–500 ms range.
- The wires are band-passed (high-pass 600–2500 Hz, new low-pass 5–14 kHz, both moving with Morph)
  and much louder, with a 12 ms burst at the start for the crack.
- Body share of the energy, Morph 0 / 50 / 100%: 74% / 29% / 4% (was 100% / 89% / 40%).
  Brightness 3953 / 7153 / 9660 Hz (was 709 / 3792 / 8629).
- The whole snare is scaled ×0.55 so its peak (−11.2 dBFS at Init) sits level with the kick
  (−11.1). Grooves still peak −4.0 to −6.1 dBFS; harness ALL PASS, host_sim 19/19.
- Kits unchanged; their snares simply sound more like snares. Version 1.2.1 (1201).
