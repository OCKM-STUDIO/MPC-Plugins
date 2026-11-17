# OCKM DRIFT

A generative lead for the MPC, made for house and techno. Instrument, 0 in / 2 out (mono,
centred). It plays its own 16-step line on a fat retro analogue mono lead, locked to the MPC's
tempo and grid, and the line slowly changes by itself while staying in key. Part of the all-OCKM
setup: OCKM FM Kit for drums, OCKM BSS for soft bass, OCKM-03 for acid, OCKM Wave for chords and
pads, OCKM Gate for pumping, OCKM DRIFT for the lead.

## How it plays

1. Put OCKM DRIFT on a track and press **Play** on the MPC. It plays a 16-step line in A minor.
   You don't need any notes in the sequence.
2. Every couple of bars a few steps change: a note moves a step up or down the scale, a note
   comes or goes, a slide appears. **Mutate** sets how much changes.
3. Like it? Turn **Lock** on. The line stops changing (the knobs still work), and the locked line
   is saved with your project.
4. Want a different line? Change **Seed**. The same Seed always plays the same line, bar for
   bar, every time you press Play.
5. Write chord roots in the MPC sequence (one note per chord, any octave). The line follows them
   and stays in key.
6. Press **Stop**: it goes quiet, unless **Run Stopped** is on.

## The generator

- **Rhythm:** **Hits** spreads that many notes evenly over the 16 steps (7 = `x..x.x.x..x.x.x.`,
  a rolling house figure; 16 = every step). **Rotate** shifts them later. **Density** at 50%
  (shown as "Hits") plays exactly the hits; lower drops some, higher fills the gaps. **Rate**
  sets the step length: 1/16 (one bar), 1/8 (two bars) or 1/4 (four bars). **Swing** delays
  every second step (the plugin can't read the MPC's own swing, so match it here).
- **Notes:** a random walk through the scale, mostly one scale step at a time, bouncing back at
  the edges of **Range** (1–3 octaves above the root). Step 1 is always the root; notes on the
  beats usually land on chord notes (1st, 3rd, 5th). **Scale** (8 choices), **Root**, and
  **Octave** (0 = the root in the octave starting at C3, so A minor sits around A3 = 440 Hz;
  −2 turns it into a bassline). **Slide** is the chance a note glides into the next one.
- **Evolution:** every **Evolve** bars (1, 2, 4 or 8, always on a bar line), each step changes
  with a chance of **Mutate**, in one small way: its note walks one or two scale steps, or its
  rhythm, accent or slide is re-rolled. Mutate 0% never changes anything; even 100% only moves
  notes in small steps, so it never turns into noise.
- **Rhythm** (v1.1) picks the base groove: **Even** (from Hits, as above), **Offbeat**
  (`..x...x...x...x.`), **Rolling** (`.xxx.xxx.xxx.xxx`), **Gallop**, **Syncopated**, **Two-Step**,
  **Stab** or **Call** (busy first half, empty second half). Rotate, Density, Slide and Mutate work
  on every groove; Hits only on Even.
- **Chord Stick** (v1.1): how close the line sticks to the chord. 0% wanders freely through the
  scale, 50% (default, as v1.0) puts most beats on chord notes, 100% makes every note a chord note
  (an arpeggio of the chord). Step 1 is always the chord's root.
- **Cut Drift:** a slow, smooth wander of the filter (up to ±2 octaves, a new target every 4
  bars), repeatable from the Seed. 0% = off.
- If your MPC sequence loops (say 2 bars), the line still keeps evolving: it counts bars played,
  not bars of the sequence.

## Chords (v1.1)

**Chords** makes the line follow a progression by itself, so you don't need to write chord notes.
The chords are steps of your Scale (1 = the root), so they work in any key and stay in key:
**1-6-3-7** (Am F C G in A minor), **1-4**, **1-7-6-7**, **1-5**, **1-6-4-5**, **1-3-4-6**,
**2-5-1-1**. **Chord Bars** sets 1, 2 or 4 bars per chord.

