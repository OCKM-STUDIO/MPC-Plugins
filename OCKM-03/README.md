# OCKM-03

A lean acid bass synth for Akai MPC standalone hardware, by OCKM. It does one job: squelchy, slidey acid basslines played from the MPC's own sequencer.

One voice, one oscillator, one resonant filter, slide, accent and drive, on one classic-style panel. There are no LFOs, effects, arpeggiator or extra oscillators, on purpose. It works with the MPC's own arpeggiator.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta) → turn on **Enable Enhancements** and **External Plugin Support**.
- Install: copy the `OCKM - VST - OCKM-03` folder into a folder called `VST2 Plugins` at the root of a USB stick or SD card, insert it and restart the MPC. Load it on a track as a plugin instrument (External Plugins → OCKM-03).

## Playing it

OCKM-03 plays one note at a time; the newest note wins.

- **Slide:** make two notes overlap in the MPC's sequencer (lengthen the first note so it runs past the start of the next one). The pitch glides to the new note and the filter does not re-pluck. Notes that don't overlap start fresh.
- **Accent:** any note with velocity at or above **Accent Thr** (100 by default) is accented. It is louder and brighter, with an extra filter "wow". Accents close together stack up into a rising sweep; spaced-out accents don't. Velocity has no other effect.
- **With the MPC arp:** an arp usually plays every note at the same velocity. If that is 100 or more, every note is an accent: punchier and snappier, but all the knobs still work. If you want accents only on some notes, set the arp's velocity below 100 (or raise Accent Thr), and play the notes you want accented harder.
- Pitch bend and the mod wheel are ignored.

## The panel

One page. The Q-Link banks are the three framed columns:

| Filter | Voice | Tone |
|---|---|---|
| Cutoff | Accent | Wave (Saw / Square) |
| Resonance | Accent Thr | Tuning (±12 st) |
| Env Mod | Slide Time | Drive |
| Decay | | Volume |

In Program mode, Q-Links 1–11 play these 11 controls in parameter order and Q-Links 12–16 are unassigned.

## Factory presets

1. **Init Acid:** neutral starting point.
2. **Classic Squelch:** saw, mid cutoff, high resonance, plenty of Env Mod.
3. **Rubber Sub:** square, low and round, long slide.
4. **Acid Scream:** screaming resonance, short decay, hard accents and some drive.

## Version 1.1 (after the first hardware test)

- **Back to the classic controls.** The four macros (Squelch, Bite, Rubber, Grit) and their recipe page are gone from the screen and the Q-Links, and they no longer affect the sound. Their 15 parameters still exist inside the plugin, because removing them would scramble the settings of projects saved with 1.0. They do nothing.
- **Works with an arp.** With every note accented, Decay used to do nothing and the filter stayed pinned open, so the knobs seemed dead. Accented notes now still follow Decay (they are just shorter), the accent build-up drains with Decay and is capped, and Env Mod sweeps 5.5 octaves instead of 7, so Cutoff and Env Mod keep working.
- **Drive** now changes across its whole travel (before, it was fully on by 25%).
- **Wave switch:** SAW and SQUARE are printed on the panel under the two buttons, and the buttons' own captions are set to always show.

## Under the hood

- Oscillator: band-limited (PolyBLEP) saw, or a slightly narrow square for a hollow, reedy tone.
- Filter: 4-pole low-pass ladder in zero-delay-feedback form, with a saturating input stage, run at 2× the sample rate. Cutoff runs from 20 Hz (with Env Mod and Accent at 0, nearly silent) to 8 kHz. Resonance goes right up to the edge of self-oscillation, where it sings and the bass thins out, as acid should. It is tuned for the classic acid character; it is not a circuit-accurate diode-ladder model.
- Filter envelope: instant attack, exponential decay (50 ms to 2.5 s). Accented notes decay shorter but still follow Decay (shorter of Decay and 0.3 × Decay + 120 ms: 50 ms to about 0.9 s). Env Mod sweeps up to 5.5 octaves.
- Amp: a long fixed decay while the note is held and an 8 ms release; no amp envelope knobs.
- Accent: up to +8 dB louder and up to 1.2 octaves of extra filter push. Build-up: each accented note charges an accumulator (capped at 1.6 accents' worth) that drains over about a quarter of the Decay time plus 60 ms; its smoothed level adds up to about 1.6 more octaves. A quick run of accents climbs; a steady arp of accents settles at a level set by Decay and Accent instead of pinning the filter open.
- Drive: tanh soft clip after the amp. The gain into the clipper grows with the square of the knob (up to 12×) and the effect is fully mixed in by 50%, so the whole travel is usable; level compensated on peaks (it gets denser and up to about 4 dB louder on average); 0 is clean.
- Output ceiling: untouched below −3 dBFS, then rounds off smoothly, so the hardest settings never clip the MPC mixer.

## CPU

One instance uses about 0.2% of one core of a desktop Mac in the SDK's host simulator (unchanged in 1.1). The MPC is roughly 10–40× slower, so expect about 2–7% of one core on an MPC One. **Not yet measured on the MPC One:** CPU meter readings with no instance, one instance and several instances are still to be filled in here.

## Not in v1

A built-in step sequencer, extra oscillators, LFOs, effects, a mod matrix, an arpeggiator (use the MPC's), macros (tried in 1.0, retired in 1.1), polyphony, velocity beyond accent, pitch bend and mod wheel, and amp envelope controls. The code keeps the note handling (`NoteInput`) separate from the sound (`AcidVoice`), so a sequencer can be added later without touching the voice.

## Build

Run these from the MPC-Plugins repo root. `SDK` is the MPC Plugin SDK folder:

```
SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}
```

```
python3 $SDK/tools/build.py OCKM-03
$SDK/.sdk-tools/host_sim OCKM-03/build/native/*.dylib test --wav OCKM-03/build/test.wav
clang++ -std=c++17 -O2 -I$SDK/framework -IOCKM-03/src OCKM-03/tests/harness.cpp -o OCKM-03/build/harness && OCKM-03/build/harness OCKM-03/build/demo-pattern.wav
python3 $SDK/tools/package.py OCKM-03
```

### The look (battered silver panel)

The screen artwork is drawn by `make_skin.py` (Python + numpy, seeded so it is repeatable): worn brushed-aluminium panels with printed section frames and knob scales, black knurled knobs with spun-metal caps, and worn plastic Wave buttons with SAW / SQUARE printed underneath. It writes the images to `assets/` and the skin to `skins/`. Because `skins/` exists, `build.py` copies it instead of regenerating it from `ui.json`, so **after changing `ui.json`, re-run the script**:

```
python3 $SDK/tools/build.py OCKM-03 --target native
python3 OCKM-03/make_skin.py
python3 $SDK/tools/build.py OCKM-03
```

## Licence and credits

MIT. Copyright (c) OCKM. Built with the MIT-licensed MPC Plugin SDK; all DSP was written for this plugin using the SDK's own helpers (PolyBLEP oscillator, fastTanh). No third-party DSP code is used.
