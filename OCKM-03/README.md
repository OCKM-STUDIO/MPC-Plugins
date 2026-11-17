# OCKM-03

A lean acid bass synth for Akai MPC standalone hardware, by OCKM. It does one job: squelchy, slidey acid basslines played from the MPC's own sequencer.

One voice, one oscillator, one resonant filter, slide, accent and drive, on one classic-style panel, plus **Mutate** (repeatable octave jumps, slide-ins and rests that bring a programmed pattern to life), a **Pattern** generator that writes acid lines for you, and a one-knob **Echo**. There are no LFOs, effects (beyond the one Echo knob), arpeggiator or extra oscillators, on purpose. It works with the MPC's own arpeggiator.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta) → turn on **Enable Enhancements** and **External Plugin Support**.
- Install: copy the `OCKM - VST - OCKM-03` folder into a folder called `VST2 Plugins` at the root of a USB stick or SD card, insert it and restart the MPC. Load it on a track as a plugin instrument (External Plugins → OCKM-03).

## Playing it

OCKM-03 plays one note at a time; the newest note wins.

- **Slide:** make two notes overlap in the MPC's sequencer (lengthen the first note so it runs past the start of the next one). The pitch glides to the new note and the filter does not re-pluck. Notes that don't overlap start fresh.
- **Accent:** any note with velocity at or above **Accent Thr** (100 by default) is accented. It hits harder at the start (the extra volume dies away with the note's envelope) and opens the filter. **Resonance changes what accents do:** at low Resonance an accent is a quick bright blip; at high Resonance it swells a moment later (the classic "wow"), and accents close together stack up into a rising sweep. Spaced-out accents don't stack. Velocity has no other effect.
- **With the MPC arp:** an arp usually plays every note at the same velocity. If that is 100 or more, every note is an accent: punchier and snappier, but all the knobs still work. If you want accents only on some notes, set the arp's velocity below 100 (or raise Accent Thr), and play the notes you want accented harder.
- Pitch bend and the mod wheel are ignored.

## The panel

Two pages: **ACID** (the sound, below) and **PATTERN** (the generator: Pattern, Pattern #, Length and Scale in one column, Density, Pat Accent, Pat Slide and Range in the next). Each framed column is one Q-Link bank:

| Filter | Voice | Tone | Mutate |
|---|---|---|---|
| Cutoff | Accent | Wave (Saw / Square) | Oct Jump |
| Resonance | Accent Thr | Tuning (±12 st) | Add Slide |
| Env Mod | Slide Time | Drive | Add Rest |
| Decay | Echo | Volume | Variation |

A third page, **GRIT**, holds Overdrive, Muffler and Full Bass (see Version 1.6).

In Program mode, Q-Links 1–11 play the Filter, Voice and Tone controls in parameter order, Q-Links 12–15 play Mutate, and Q-Link 16 plays Echo.

## Pattern (generator)

Pattern lives on the second page, **PATTERN**.

1. Set **Pattern** to **HOLD**.
2. Hold a note: a pad, or one long note on the MPC grid. OCKM-03 plays its own 16th-note acid line in that key for as long as the note is held, and stops when you let go.
3. Hold a different note to transpose the line. One long note per bar on the grid writes the key changes, and that's how you place the line in your song.

The controls:
- **Pattern #** (1–999) picks the line. Turning it is "Generate". The number is saved with your project, so a line you like always comes back.
- **Density** sets how many steps play.
- **Pat Accent** sets how many steps are accented.
- **Pat Slide** sets how many steps glide into the next.
- **Range** (1–3 octaves) sets how far the line climbs.
- **Length** (1–32 steps) sets when it repeats.
- **Scale** picks Minor, Phrygian, Dorian, Minor Pentatonic or Harmonic Minor.

How it behaves:
- **Reshaping is live and reversible.** Moving a knob reshapes the current line, and returning it to a value gives exactly the same line back. The amounts are exact: Density 50% plays 8 of 16 steps.
- **It follows the MPC.** While the sequence plays, the line sits on the song's 16ths, so it stays locked when you loop or move the playhead. With the sequencer stopped, it starts the moment you press the pad, at the MPC's tempo.
- **Mutate works on the line** exactly as on notes you play: octave jumps, slide-ins and rests on the same steps of every bar.
- **The sound comes from the ACID page.** Accents still use Accent Thr: Pattern sends accented steps at velocity 127 and others at 80.
- **Swing:** the MPC's swing can't reach notes OCKM-03 makes itself, so the line plays straight.
- **Off by default:** with Pattern on OFF, OCKM-03 plays your notes exactly as before.

## Echo

One knob. At 0 the echo is off. Turning it up brings in more echo **and** more repeats together: from a faint single slap to a long rolling trail.

- The repeats are a **dotted 8th** apart and follow the MPC's tempo (120 BPM if the MPC gives none).
- The repeats are **bass-light** (roughly 250 Hz to 3.5 kHz), and each one is a little thinner and darker than the last, so the bass itself stays dry and solid and the echoes sit behind it.
- Turning Echo back to 0 lets the last repeats fade, then switches the echo off completely. Old echoes never come back when you turn it up again.
- It's mono, like the rest of OCKM-03. For a wide stereo echo, use one of the MPC's delay effects on the track instead.

## Mutate

Mutate changes what OCKM-03 **plays**, not the notes on your grid. Each note can jump up an octave, slide in from the previous note, or be left out, depending on **where it falls in the bar**:

- **Oct Jump:** how many steps jump up an octave.
- **Add Slide:** how many steps glide in from the previous note, without the filter re-plucking, even if the notes don't overlap.
- **Add Rest:** how many steps are left silent.
- **Variation (1–64):** which steps. Each Variation is a different, fixed choice.

How it behaves:

- **It repeats.** The same steps change on every pass of the bar, so it sounds like a pattern, not randomness. Change Variation for a different version.
- **The amounts are exact and build up.** 25% means 4 of the 16 steps, 50% means 8. Turning a knob up only adds steps, and turning it down takes them away in reverse order, so it works as a live control.
- **At 0 it does nothing:** you hear exactly what you programmed.
- **You can record it.** All four are ordinary Q-Link parameters, so you can record knob moves into the MPC sequence (for example, Oct Jump up for the last bar of a phrase).
- It follows the MPC's playhead while the sequence plays. When you play pads by hand with the sequencer stopped, it counts notes instead.
- It can't put notes on the grid: plugins can't send MIDI back to the MPC.

## Factory presets

1. **Init Acid:** neutral starting point.
2. **Classic Squelch:** saw, mid cutoff, high resonance, plenty of Env Mod.
3. **Rubber Sub:** square, low and round, long slide.
4. **Acid Scream:** screaming resonance, short decay, hard accents and some drive.
5. **Mutant Squelch:** like Classic Squelch, with Mutate on (Oct Jump 25%, Add Slide 30%, Add Rest 20%, Variation 7) and Echo at 30%. Feed it a plain run of 16ths and it plays a riff.

6. **Acid Machine:** the Classic Squelch sound with Pattern on (Pattern # 7). Hold a note.
7. **Mutant Machine:** Mutant Squelch's sound with Pattern on (Pattern # 42, Range 2), a little Mutate and Echo. Hold a note.
8. **Grit Squelch:** a squelch with Overdrive, a little Muffler and Full Bass on.

Presets 1–4 open with Mutate and Echo at 0, presets 1–5 and 8 with Pattern off, and every preset except Grit Squelch with the GRIT page at 0.

## Version 1.6: Grit

- **Grittier filter:** every stage of the filter now saturates when pushed, as on the original circuit, not just its input. I also fixed an error in 1.4's filter: the input stage was modelled slightly wrong, which made it duller and over-damped. The fixed filter now matches the circuit study's own resonance figures. All presets are re-levelled to 1.5's loudness.
- **Soft attack:** normal notes start over 3 ms and accents over 0.5 ms, as on the original, so accents punch through.
- **GRIT page:**
  - **Overdrive** drives the oscillator into the filter (up to 66× at 100%, after a well-known mod for the original). Low settings thicken the sound; high settings square the wave up inside the filter and make it snarl. The volume stays roughly level.
  - **Muffler** soft-clips the output, so note starts and accents get rounder and fuzzier.
  - **Full Bass** brings back the deep bass the original circuit thins out (32 Hz is down about 1 dB instead of 5). Off is the authentic thin low end.
- **New preset: Grit Squelch.**

## Version 1.5

- **Pattern generator** (see above), on a new PATTERN page, with two new presets. The idea comes from the GPL-3 [mpc-vst-acid](https://github.com/sd88me/mpc-vst-acid) plugin; this is our own design, written from its description, not its code.

## Version 1.4: closer to the classic circuit

- **New filter:** a diode ladder (the type the classic acid unit uses) instead of the 4-pole ladder. It's rounder and more rubbery, the deep bass is thinner (as in the original), and at high Resonance it rings hard without whistling on its own. Nearly closed, its resonance is gentler.
- **Accent "wow" tied to Resonance:** see *Playing it*. This replaces the 1.1 accent build-up, and it still can't pin the filter open under an arp.
- **Accent punch:** an accented note's extra volume dies away with the envelope, so it's a hit, not a louder note.
- **Env Mod works both ways:** more Env Mod also lowers the filter's resting point, so notes pluck more instead of just getting brighter.
- **Presets:** Cutoff raised by 9 on presets 2–5 to make up for the new filter and Env Mod; Rubber Sub's Volume is now −7 dB. Your own projects keep their settings, but will sound a little different (generally darker, so you may want to raise Cutoff).

## Version 1.3

- **Echo:** one knob, dotted-8th, tempo-synced, with bass-light repeats. It takes the empty Voice slot (the OCKM-03 badge is gone from the panel) and Q-Link 16. Mutant Squelch now has a little echo.

## Version 1.2

- **Mutate** (see above): Oct Jump, Add Slide, Add Rest and Variation, on the fourth Q-Link column.
- **Key tracking:** the filter follows half of the pitch, measured from C1, so higher and octave-jumped notes stay bright instead of getting choked. Notes at C1 sound as before.
- **Low cut:** a fixed 30 Hz filter trims sub rumble, leaving room for your kick. Rubber Sub and Acid Scream are 3 dB quieter to keep the same headroom.
- New preset **Mutant Squelch**.

## Version 1.1 (after the first hardware test)

- **Back to the classic controls.** The four macros (Squelch, Bite, Rubber, Grit) and their recipe page are gone from the screen and the Q-Links, and they no longer affect the sound. Their 15 parameters still exist inside the plugin, because removing them would scramble the settings of projects saved with 1.0. They do nothing.
- **Works with an arp.** With every note accented, Decay used to do nothing and the filter stayed pinned open, so the knobs seemed dead. Accented notes now still follow Decay (they are just shorter), the accent build-up drains with Decay and is capped, and Env Mod sweeps 5.5 octaves instead of 7, so Cutoff and Env Mod keep working.
- **Drive** now changes across its whole travel (before, it was fully on by 25%).
- **Wave switch:** SAW and SQUARE are printed on the panel under the two buttons, and the buttons' own captions are set to always show.

## Under the hood

- Oscillator: band-limited (PolyBLEP) saw, or a slightly narrow square for a hollow, reedy tone.
- Filter (v1.4, corrected and made grittier in 1.6): a diode-ladder model. Four coupled stages, where each stage loads its neighbours and the bottom capacitor is smaller, with a saturating (tanh) input pair. Two coupling high-passes (78 Hz and 13 Hz) sit inside the resonance loop, as in the original circuit. All six states are solved together each sample with no delay in the loop (zero-delay feedback), at 2× the sample rate. The displayed Cutoff (20 Hz–8 kHz) is where resonance peaks. At maximum, Resonance is 90% of the way to self-oscillation. Since 1.6, the input pair feeds a current into the bottom stage (1.4 got this wrong), and every rung saturates (tanh), with one correction step after the linear solve. Overdrive multiplies the oscillator level into the input pair by 1–66.6.
- Key tracking: the cutoff moves 0.5 octave per octave played, measured from C1 (MIDI 36), following the slide.
- Filter envelope: instant attack, exponential decay (50 ms to 2.5 s). Accented notes decay shorter but still follow Decay (shorter of Decay and 0.3 × Decay + 120 ms: 50 ms to about 0.9 s). Env Mod sweeps up to 5.5 octaves: 30% of it below the Cutoff knob (where the envelope rests) and 70% above.
- Amp: a 3 ms attack (0.5 ms for accents), a long fixed decay while the note is held and an 8 ms release; no amp envelope knobs. Muffler: a soft clip after the amp, with its threshold falling from 0.5 to 0.1 and up to 2.2× make-up gain.
- Accent (v1.4): an accent voltage (Accent × envelope) drives both the filter and the volume.
  - **Filter:** the voltage also charges a capacitor that can only charge up. It charges over 32–50 ms, slower at higher Resonance, and drains over about 100 ms. Resonance blends the filter push from the direct voltage (a blip) to the capacitor (a late swell that builds over a run of accents). The push is up to 3 octaves.
  - **Volume:** the extra level is up to +8 dB, follows the voltage above a small threshold, and dies away with it.
  - **Arps:** the capacitor can never exceed the direct voltage, so a steady arp of accents can't pin the filter open.
- Drive: tanh soft clip after the amp. The gain into the clipper grows with the square of the knob (up to 12×) and the effect is fully mixed in by 50%, so the whole travel is usable; level compensated on peaks (it gets denser and up to about 4 dB louder on average); 0 is clean.
- Low cut: 30 Hz, 12 dB/octave high-pass after the drive.
- Pattern: for each step and lane (play, accent, slide, pitch, octave), a fixed number comes from an integer hash of the Pattern #, the step and the lane. The steps are ranked within the line's Length, and the knobs set how far down that order goes. Pitches come from the scale, weighted toward the root (about 40%), then the 5th, the minor 3rd and the 7th. Plain notes last half a step; slide notes tie into the next. Nothing runs per sample.
- Mutate: decisions depend only on the 16th-step in the bar (from the MPC's playhead), the Variation and the mutation type. Each Variation shuffles the 16 steps into a fixed order per mutation, and an amount takes the first N steps of that order. Nothing runs per sample.
- Echo: a dotted-8th delay (up to 1.2 s, allocated when the plugin starts). Wet level 0–45% and feedback 15–65% rise together with the knob. Everything entering the delay passes a 250 Hz high-pass and a 3.5 kHz low-pass, and the time glides over 50 ms when the tempo changes. It's bypassed at 0 once the tail has died away, and adds about 2% to the plugin's CPU use.
- Output ceiling: untouched below −3 dBFS, then rounds off smoothly, so the hardest settings never clip the MPC mixer.

## CPU

One instance uses about 0.2% of one core of a desktop Mac in the SDK's host simulator (unchanged in 1.1 and 1.2; Echo adds about 2% of that). The 1.4 diode-ladder filter measured the same on the desktop (0.36% of real time playing 16ths, against 0.37% for 1.3), but it does more arithmetic per sample, so check the MPC's meter. 1.6's saturating filter adds about 30% more on the desktop (0.42% of real time playing 16ths). The MPC is roughly 10–40× slower, so expect about 2–7% of one core on an MPC One. **Not yet measured on the MPC One:** CPU meter readings with no instance, one instance and several instances are still to be filled in here.

## Not in v1

Step editing, swing and two-line blending for Pattern, sending Pattern's line out as MIDI, extra oscillators, LFOs, effects beyond the one Echo knob, a mod matrix, an arpeggiator (use the MPC's), more echo controls or stereo echo (use the MPC's delay effects), macros (tried in 1.0, retired in 1.1), octave jumps down, Mutate patterns longer than one bar, polyphony, velocity beyond accent, pitch bend and mod wheel, and amp envelope controls. The code keeps the note handling (`NoteInput`, `Pattern`) separate from the sound (`AcidVoice`).

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

The screen artwork is drawn by `make_skin.py` (Python + numpy, seeded so it is repeatable): worn brushed-aluminium panels with printed section frames and knob scales, black knurled knobs with spun-metal caps, and worn plastic Wave buttons with SAW / SQUARE printed underneath. Since v1.3 every slot holds a control, so there's no name badge. It writes the images to `assets/` and the skin to `skins/`. Because `skins/` exists, `build.py` copies it instead of regenerating it from `ui.json`, so **after changing `ui.json`, re-run the script**:

```
python3 $SDK/tools/build.py OCKM-03 --target native
python3 OCKM-03/make_skin.py
python3 $SDK/tools/build.py OCKM-03
```

## Licence and credits

MIT. Copyright (c) OCKM. Built with the MIT-licensed MPC Plugin SDK; all DSP was written for this plugin using the SDK's own helpers (PolyBLEP oscillator, fastTanh). No third-party DSP code is used. The 1.5 Pattern generator is our own code, inspired by the behaviour described in the README of the GPL-3 [mpc-vst-acid](https://github.com/sd88me/mpc-vst-acid) plugin; none of its code was read or copied. The 1.4 filter topology, coupling corners and accent behaviour follow the published circuit notes of the GPL-3 [DW-303](https://github.com/mo0kid/DW-303) project (`docs/circuit/`). Only the circuit behaviour described there was used; no code was copied. The 1.6 changes (stage saturation, Overdrive, Muffler, Full Bass, soft attack) follow the behaviour described in the README of the AGPL-3 [mpc-vst-acid303](https://github.com/sd88me/mpc-vst-acid303) project and the public manual of the classic after-market mods for the original unit; no code was read or copied.