- The progression starts from its first chord every time you press Play, counting bars played,
  so start from the top of your song to keep it in step with your other parts.
- **Your MPC notes win:** as soon as you play or sequence a note, the line follows your notes until
  the next Play. Sketch with a progression, then write your own chords and they take over.
- With Keys on Transpose the chords move the line by semitones; with Keys Off the progression still
  plays (Keys Off only ignores MPC notes).

## Notes from the MPC

Plugins can't send MIDI, so DRIFT always plays its own sound. Notes you play or sequence on its
track move the line instead. **Only the note name counts, not the octave**, and the line moves
to the nearest place, so it stays in its register.

**Keys:**
- **In Key** (default): the note is a chord root inside the key. The whole line moves by scale
  steps and stays in key. In A minor, write A – F – C – G (one note per bar) and step 1 of each
  bar becomes A, F, C, G, with every other note moving with it, all still in A minor.
- **Transpose:** the line moves by exact semitones (an A minor line becomes a C minor line on C).
- **Off:** notes are ignored.

**Play:**
- **Always** (default): the line plays whenever the MPC plays. The last note stays in force.
- **Held:** the line plays only while a note is held, so the sequence decides where the lead
  comes in and out.

A chord note recorded up to 25 ms late still catches the downbeat. With a whole chord held, the
lowest note counts.

## The sound

- **Oscillators:** two at the note, spread by **Detune** (up to ±12 cents), plus one an octave
  down (**Sub**). **Wave** goes from saw through saw+square to square. Each oscillator drifts
  slowly by a few cents on its own, like old hardware.
- **Filter:** a warm 24 dB/octave ladder low-pass, 60 Hz – 12 kHz, with **Resonance** up to a
  singing peak (stable at 100%), **Env Amount** (up to +5 octaves) and **Decay**. It follows the
  note at half an octave per octave.
- **Drive:** warm, level-matched soft clip. **Length:** how long each note is held (share of a
  step). **Release:** the tail. **Glide:** how long a slide takes. **Accent:** how much accented
  notes (the hits on the beats, plus about one in six others) stand out: up to 4 dB louder with a
  brighter, snappier filter.

## Pages and Q-Links

| SOUND | | | |
|---|---|---|---|
| **FILTER** | **OSC** | **AMP** | **PLAY** |
| Cutoff | Wave | Length | Keys |
| Resonance | Detune | Release | Play |
| Env Amount | Sub | Accent | Run Stopped |
| Decay | Glide | Drive | Volume |

| GEN | | | |
|---|---|---|---|
| **EVOLVE** | **NOTES** | **RHYTHM** | **MOVE** |
| Mutate | Scale | Rhythm | Rate |
| Evolve | Root | Hits | Slide |
| Seed | Range | Rotate | Swing |
| Lock | Octave | Density | Cut Drift |

| HARMONY | | | |
|---|---|---|---|
| **CHORDS** | | | |
| Chords | | | |
| Chord Bars | | | *badge* |
| Chord Stick | | | |

Each column is one Q-Link bank. Cutoff is the top-left knob on SOUND, Mutate the top-left on
GEN. In Program mode the Q-Links follow the SOUND page, except that the PLAY column becomes
**Mutate, Density, Cut Drift, Volume** (performance knobs instead of settings).

## Presets

| Preset | The line |
|---|---|
| Init Drift | a rolling minor lead that changes a few notes every 2 bars |
| Slow Burn | deep-house lead that turns very slowly, filter breathing over minutes |
| Rolling Techno | driving and dark, a little different every bar |
| Peak Arp | every 16th over two octaves, an arpeggio that rewrites itself |
| Sparse Dub | few notes on 1/8 steps, long tails, lots of space |
| Singing Lead | slow, sliding, resonant melody |
| Deep Walker | a walking bassline (two octaves down) |
| Chaos Garden | restless and busy, still in key |
| Frozen Hook | a hook that stays put (Lock on, Seed 42) |
| Chord Rider | follows 1-6-3-7 by itself (2 bars a chord), sticking close to the chords |
| Offbeat Stabs | short stabs between the kicks, moving through 1-4 |
| Rolling Bass | a rolling techno bassline on 1-5 (4 bars a chord) |

