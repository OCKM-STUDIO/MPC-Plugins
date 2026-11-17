# OCKM DRIFT: specification (v1)

> **Governing rule: keep it lean. OCKM DRIFT does one job: it plays its own lead line, a fat
> retro analogue mono lead, and that line slowly changes by itself while staying in key.**
> Press Play on the MPC and it plays; turn Mutate and it wanders; turn Lock on and it holds the
> line you like. Anything that does not serve that goes in section 10 ("Not in v1").

Status: **built (v1.0.0), 2026-11-14.** Build 0 errors / 0 warnings, host_sim 18/18 with one
expected warning, harness ALL PASS. Not yet tried on the MPC One. Section 13 lists what the build
changed from this draft. Open questions (section 12) were built as recommended (Simon: "Go ahead").

| Item | Value |
|---|---|
| Plugin name | `OCKM DRIFT` (permanent) |
| Manufacturer | `OCKM` |
| uid | `O8z7` (plugin.json and `MPC_FOURCC`; permanent) |
| Class / slug | `OCKMDRIFT` / `ockm-drift` |
| Output folder | `OCKM - VST - OCKM DRIFT/` |
| Kind | instrument, 0 in / 2 out (the lead is mono and centred) |
| Licence | MIT, Copyright (c) OCKM |
| Target | MPC One (32-bit ARM) first; arm64 comes for free |
| CPU budget | under 6% of one MPC One core per instance; hard ceiling 10% (section 9) |
| Skin tint | **plum** (new: a dusty purple anodised metal; the other five tints are taken) |

## 1. How it plays, in one minute

1. Put OCKM DRIFT on a track. Press Play on the MPC. It plays a 16-step line in A minor,
   locked to the MPC's tempo and grid. No notes needed in the sequence.
2. Every couple of bars a few steps change: a note moves up or down a step in the scale, a
   note appears or drops out, a slide comes or goes. **Mutate** sets how much changes.
3. Like what you hear? Turn **Lock** on. The line stops changing (the knobs still work).
4. Want a different starting line? Change **Seed**. The same Seed always starts the same way.
5. Write chord roots in the MPC sequence (one note per chord, any octave). The line follows
   them, staying in key (section 4).
6. Press Stop: it goes quiet (unless **Run Stopped** is on).

## 2. The sound: a retro analogue mono lead

Signal flow: **3 oscillators → 24 dB ladder filter → drive → amp → DC cut → volume → safety limit**

One voice, always (the sequencer only ever plays one note at a time).

- **Oscillators** (band-limited, PolyBLEP; all three follow the gliding pitch):
  - Osc 1 and Osc 2 at the played pitch, spread apart by **Detune** (0–100% = 0 … ±12 cents).
  - Osc 3 one octave down, level set by **Sub** (0–100%). It thickens the lead like a third
    oscillator dropped an octave on the old monosynths.
  - **Wave** (0–100%) morphs all three from saw (0%) through saw+square (50%) to square (100%).
    The square is made from two saws, so it costs little and stays alias-free.
  - **Gentle pitch drift**, fixed (no knob): each oscillator wanders by up to about ±3 cents,
    slowly and independently, so the sound is alive like old hardware. Worked out every 32
    samples.
  - The oscillators are free-running (not reset on each note), as on analogue hardware, so
    every note starts a little differently. The mix is level-compensated so Wave and Sub
    don't change the loudness much.
- **Filter**: a warm 24 dB/octave resonant low-pass, a zero-delay-feedback four-pole ladder
  (stable when swept fast), with gentle saturation inside the loop so the resonance sings
  rather than whistles.
  - **Cutoff** 0–100%, exponential, ≈ 60 Hz … 12 kHz.
  - **Resonance** 0–100%: up to a strong singing peak just short of self-oscillation; stable
    at 100%. The input is boosted a little as resonance rises, so the lead keeps its body.
  - **Env Amount** 0–100% = 0 … +5 octaves at the envelope peak.
  - **Decay** 0–100% ≈ 30 ms … 2 s: how quickly the filter envelope closes after each note.
    Short = plucky blips, long = vowel-like "waah" notes.
  - Key tracking, fixed: half an octave per octave, so high notes keep their brightness.
  - **Cut Drift** (GEN page) adds a slow wander of the cutoff (section 3.6).
- **Drive** 0–100%: warm soft-clip after the filter, level-compensated. 0% = clean.
- **Amp**: fixed 2 ms attack (click-free, punchy), full sustain while the step's gate is held,
  then **Release** (0–100% ≈ 5 ms … 1.5 s).
- **Length** 10–100%: how long each step's note is held, as a share of a step. Short = staccato
  stabs; 100% = notes run into each other.
- **Glide** 0–100% (≈ 10 … 400 ms): the time of a slide. Slides happen only on steps the
  pattern marks as sliding (**Slide** on the GEN page sets how many): the note is held into the
  next one and glides to its pitch without retriggering the envelopes, the classic "legato
  slide". Glide is constant-time, so big and small intervals take the same time.
