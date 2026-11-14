# OCKM FM Kit

Eight one-shot FM drums for the MPC, made for house and techno. Instrument, 0 in / 4 out
(Main + Aux), MIDI in. Part of the all-OCKM setup: OCKM FM Kit for drums, OCKM-03 for bass,
OCKM Wave for chords and pads, OCKM Gate for pumping.

## The kit

| Note | Drum | 5th knob | How it's made |
|---|---|---|---|
| 36 | Kick | Punch | Low sine with a fast pitch drop; a modulator at the same pitch adds the knock |
| 37 | Snare | Snap | Sine with an off-pitch modulator (shell ring) plus high-passed noise (wires) |
| 38 | Clap | Spread | A sine whose pitch is shaken by noise, making a band of noise; three quick claps then a tail |
| 39 | Closed Hat | Metal | Three sines in a chain at odd ratios, high-passed. Chokes the open hat |
| 40 | Open Hat | Metal | Same, longer |
| 41 | Low Tom | Bend | Like the kick, higher, gentler bend |
| 42 | High Tom | Bend | Same, higher |
| 43 | Perc | Ratio | Sine with an odd-ratio modulator: rim and woodblock, bell, then clang |

Other notes wrap onto the same eight (44 = Kick, 35 = Perc), so every pad bank and any keyboard
plays the kit. Notes choose the drum; they don't change its pitch. Note-offs are ignored.

**FM in one line:** one sine (the modulator) wobbles another (the carrier). How much it wobbles
is the **Tone** knob (brightness: pure, knocky, buzzy, noisy). The speed ratio between the two
decides pitched (whole numbers) or metallic (odd numbers).

## Knobs

Each drum: **Tune** (±24 semitones), **Decay** (length, scaled to suit the drum), **Tone** (FM
amount), **Level**, and its 5th knob from the table. Velocity sets loudness (soft hits about
−10 dB) and adds a little brightness.

Global: **Master** (−40…+6 dB, parameter 0) and **Routing**.

| Page | Q-Link columns |
|---|---|
| KIT 1 | Kick, Snare, Clap, Closed Hat (Tune / Decay / Tone / Level, top to bottom) |
| KIT 2 | Open Hat, Low Tom, High Tom, Perc |
| SHAPE | 5th knobs for drums 1–4, 5th knobs for drums 5–8, badge, Routing + Master |

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
  All eight ringing at once measured 0.34% of one Apple M3 core, so roughly 2–3.5% on an MPC One.
- Retriggers and the hat choke fade the old sound over 1.5 ms, so there are no clicks.

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