Over 16 bars (8 with chord changes) they peak between −3.8 and −6.5 dBFS. `build/demo.wav`
(about 4 minutes) plays Init Drift evolving for 16 bars, 8 bars of each other preset, then Init
Drift following A – F – C – G.

**v1.1 changed nothing for existing projects:** the new controls start where v1.0 was, and the
harness replays 4,432 notes recorded from v1.0 (and a v1.0 saved project) note for note.

## Honest notes

- **CPU:** one voice, always: about 0.16% of one desktop core playing every 16th (harness). The
  MPC One is roughly 10–40× slower, so expect about 2–5% of one core. **Not yet measured on the
  MPC One.**
- **The output never clips:** a soft ceiling above −3 dBFS keeps even everything at maximum with
  Volume +6 dB under 0 dBFS.
- **Lock with presets:** a preset with Lock on starts on its Seed's first pattern. Loading a
  preset whose Seed is the same as the current one keeps the current pattern.
- **Odd time signatures:** a 16-step pattern is laid over whole bars; in 3/4 the steps past the
  pattern's end rest until the next pattern starts.
- When the MPC loops its sequence, a bar line that arrives within a quarter bar of the last one
  (the loop jump landing in the same audio block) is not counted twice.
- Not yet tried on the MPC One: whether chord notes sequenced on the bar line catch the downbeat
  there as they do in the harness, and the feel of the Q-Links.

## Not in v1

A live step display (the MPC skin can only show parameter values; showing the playing step would
mean changing 16 parameters every step, which the MPC records as knob moves); pattern lengths
other than 16 steps, triplet rates; editing single steps; writing your own chord progression,
seventh chords, a chord display; a sound switch (lead/bass/stab/pluck); MIDI output (being looked
at separately); polyphony, chords, unison; MIDI out;
following the MPC's swing setting; phrase tricks (call and answer, fills); custom scales; knobs
for accent chance, oscillator drift, key tracking or filter attack; LFOs, vibrato, pitch bend,
mod wheel, velocity from incoming notes; effects (use the MPC's insert effects); other filter
types, noise, sync; oversampling; more than 9 presets.

## Build, test, skin

From the repo root (`SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}`):

```bash
python3 $SDK/tools/build.py "OCKM DRIFT"
```

```bash
$SDK/.sdk-tools/host_sim "OCKM DRIFT"/build/native/*.dylib test --playing --wav "OCKM DRIFT/build/test.wav"
```

```bash
clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM DRIFT/src" "OCKM DRIFT/tests/harness.cpp" -o "OCKM DRIFT/build/harness" && "OCKM DRIFT/build/harness" "OCKM DRIFT/build/demo.wav"
```

host_sim needs `--playing`: without it the MPC transport is stopped and DRIFT is (correctly)
silent. With it, one warning is expected: the line keeps playing after all-notes-off, because
with Play = Always it doesn't need notes.

After any `ui.json` change, redraw the plum skin and build again:

```bash
python3 "OCKM DRIFT/make_skin.py" && python3 $SDK/tools/build.py "OCKM DRIFT"
```

## Licence

MIT, Copyright (c) OCKM. Built with inMusic's MIT-licensed MPC Plugin SDK (its one-pole and
smoother helpers are used as-is). The zero-delay-feedback ladder, PolyBLEP oscillators,
Euclidean rhythm and integer hash are standard published techniques, written fresh here. Ideas
(scale-step random walks, seeded repeatable variation, chord tones on the beats) come from
OCKM's own OCKM GENERATOR project; no code was copied. No other code or data borrowed.