- **Accent** 0–100%: how much accented steps stand out. An accented step is up to 4 dB louder,
  with a stronger, snappier filter envelope (up to +1.5 octaves). Which steps are accented is
  part of the pattern (section 3.4).
- **Volume** (parameter 0): −60 … +6 dB, default −6 dB. Smoothed, no side effects.
- **Output never clips**: a transparent safety soft-limit above −3 dBFS; every preset is tuned
  to peak well below that (harness check).
- All filter, pitch and envelope maths at control rate (every 8 samples, interpolated per
  sample). No per-sample `exp`, `pow`, `sin`, `tan` or `tanh`.

## 3. The generator

### 3.1 Clock and grid

- The line has **16 steps**. **Rate** sets the step length: 1/16 (one bar), 1/8 (two bars) or
  1/4 (four bars).
- **When the MPC is playing**, each step is placed from the MPC's song position (`ppqPos`), so
  every step lands exactly on the MPC's grid, sample-accurate within the block, and stays there
  through tempo changes. Step 1 is always on a bar line.
- **Swing** 50–75% delays every second step, like the MPC's own swing (50% = straight). The
  plugin can't read the MPC's swing setting, so match it by ear or by number.
- **When the MPC is stopped**: silent, and any sounding note is released. With **Run Stopped**
  on, the line keeps running at the MPC's tempo (120 BPM if the MPC reports none), so you can
  dial in sounds without pressing Play.
- When the MPC loops its sequence (song position jumps back), the line just carries on: the
  step position follows the MPC, and the evolution keeps going (it counts bars played, not bars
  of the sequence). So a 2-bar MPC loop still gives an ever-changing line.

### 3.2 What a pattern is

Each of the 16 steps stores:

- a **note**, as a step in the scale (not a fixed pitch), so Scale, Root and Range can change
  live and every note stays in key;
- four hidden **dice values** (0–1) that decide, together with the knobs, whether the step
  plays (Density), whether it is accented and whether it slides (Slide).

Because the dice are stored and the knobs are applied live, turning Density or Slide always
does something immediately and predictably, and turning it back gives the same line back.

### 3.3 Rhythm: Euclidean hits plus Density

- **Hits** (1–16) spreads that many notes as evenly as possible over the 16 steps (a Euclidean
  rhythm; 7 hits = a classic rolling house figure, 16 = every step, 4 = four on the floor).
- **Rotate** (0–15) shifts the hits later by that many steps.
- **Density** (0–100%) is applied on top:
  - **50%** (default): exactly the Euclidean hits.
  - **below 50%**: hits drop out, down to none at 0%. Which ones drop is decided by the dice,
    so it is repeatable, and the hit on step 1 is always kept last.
  - **above 50%**: extra notes fill the gaps, up to every step at 100%.
- A step that doesn't play leaves a rest (or, after a slide, the previous note keeps
  sounding until its Length ends).

### 3.4 Notes: random walk, in key

- **Scale**: Minor, Dorian, Phrygian, Major, Mixolydian, Harmonic Minor, Minor Pentatonic,
  Major Pentatonic.
