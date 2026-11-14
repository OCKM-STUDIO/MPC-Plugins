# OCKM FM Kit

Eight one-shot FM drums for the MPC, made for house and techno. Instrument, 0 in / 4 out
(Main + Aux), MIDI in. Part of the all-OCKM setup: OCKM FM Kit for drums, OCKM-03 for bass,
OCKM Wave for chords and pads, OCKM Gate for pumping.

## The kit

| Note | Drum | How it's made | Morph 0% → 50% → 100% |
|---|---|---|---|
| 36 | Kick | Low sine with a fast pitch drop; a modulator adds the knock | round sub → punchy kick → hard FM boom with a zap |
| 37 | Snare | Sine with an off-pitch modulator (shell ring) plus high-passed noise (wires) | tuned body → classic snare → noisy metallic crack |
| 38 | Clap | A sine whose pitch is shaken by noise, making a band of noise | one tight snap → three claps → wide, lower five-clap |
| 39 | Closed Hat | Three sines in a chain at odd ratios, high-passed. Chokes the open hat | glassy ping → classic metal → trashy wash |
| 40 | Open Hat | Same, longer | same |
| 41 | Low Tom | Like the kick, higher, gentler bend | soft tom → synth tom with a big bend → metallic conga |
| 42 | High Tom | Same, higher | same |
| 43 | Perc | Sine with an odd-ratio modulator | rim click → woodblock / cowbell → long bell |

Other notes wrap onto the same eight (44 = Kick, 35 = Perc), so every pad bank and any keyboard
plays the kit. Notes choose the drum; they don't change its pitch. Note-offs are ignored.

**FM in one line:** one sine (the modulator) wobbles another (the carrier). How much it wobbles
is the **Tone** knob (brightness: pure, knocky, buzzy, noisy). The speed ratio between the two
decides pitched (whole numbers) or metallic (odd numbers).

## Knobs

Each drum: **Tune** (±24 semitones), **Decay** (length, scaled to suit the drum), **Tone** (FM
amount), **Level** and **Morph** (0–100%, default 50%: blends the drum through the three characters in
the table, so it changes what kind of drum it is). Velocity sets loudness (soft hits about
−10 dB) and adds a little brightness.

Global: **Master** (−40…+6 dB, parameter 0) and **Routing**, plus three "feel" knobs (all off at 0%):

- **Variation**: every hit slightly different (pitch, brightness, length, level, Morph), so fast
  hats and repeated percs stop sounding machine-gunned. Around 20–30% sounds played.
- **Vel>Morph**: velocity moves Morph too. Velocity 95 leaves it alone, harder hits push towards
  100%, softer ones pull back (up to 40% each way). Hard snares crack, ghost notes stay soft.
- **Drive**: warm-to-gritty saturation on each drum, the kick most and the hats least. It lifts the
  body and tail and rounds the peaks. It applies from the next hit.

| Page | Q-Link columns |
|---|---|
| KIT 1 | Kick, Snare, Clap, Closed Hat (Tune / Decay / Tone / Level, top to bottom) |
| KIT 2 | Open Hat, Low Tom, High Tom, Perc |
| SHAPE | Morph for drums 1–4, Morph for drums 5–8, badge + Variation / Vel>Morph / Drive, Routing + Master |

## Multi-out

Outputs 1-2 are Main and 3-4 are Aux; route them to separate mixer channels on the MPC.

- **All Main**: everything on 1-2.
- **Kick>Aux**: the kick on its own on 3-4, to compress or sidechain it.
- **Hats>Aux**: both hats on 3-4.
- **Split**: kick, snare and clap on 1-2; hats, toms and perc on 3-4.

## Presets

Init Kit, Deep House Kit, Warehouse Techno, Minimal Clicks, Industrial Clang, Rumble Techno,
Lo-Fi House, Acid Partner Kit. `build/demo.wav` plays the same 4-bar groove through each in turn.

## Honest notes

- The output never clips: a soft limiter above −2.5 dBFS keeps even all eight drums at full
  level under 0 dBFS. Normal grooves peak around −5 dBFS and never touch it.
- CPU: a ringing drum costs 2–3 sine-table reads per sample, and a finished drum costs nothing.
  All eight ringing at once measured about 0.5% of one Apple M3 core, so roughly 3–5% of an MPC One
  core in that worst case; normal grooves stay around 1%.
- Retriggers and the hat choke fade the old sound over 1.5 ms, so there are no clicks.

## Changes

- **1.2.0:** new **Variation**, **Vel>Morph** and **Drive** knobs (SHAPE page, FEEL column). All
  default to 0%, so 1.1 projects sound exactly the same. Kits use them (Init leaves them off).
- **1.1.0:** the fifth knob on every drum became **Morph**. Projects saved with 1.0 load fine, but
  their drums sound different at the same knob position. Kits retuned around Morph.

## Not in v1

Per-operator editing (ratios, envelopes, levels), algorithms and feedback; more than eight drums
or layering; user choke groups; pitch from the note played; per-drum pan, filter, drive or sends;
velocity curves; a built-in sequencer or effects; samples; a second Aux pair.

## Build, test, skin

From the repo root (`SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}`):

```bash
python3 $SDK/tools/build.py "OCKM FM Kit"
```

```bash
$SDK/.sdk-tools/host_sim "OCKM FM Kit"/build/native/*.dylib test --wav "OCKM FM Kit/build/test.wav"
```

```bash
clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM FM Kit/src" "OCKM FM Kit/tests/harness.cpp" -o "OCKM FM Kit/build/harness" && "OCKM FM Kit/build/harness" "OCKM FM Kit/build/demo.wav"
```

After any `ui.json` change, redraw the bronze skin and build again:

```bash
python3 "OCKM FM Kit/make_skin.py" && python3 $SDK/tools/build.py "OCKM FM Kit"
```

## Licence

MIT, Copyright (c) OCKM. Built with inMusic's MIT-licensed MPC Plugin SDK; the note map, wrapping
and multi-out idea follow the SDK's MIT `DrumSynth` example (ideas only). No other code or data
borrowed.
