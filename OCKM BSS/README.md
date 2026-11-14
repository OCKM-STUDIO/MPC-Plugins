# OCKM BSS

A warm, round, soft bass for the MPC, made for house and techno. Instrument, 0 in / 2 out, MIDI
in, one note at a time. Part of the all-OCKM setup: OCKM FM Kit for drums, OCKM BSS for soft
bass, OCKM-03 for acid, OCKM Wave for chords and pads, OCKM Gate for pumping.

## The idea: two knobs

**Cutoff** sets how bright, **Decay** sets how long. Together they take the sound from almost a
kick drum (both down) to a full-on growl (both up):

| | Decay short | Decay mid | Decay long |
|---|---|---|---|
| **Cutoff low** | kick-like thud | round, deep sub | dark, smooth held sub |
| **Cutoff mid** | short, punchy pluck | warm rolling house bass | soft "wow" that slowly closes |
| **Cutoff high** | bright, snarling stab | buzzy driving bass | **full-on growl** |

Each knob has an extra job at the end of its travel, so you don't need the other knobs for the
journey:

- **Decay 0–25% (thump):** the note stops holding, and each note starts with a quick pitch drop
  (up to two octaves, gone in about 40 ms). With Cutoff low, that's a thud tuned to your note.
- **Decay 75–100% (hold):** held notes keep their level, so long notes keep growling.
- **Cutoff 60–100% (growl):** more saw comes in, the filter is pushed into its own warm
  saturation, and the low-mids (around 220 Hz) are lifted. The top of Cutoff gets fuller, not
  fizzier.

In between, every knob works exactly as set (the harness proves it bit for bit).

## The sound

- **Oscillators:** a rounded sine (the body), a **Sub** sine one octave down, and a **Saw** for
  the filter to open into. **Detune** pulls the saw up to 20 cents sharp for slow movement.
- **Filter:** a 24 dB/octave ladder low-pass, from 40 Hz (just the note: muffled) to 4 kHz
  (open and full), with the same musical step at every point of the knob. With the envelope on
  top, it eases off above 2.5 kHz and never opens past 6 kHz, so it never gets fizzy. It follows the note
  at half an octave per octave. **Resonance** goes up to a singing peak and stays stable at 100%,
  and the bass stays full as it rises. **Env Amount** sets how far the filter envelope opens it
  (up to 6 octaves); **Decay** sets how fast it closes.
- **Velocity:** with **Vel>Filter** up, softer notes are darker (velocity 127 sounds exactly as the
  knobs are set). Softer notes are also a little quieter (6 dB at most, fixed).
- **Amp:** **Attack**, the Decay knob, **Sustain**, **Release**.
- **Glide:** at 0 every note re-plucks, even if notes overlap. Above 0, a note that overlaps the
  previous one in the sequence slides to its pitch without re-plucking. Lengthen a note into the
  next one on the MPC to make it slide.
- **Drive:** a gentle, level-matched soft clip after the filter, with a fixed gentle trim at
  5 kHz after it so it adds grit, not fizz.
- **Echo:** synced to the MPC's tempo (1/16, 1/8T, 1/8, dotted 1/8, 1/4), ping-pong left and
  right. What goes into the echo loses everything below 250 Hz (24 dB/octave) and each repeat
  gets a little darker and lighter on lows, so the bass stays one tight, centred note and never
  smears into the kick. Echo Mix 0 (default) switches it off completely.

## Pages and Q-Links

| BASS | | | |
|---|---|---|---|
| **FILTER** | **OSC** | **AMP** | **ECHO** |
| Cutoff | Sub | Attack | Drive |
| Decay | Saw | Sustain | Echo Time |
| Resonance | Detune | Release | Feedback |
| Env Amount | Glide | Volume | Echo Mix |

Each column is one Q-Link bank; Cutoff and Decay are the top two of the first. Program-mode
Q-Links use the same layout. Page **SETUP** holds Vel>Filter and the name badge.

## Presets

Init Soft Bass, Kick Thud, Deep Round Sub, Warm House Pluck (light dotted-1/8 echo), Rubber
Glide, Buzzy Techno, Muffled Dub (dub-techno echo), Full Growl, Open Sweep (start here and ride
Cutoff and Decay up to the growl). Over the demo bassline they peak between −4.3 and −7.8 dBFS.

`build/demo.wav` plays 4 bars of each preset, then Open Sweep ridden from kick to growl and back
over 16 bars, then Warm House Pluck with the echo turned up.

## Honest notes

- **CPU:** one voice, always: about 0.1% of one desktop core with the growl and the echo on
  (harness and host_sim). The MPC One is roughly 10–40× slower, so expect about 1–4% of one core.
  **Not yet measured on the MPC One.**
- **The output never clips:** a soft ceiling above −3 dBFS keeps even everything at maximum with
  Volume +6 dB under 0 dBFS. The presets peak around −5 to −8 dBFS and don't touch it.
- **v1.1.0: full, not fizzy.** In v1.0.0, opening Cutoff and Decay got very fizzy. Now, at
  Cutoff 100 / Decay 90, there's about 12 dB less above 2 kHz and the low-mids sit 2 dB closer
  to the sub, at the same overall level. The top quarter of Cutoff is still the gentlest step
  (7 dB of change against 13–26 dB for the others): it thickens rather than brightens.
- Cutoff moves are smoothed over 15 ms and the filter is updated every 8 samples with per-sample
  interpolation: coarse Q-Link jumps are as clean as tiny steps (harness-checked).
- Echo memory: 1.5 s per side (about 0.6 MB), set aside when the plugin loads.

## Not in v1

Polyphony or unison; stereo width for the bass; octave/tune knobs (use the MPC's transpose);
other waveforms, a shape knob or noise; LFOs, mod matrix, arpeggiator or sequencer; other filter
types; separate filter and amp decay knobs; knobs for the zone amounts or a pitch envelope; a
velocity-to-amp knob; accent or other acid behaviour (that's OCKM-03); effects beyond the drive
and the echo; echo extras (free time, filter knobs, mono switch, modulation); pitch bend and mod
wheel; 2× oversampling; more than 9 presets.

## Build, test, skin

From the repo root (`SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}`):

```bash
python3 $SDK/tools/build.py "OCKM BSS"
```

```bash
$SDK/.sdk-tools/host_sim "OCKM BSS"/build/native/*.dylib test --wav "OCKM BSS/build/test.wav"
```

```bash
clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM BSS/src" "OCKM BSS/tests/harness.cpp" -o "OCKM BSS/build/harness" && "OCKM BSS/build/harness" "OCKM BSS/build/demo.wav"
```

After any `ui.json` change, redraw the olive skin and build again:

```bash
python3 "OCKM BSS/make_skin.py" && python3 $SDK/tools/build.py "OCKM BSS"
```

## Licence

MIT, Copyright (c) OCKM. Built with inMusic's MIT-licensed MPC Plugin SDK (its `Biquad`,
`DelayLine` and one-pole helpers are used as-is). The zero-delay-feedback ladder and PolyBLEP saw
are standard published techniques, written fresh here. No other code or data borrowed.