- **Root**: C … B. **Octave** (−2 … +2) moves the whole line; at 0 the root is the one in the
  octave starting at C3 (MIDI 60, the MPC's middle C), so A minor at Octave 0 is centred on
  A3 = 440 Hz. Octave −1 or −2 makes it a bassline.
- **Range** (1–3 octaves): how far above the root the line may wander.
- **Random walk, not wild jumps**: the first pattern is built as a walk. Step 1 is the root;
  every next note moves from the one before by a small amount in the scale, most often one
  scale step, sometimes two, sometimes staying, rarely three. If the walk reaches the top or
  bottom of the Range it bounces back (it never leaps from one end to the other).
- **Anchors**: step 1 is always the root, so the key (and any chord change, section 4) is
  always clear. Notes on the beats (steps 1, 5, 9, 13) prefer chord tones of the root
  (1st, 3rd, 5th in the scale).
- **Accents**: hits on the beats are accented; about one other note in six is too (from the
  dice).
- **Slide** (0–100%): the chance that a note slides into the next one (from the dice). 0% =
  no slides.

### 3.5 Evolution: Mutate, Evolve, Seed, Lock

- **Evolve**: the pattern can change every **1, 2, 4 or 8 bars** (default 2). The change is
  applied at the start of a bar, so phrases change on the bar line. With Rate 1/8 or 1/4
  (patterns longer than a bar), Evolve is rounded up to whole patterns (so changes always land
  at the start of the pattern).
- **Mutate** (0–100%) is the chance, at each change, that each step changes. A step that
  changes does one thing, picked by the dice:
  - its note **walks** one or two scale steps up or down (still bouncing inside Range; beat
    steps still prefer chord tones; step 1 stays the root), or
  - its **rhythm dice** are re-rolled (so with Density away from 50% it may start or stop
    playing), or
  - its **accent or slide dice** are re-rolled.
  - At 25% about four steps change each time: the line is recognisable but always moving. At
    100% everything changes every time, but notes still only move by small steps, so it never
    turns into noise.
  - **Mutate 0% never changes the pattern** (harness check).
- **Seed** (1–999): the starting pattern and every later change come from Seed and the bar
  count, through a fixed hash (no clock, no system random). So **the same Seed always gives the
  same line, bar for bar, every time you press Play**: press Play, and the line starts again
  from its Seed's first pattern. Changing Seed while playing switches to the new Seed's first
  pattern at the next bar.
- **Lock** (Off/On): stops the evolution. The pattern playing when you turn Lock on is kept,
  and it is saved with the MPC project or program (in the plugin's state), so it comes back
  exactly when the project is reopened. The knobs still work while locked (Density, Hits, Scale,
  Range and so on are applied live to the frozen pattern). Turning Lock off continues evolving
  from the locked pattern; it does not jump back. Changing Seed while locked gives you the new
  Seed's first pattern, locked (a quick way to audition seeds).
  - A preset with Lock on (and no saved pattern) starts locked on its Seed's first pattern.

### 3.6 Cut Drift

- **Cut Drift** (0–100%): a slow, smooth wander of the filter cutoff, up to ±2 octaves at 100%,
  with a new target about every 4 bars and smooth curves in between (no steps). It follows the
  bar count and Seed, so it is repeatable too. 0% (default) = off.

## 4. Notes from the MPC: chord changes

Plugins can't send MIDI out, so DRIFT always plays its own sound. Notes played on the pads or
written in the MPC sequence don't play a sound; they **move the line**. Two settings:

**Keys** (default **In Key**):

- **Off**: notes are ignored. The line plays from Root.
- **In Key** (default, for chord changes): the note you play is read as a chord root inside the
  chosen key. The whole line moves up or down **by scale steps**, so it stays in key. Example:
  A minor, the line starts on A. Play or sequence an F and the line moves so step 1 is F, and
  every other note moves the same number of scale steps (F, A, C instead of A, C, E). Play a G,
  it follows to G. Write A – F – C – G in the MPC (one note per bar) and the line follows the
  progression, every note still in A minor. A note outside the key snaps down to the nearest
  scale note.
- **Transpose**: the whole line moves by the exact number of semitones between the note and
  Root (A minor line, play C: the line becomes a C minor line). Use this to follow a
  modulation, or for techno lines that move as a block. Notes then follow the played key, not
  the Scale/Root setting.

In both modes **only the note name counts, not the octave** (an F in any octave does the
same), and the line moves to the **nearest** position (A → F moves down 2 scale steps, not up
5), so the line stays in its register whatever octave Simon writes the chords in. The line's
register comes from **Octave**.

**Play** (default **Always**):

- **Always**: the line plays whenever the MPC is playing (or Run Stopped is on). The last note
  received stays in force until the next one ("latched"). Before any note, the line plays from
  Root.
- **Held**: the line only plays while a note is held. Write the chord roots as long notes in
  the MPC sequence, and the line plays only where they are, which lets the sequence decide
  where the lead comes in and out. Releasing all notes releases the line's note.

Timing details:

- A new note takes effect **from the next step**, and a note that arrives within 25 ms after a
  step has started also re-pitches that step (so chord notes recorded slightly late still catch
  the downbeat).
- With several notes held (a whole chord), the **lowest** held note counts.
- Velocity of the incoming notes is ignored; the pattern's accents shape the dynamics.
- All-notes-off releases everything and goes back to Root (in Held mode, silence).

## 5. Parameters (order and ids permanent once built)

| # | id | Name | Range | Default |
|---|---|---|---|---|
| 0 | `volume` | Volume | −60 … +6 dB | −6 dB |
| 1 | `cutoff` | Cutoff | 0–100% (≈60 Hz … 12 kHz, exponential) | 40% |
| 2 | `reso` | Resonance | 0–100% | 35% |
| 3 | `envamt` | Env Amount | 0–100% (0 … +5 oct) | 50% |
| 4 | `decay` | Decay | 0–100% (≈30 ms … 2 s) | 35% |
| 5 | `wave` | Wave | 0–100% (saw → square) | 25% |
| 6 | `detune` | Detune | 0–100% (0 … ±12 cents) | 35% |
| 7 | `sub` | Sub | 0–100% (osc 3, one octave down) | 30% |
| 8 | `glide` | Glide | 0–100% (≈10 … 400 ms, slide steps only) | 30% |
| 9 | `length` | Length | 10–100% of a step | 50% |
| 10 | `release` | Release | 0–100% (≈5 ms … 1.5 s) | 15% |
| 11 | `accent` | Accent | 0–100% | 50% |
| 12 | `drive` | Drive | 0–100% | 20% |
| 13 | `keys` | Keys | Off, In Key, Transpose | In Key |
| 14 | `play` | Play | Always, Held | Always |
| 15 | `runstop` | Run Stopped | Off, On | Off |
| 16 | `mutate` | Mutate | 0–100% | 25% |
| 17 | `evolve` | Evolve | 1 Bar, 2 Bars, 4 Bars, 8 Bars | 2 Bars |
| 18 | `seed` | Seed | 1–999 | 1 |
| 19 | `lock` | Lock | Off, On | Off |
| 20 | `scale` | Scale | Minor, Dorian, Phrygian, Major, Mixolydian, Harm Minor, Minor Pent, Major Pent | Minor |
| 21 | `root` | Root | C … B | A |
| 22 | `range` | Range | 1–3 oct | 1 |
| 23 | `octave` | Octave | −2 … +2 | 0 |
| 24 | `hits` | Hits | 1–16 | 7 |
| 25 | `rotate` | Rotate | 0–15 | 0 |
| 26 | `density` | Density | 0–100% (50% = just the hits) | 50% |
| 27 | `rate` | Rate | 1/16, 1/8, 1/4 | 1/16 |
| 28 | `slide` | Slide | 0–100% | 20% |
| 29 | `cutdrift` | Cut Drift | 0–100% (0 … ±2 oct) | 0% |
| 30 | `swing` | Swing | 50–75% | 50% |

31 parameters, all automatable except Seed, Lock, Keys, Play and Run Stopped (settings, not
performance moves). Choice parameters restore by id and index, so new choices are only ever
appended.

## 6. UI and Q-Links: two pages, four columns each

Page 1 **"SOUND"** (each column is one Q-Link bank of four, top to bottom):

| FILTER | OSC | AMP | PLAY |
|---|---|---|---|
| **Cutoff** | Wave | Length | Keys (switch) |
| Resonance | Detune | Release | Play (switch) |
| Env Amount | Sub | Accent | Run Stopped (switch) |
| Decay | Glide | Drive | Volume (fader) |

Page 2 **"GEN"**:

| EVOLVE | NOTES | RHYTHM | MOVE |
|---|---|---|---|
| **Mutate** | Scale | Hits | Slide |
| Evolve | Root | Rotate | Cut Drift |
| Seed | Range | Density | Swing |
| Lock (switch) | Octave | Rate (switch) | *OCKM DRIFT badge* |

- Cutoff is the top-left knob on SOUND and Mutate the top-left on GEN: the two knobs you ride.
- Program-mode Q-Links follow the SOUND page layout, except the PLAY column's three settings
  (Keys, Play, Run Stopped), which become Mutate, Density and Cut Drift (performance knobs rather
  than settings). So in Program mode the fourth column is Mutate, Density, Cut Drift, Volume.
- Choice parameters (Keys, Play, Evolve, Scale, Root, Rate) are sliders with a menu overlay
  (no segment captions to go missing); Run Stopped and Lock are toggle switches.
- Skin: copied from `OCKM Gate/make_skin.py`, adding a **plum** tint to its `TINTS` table
  (default `--tint plum`), with the panel-font letters and digits these labels need (the other
  sessions found Gate's font lacks some). Black knurled knobs, amber-lit switches, printed
  column headings, battered brushed metal. Switch captions are also printed on the panel (the
  OCKM-03 session found the MPC One may hide the generated ones).

## 7. Factory presets (9)

Each is a different kind of evolving line. All peak below 0 dBFS (target ≤ −3 dBFS).

| # | Name | Kind of line | Main settings |
|---|---|---|---|
| 1 | **Init Drift** | the defaults: a rolling minor lead that changes a few notes every 2 bars | A Minor, Hits 7, Mutate 25, Evolve 2 |
| 2 | **Slow Burn** | deep-house lead that turns very slowly, cutoff breathing over minutes | D Dorian, Hits 5, Mutate 10, Evolve 8, Cut Drift 45, Decay 55 |
| 3 | **Rolling Techno** | driving, dark, every bar slightly different | F Phrygian, Hits 11, Density 60, Octave −1, Mutate 30, Evolve 1, Drive 45, Reso 50 |
| 4 | **Peak Arp** | all 16ths, two octaves, an arpeggio that rewrites itself | A Minor Pent, Hits 16, Range 2, Mutate 40, Evolve 1, Decay 15, Length 40 |
| 5 | **Sparse Dub** | few notes, long tails, lots of space | G Dorian, Hits 3, Rotate 2, Rate 1/8, Release 55, Cut Drift 60, Mutate 20, Evolve 4 |
| 6 | **Singing Lead** | slow sliding melody, open and resonant | C Mixolydian, Rate 1/8, Hits 6, Slide 60, Glide 50, Range 2, Length 90, Reso 45 |
| 7 | **Deep Walker** | a walking bassline (same engine, two octaves down) | E Minor, Octave −2, Hits 6, Sub 60, Cutoff 25, Mutate 15, Wave 60 |
| 8 | **Chaos Garden** | restless, busy, still in key | B Harm Minor, Hits 9, Density 75, Range 3, Mutate 70, Evolve 1, Cut Drift 50 |
| 9 | **Frozen Hook** | a hook that stays put: Lock on, Seed 42 | C Minor, Hits 7, Rotate 3, Lock On, Slide 30 |

## 8. Realtime and state

- `process()` never allocates, locks, logs or touches files.
- Pattern generation and mutation happen at most once per bar, inside `process()`, as plain
  arithmetic on two fixed 16-step arrays (about 16 × 10 operations): no allocation, no spike.
- Lock's saved pattern: `saveExtraState` writes a small versioned blob (version byte + 16 steps
  × note + 4 dice, plus a "has pattern" flag, about 300 bytes). `loadExtraState` stages it and
  `reset()` swaps it in on the audio thread (the framework's rule).
- Seed and bar count go through a fixed integer hash, identical on every CPU, so the desktop
  harness and the MPC give the same line.
- Denormals: filter and DC states flushed once the voice is silent.

## 9. CPU budget (MPC One, 32-bit ARM)

Mono, one voice, always. Per sample: three oscillators made from six PolyBLEP saws (~60
operations), the ladder with in-loop saturation (~50), two envelopes, glide, drive, DC cut and
limiter (~30): about 140 simple float operations per sample, roughly 7 million per second at
48 kHz. The sequencer adds one position compare per sample and a few hundred operations per bar.

- Coefficients (`exp2` for pitch and cutoff, `tan` for the filter) every 8 samples,
  interpolated; drift and Cut Drift every 32 samples.
- **Budget: under 6% of one MPC One core per instance; hard ceiling 10%.** On the desktop
  harness that corresponds to well under 0.4% of one core, which the harness prints and checks.
- No oversampling (band-limited oscillators, gentle saturation). If the harness hears aliasing
  at Drive 100% on high notes, 2× oversampling of the filter would be considered and the budget
  re-measured.
- Memory: a few kilobytes; nothing allocated in `process()`.

## 10. Not in v1

Left out on purpose, under the governing rule:

- **A live step display.** The MPC skin can only show parameter values. Showing the playing
  step would mean the plugin changing 16 parameters every step, which the MPC would treat as
  knob moves (recorded into automation, saved in the project). Not worth the risk. The line is
  for listening, not reading.
- Pattern lengths other than 16 steps (polymeter), step lengths other than 1/16, 1/8, 1/4
  (no triplets)
- Editing single steps by hand
- Polyphony, chords, unison, a second voice
- MIDI out (plugins can't send MIDI to the MPC)
- Following the MPC's own swing setting (the plugin can't read it; use Swing)
- Phrase tricks from the generator project (call and answer, fills, turnarounds)
- Custom scales, a "chord tones only" mode, a knob for how far the walk may jump
- Knobs for accent chance, oscillator drift amount, key tracking, filter envelope attack
- LFOs, vibrato, pitch bend, mod wheel, velocity from the incoming notes
- Effects (delay, reverb, chorus): use the MPC's insert effects
- Filter types other than the 24 dB low-pass; noise; oscillator sync
- Oversampling (section 9)
- More than 9 presets

## 11. Tests

1. **Build**: `python3 $SDK/tools/build.py "OCKM DRIFT"` → 0 validator errors.
2. **host_sim**: `test --wav build/test.wav` → no FAIL; every WARN explained. (host_sim's
   own note tests send MIDI notes without a playing transport, so expect it to report a quiet
   plugin unless Run Stopped is on; this will be checked and explained.)
3. **`tests/harness.cpp`** (includes the plugin source, ends with `ALL PASS`). It drives the
   plugin with a simulated MPC transport and logs every note the line plays (step, pitch,
   accent, slide, sample position) as well as the audio:
   - **Same Seed, same line**: two fresh instances, same settings, 32 bars each → identical
     note logs and bit-identical audio; a different Seed → a different line.
   - **Press Play again, same line**: stop and restart → the note log starts over identically.
   - **Lock freezes**: Lock on at bar 5, Mutate 100% → bars 5–32 repeat the bar-5 pattern
     exactly; Lock off → it changes again from that pattern. Save state with Lock on, load into
     a new instance → the same locked pattern plays.
   - **Mutate 0 never changes**: Mutate 0%, every Evolve setting, 64 bars → every bar's
     pattern identical to bar 1.
   - **Mutate changes**: Mutate 25% → the pattern changes at Evolve boundaries only, and by
     small steps (no note moves more than 2 scale steps per change).
   - **In key**: every Scale × every Root × Range 1–3 × Keys Off/In Key with random chord notes
     → every played pitch class is in the scale (in Transpose mode: in the transposed scale).
   - **On the grid**: at 98, 124 and 140 BPM, every Rate, Swing 50% → every note starts within
     one sample of `step × step length`; with Swing → only the off steps move, by the right
     amount. Through an MPC-style loop (song position jumping back) → steps stay on the grid and
     the evolution keeps counting.
   - **Silent when stopped**: transport stopped, Run Stopped off → exact silence (after the
     release of a note that was playing); Run Stopped on → the line plays.
   - **Keys**: In Key with A–F–C–G chord notes → step 1 of each bar is the chord root, all
     notes in A minor; Held mode → silent with no note held; a note 10 ms late still re-pitches
     the downbeat.
   - **Rhythm**: Density 50% → exactly the Euclidean hits (checked against a reference table);
     0% → silence; 100% → all 16 steps.
   - **No clipping across presets**: every preset over 16 bars peaks below 0 dBFS (printed per
     preset); worst case (Resonance, Drive, Accent, Env Amount at 100%, Volume +6 dB, every
     note from Octave −2 to Range 3 at +2) stays below 0 dBFS.
   - **Resonance stable**: Resonance 100% across the cutoff range → finite, no runaway.
   - **Volume (parameter 0) harmless**: nudging it changes only the level, not the line.
   - CPU on the desktop printed and under 0.4% of one core.
4. **Demo WAV**: `build/demo.wav`: Init Drift evolving over 16 bars, then 8 bars of each other
   preset, then Init Drift following an A–F–C–G progression (In Key).
5. **Package**, then `python3 sd_card.py` with no warnings, then hand-over with the SD-card
   steps.
6. On the MPC One (Simon): CPU meter while playing; does the line stay on the MPC's grid when
   the sequence loops; do chord notes sequenced on the bar line catch the downbeat.

## 12. Open questions for Simon

1. **Keys default In Key, Play default Always** (the line plays on its own, and chord notes move
   it in key). OK, or should it default to Held, so it only plays where notes are written?
2. **Evolution restarts from the Seed every time Play is pressed** (repeatable). The other
   option is to keep evolving across Stop/Play, which is less repeatable. Recommended: restart.
3. Is **Octave 0 = around A3 (440 Hz)** the right home register for a house/techno lead, or
   should the default sit an octave lower?
4. **Swing** is included (one knob) because the plugin can't follow the MPC's swing. Keep it?
5. Plum tint OK?

## 13. Changes made while building (v1.0.0)

1. **Output +6 dB** (voice make-up gain), so presets peak at −3.8 … −6.3 dBFS instead of about
   −12 dBFS. The worst case (everything at 100%, Volume +6 dB) peaks at −0.09 dBFS, held by the
   ceiling.
2. **MPC loop jumps are counted once.** When an audio block straddles the MPC's loop point, the
   plugin sees the next bar line and then, a few milliseconds later, bar 1 again. A bar line
   within a quarter bar of the previous one is no longer counted as another bar (it can still be
   the pattern start an Evolve point waits for). Before the fix a 2-bar MPC loop evolved about
   22 times in 16 bars; now 15.
3. **State:** if a Seed change hasn't been applied yet when the MPC saves, the saved pattern is
   the new Seed's first pattern (what will play), and a just-loaded pattern is saved back as
   loaded. host_sim's state round-trip is now byte-identical.
4. **Source split** into `src/OCKMDRIFT.cpp` (parameters, clock, keys, state), `src/Generator.h`
   (scales, hash, pattern, Mutate, Euclid) and `src/LeadVoice.h` (the sound), following the
   SDK's ~800-line guideline.
5. **Tests:** host_sim runs with `--playing` (stopped, DRIFT is silent by design). The one
   remaining warning is by design: with Play = Always the line keeps playing after all-notes-off.
   The rhythm reference check is a property check (exact hit count, gaps differing by at most
   one step, first hit at Rotate) over all 16 × 16 Hits/Rotate settings, plus Hits 7 =
   `x..x.x.x..x.x.x.`.

Harness results (selection): 55,296 notes across every Scale × Root × Range × Keys mode, all in
key; 1,344 notes at 98/124/140 BPM, every Rate, Swing 50/66, all within one sample of the grid;
Mutate 0% unchanged over 64 bars at every Evolve; Mutate 25% at Evolve 4 changed 15 times in 64
bars, only on 4-bar lines, notes moving at most 2 scale steps; Lock held 27 bars at Mutate 100%
and came back from saved state; exact silence 0.25 s after Stop; a chord note 10 ms late
re-pitches the downbeat; CPU 0.13% of one desktop core.

## 14. v1.1: Chords, Chord Stick and Rhythm (built, v1.1.0)

Status: **built 2026-11-17** (Simon: "Go ahead"). Build 0 errors / 0 warnings, host_sim 18/18
with the one expected warning, harness ALL PASS (including note-for-note v1.0 compatibility over
4,432 recorded notes and a v1.0 saved project). Not yet tried on the MPC One. Section 14.11 lists
what the build settled.

Simon asked for three additions (2026-11-17). They are added as **new parameters at the end** of
the list, and their defaults reproduce v1.0 exactly, so existing projects and presets play the
same line note for note (harness check against a v1.0 recording). Version 1.1.0.

### 14.1 Chord Stick: how close the line sticks to the chord

At the moment, notes on the beats move to a chord tone (1st, 3rd or 5th of the current chord)
75% of the time, and the other steps never do. **Chord Stick** (0–100%) makes that a knob. Each
step still uses its own dice value, so the choice is repeatable and follows the Seed:

| Chord Stick | Beat steps (5, 9, 13) | Other steps |
|---|---|---|
| **0%** | free: any scale note the walk lands on | free |
| **25%** | chord tone 3 times in 8 | free |
| **50%** (default = v1.0) | chord tone 3 times in 4 | free |
| **75%** | always a chord tone | chord tone half the time |
| **100%** | always a chord tone | **always a chord tone**: an arpeggio of the chord |

(The rule: a beat step snaps when its dice value is below 1.5 × Chord Stick; any other step when
it is below 2 × (Chord Stick − 50%).)

- Step 1 stays the chord root at every setting (the anchor that makes chord changes audible).
- "The chord" is the triad on the current chord root, built from the Scale: in A minor with the
  chord on F, the chord tones are F, A and C. It follows MPC notes (section 4) and the built-in
  Chords (14.2) the same way.
- A note moves to the **nearest** chord tone (at most one or two scale steps), so the line keeps
  its shape. It stays inside Range.

### 14.2 Chords: a built-in progression

**Chords** picks a progression for the line to follow by itself, so you don't have to write
chord notes in the MPC. The chords are **scale degrees** of the Scale and Root you've set, so
every progression works in any key and scale, and every note stays in key:

| # | Chords | In A minor | Feel |
|---|---|---|---|
| 0 | **Off** (default) | follows MPC notes, as in v1.0 | |
| 1 | **1-6-3-7** | Am F C G | the classic emotional minor loop |
| 2 | **1-4** | Am Dm | deep, two-chord house |
| 3 | **1-7-6-7** | Am G F G | driving, anthemic |
| 4 | **1-5** | Am Em | dark techno pendulum |
| 5 | **1-6-4-5** | Am F Dm Em | soulful, deep house |
| 6 | **1-3-4-6** | Am C Dm F | rising, uplifting |
| 7 | **2-5-1-1** | Bdim Em Am Am | turnaround (sounds jazziest in Dorian or Major) |

- **Chord Bars** (1, 2 or 4 bars per chord; default 1) sets how long each chord lasts.
- The chords move the line exactly as MPC notes do (section 4): **In Key** moves it by scale
  steps; with Keys on **Transpose** the whole line moves by the chord's semitones instead. With
  Keys **Off** the progression still plays (Keys Off only means "ignore MPC notes").
- In the pentatonic scales, the chord degree is taken from the matching seven-note scale (Minor
  Pent → Minor, Major Pent → Major) and then snapped into the pentatonic scale, so it stays in key.
- **Timing:** chord changes land on bar lines. The progression starts from its first chord every
  time you press Play, counting bars played (like Evolve), so starting from the top of your song
  keeps it in step. It can't see your MPC sequence, so if you start mid-song it may sit a chord
  or two out against your other parts. An MPC loop of a whole number of chord cycles (for example
  4 bars at 1 bar per chord) stays in step.
- **MPC notes always win:** as soon as a note arrives from the MPC (pad or sequence), the
  progression steps aside and the line follows your notes until the next time you press Play.
  So you can use the progression to sketch, then write your own chords and they take over.
- With Play on **Held**, the line still only plays while a note is held (and that note sets the
  chord, as above).

### 14.3 Rhythm: hand-picked grooves

The even-spread rhythm (Hits) is good at rolling, even patterns but can't make the lopsided
figures house and techno lean on. **Rhythm** picks the base rhythm (x = note, . = rest):

| # | Rhythm | Pattern | Use |
|---|---|---|---|
| 0 | **Even (Hits)** (default = v1.0) | from Hits | as in v1.0 |
| 1 | **Offbeat** | `..x...x...x...x.` | the classic between-the-kicks stab |
| 2 | **Rolling** | `.xxx.xxx.xxx.xxx` | rolling techno: every 16th except on the kick |
| 3 | **Gallop** | `x.xxx.xxx.xxx.xx` | an 8th and two 16ths, every beat |
| 4 | **Syncopated** | `x..x..x...x..x..` | 3-3-4-3-3 push |
| 5 | **Two-Step** | `x.....x.x.....x.` | broken, swung |
| 6 | **Stab** | `..x....x..x.....` | sparse stabs |
| 7 | **Call** | `x.xx.x.x........` | busy first half, an empty second half to answer |

- **Hits** only works on Even; the other rhythms ignore it (and the Hits knob shows its value as
  usual, just with no effect).
- **Rotate**, **Density**, **Slide** and **Mutate** work on every rhythm exactly as before:
  Density 50% plays exactly the pattern, lower drops notes, higher fills the gaps (from the same
  dice, so it stays repeatable), and the line keeps evolving.
- Accents: as before, notes on the beats are accented, plus about one in six others.
- Ideas for these grooves come from OCKM GENERATOR's rhythm lists; the patterns are written fresh.

### 14.4 New parameters (appended; order and ids permanent once built)

| # | id | Name | Range | Default |
|---|---|---|---|---|
| 31 | `chordstick` | Chord Stick | 0–100% | 50% (= v1.0) |
| 32 | `chords` | Chords | Off, 1-6-3-7, 1-4, 1-7-6-7, 1-5, 1-6-4-5, 1-3-4-6, 2-5-1-1 | Off |
| 33 | `chordbars` | Chord Bars | 1 Bar, 2 Bars, 4 Bars | 1 Bar |
| 34 | `rhythm` | Rhythm | Even, Offbeat, Rolling, Gallop, Syncopated, Two-Step, Stab, Call | Even |

All four are automatable (Chords and Rhythm can be changed by automation in the sequence; a
change lands on the next step, and a Chords change on the next bar line).

### 14.5 UI: a third page

The screen layout isn't permanent (only parameter ids and order are), so the GEN page is tidied
to make room and a third page added:

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
| Chord Bars | | | *OCKM DRIFT badge* |
| Chord Stick | | | |
| | | | |

- The badge moves from GEN to HARMONY; HARMONY's other columns stay empty, ready for v2 (for
  example the MIDI-out settings, if that goes ahead).
- SOUND and the Program-mode Q-Links are unchanged.
- Choices use the slider-with-menu control, as before. The value texts (`1-6-3-7` and so on) are
  drawn by the MPC, so the panel font needs nothing new (the CHORDS heading uses letters it has).

### 14.6 Presets: three new ones (12 in all)

The nine v1.0 presets are unchanged (the new parameters sit at their defaults). Three are added
at the end of the list to show off the new features:

| # | Name | The line | Main settings |
|---|---|---|---|
| 10 | **Chord Rider** | follows a progression by itself, sticking close to the chords | A Minor, Chords 1-6-3-7, Chord Bars 2, Chord Stick 80, Hits 7, Mutate 20 |
| 11 | **Offbeat Stabs** | short stabs between the kicks, moving through 1-4 | D Dorian, Rhythm Offbeat, Chords 1-4, Chord Bars 2, Length 25, Decay 20, Chord Stick 100 |
| 12 | **Rolling Bass** | a rolling techno bassline on 1-5 | F Phrygian, Rhythm Rolling, Octave −2, Chords 1-5, Chord Bars 4, Sub 60, Cutoff 30, Mutate 20 |

### 14.7 CPU, realtime, state

- Everything new happens once per step or once per bar (a few dozen operations). No change to the
  per-sample cost or the CPU budget.
- State: the saved pattern is unchanged (Chords, Chord Stick and Rhythm are parameters).
  Projects saved with v1.0 load with the new parameters at their defaults, so they play exactly
  as before.

### 14.8 Not in v1.1

Writing your own progression step by step; more than 8 progressions or 8 rhythms; seventh or
ninth chords; chord tones other than 1-3-5; a chord display on the screen (same reason as the
step display); a sound switch (lead/bass/stab/pluck): possible later; MIDI output: a separate
look (section 14.10).

### 14.9 Tests added to the harness

- **v1.0 compatibility:** before any v1.1 code, record v1.0's note log (Init Drift and four other
  presets, 32 bars, with and without MPC chord notes). v1.1 at default settings must reproduce it
  note for note, and a v1.0 saved state must load and play the same.
- **Chord Stick:** at 0% no step is snapped (notes = the folded walk); at 100% every note is a
  chord tone of the current chord (with MPC chords and with the built-in Chords); at 50%
  identical to v1.0; notes never leave Range.
- **Chords:** for every progression × Chord Bars × Scale × Root, step 1 of each bar is the right
  chord root, chords change only on bar lines, and every note is in key; Transpose moves by the
  chord's semitones; a pressed MPC note takes over and keeps control until the next Play; the
  progression restarts on Play.
- **Rhythm:** at Density 50% each rhythm plays exactly its pattern (shifted by Rotate); 0% plays
  nothing; 100% plays every step; Mutate still evolves it; Hits has no effect except on Even.
- All existing checks still pass (same Seed, Lock, Mutate 0, grid, silence, no clipping across
  all 12 presets).

### 14.10 Separate: MIDI output

Kept out of v1.1 on purpose. Whether DRIFT can also send its line as MIDI (through its own MIDI
port, as another developer's plugin does) depends first on a test on the MPC One, then gets its
own SPEC section.

### 14.11 Settled while building (v1.1.0)

1. **Compatibility is recorded, not assumed:** `tests/v1_0_notes.txt` (4,432 notes: the nine v1.0
   presets over 32 bars, with and without MPC chord notes) and `tests/v1_0_state.hex` (a v1.0
   project with a locked line) were recorded from the v1.0 code before any change. The harness
   replays them against v1.1.
2. **Chord Stick at 50% is exactly v1.0:** the knob is read as its 0–1 position, so the default
   gives a beat-step threshold of exactly 0.75, as v1.0 had.
3. **A note held when Play is pressed** keeps control over Chords (so Held mode with Chords on
   follows the held note from the first step).
4. Programs 10–12 appended; info version 1100, plugin.json 1.1.0. The HARMONY page's Q-Links map
   Chords, Chord Bars and Chord Stick; Program-mode Q-Links are unchanged from v1.0.

Harness results (selection): 504 renders of every progression × Chord Bars × Scale × 3 roots with
step 1 always the right chord root and every note in key; Transpose moves by the chord's
semitones; Chord Stick 100% makes every note a chord tone with built-in Chords and with MPC chord
notes; every groove × Rotate plays exactly its pattern at Density 50%; all 12 presets peak between
−3.8 and −6.5 dBFS; CPU 0.16% of one desktop core. `build/demo.wav` (3 min 50 s) now ends each
preset tour with the three v1.1 presets.
