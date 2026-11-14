# OCKM Wave — specification (v1)

> **Governing rule: keep it lean. OCKM Wave does one job: play wavetables well, and teach how
> wavetable synthesis works while you play.** One wavetable oscillator, a sub, one filter, two
> envelopes and one LFO. Everything else goes in section 10 ("Not in v1").

Status: **built (v1).** Decisions taken while building are in section 12 and marked "(build)".

OCKM Wave is a polyphonic wavetable synth instrument for the MPC One. Every factory preset is a
small lesson: it shows one wavetable idea, and the README explains that idea in plain English.
Naming rule: no other companies' product or brand names anywhere (see ../HOUSE-STYLE.md).

| Item | Value |
|---|---|
| Plugin name | `OCKM Wave` (permanent) |
| Manufacturer | `OCKM` |
| uid | `O57W` (from `new_plugin.py`; permanent) |
| Kind | instrument, 0 in / 2 out |
| Output folder | `OCKM - VST - OCKM Wave/` |
| Project folder | `OCKM Wave/` in the MPC-Plugins repo |
| Licence | MIT, `Copyright (c) OCKM` |
| Look | brushed metal, **champagne** tint (`make_skin.py` copied from OCKM Gate, `--tint champagne`) |
| Voices | **6** (Poly), or 1 (Mono / Legato) |
| CPU budget | see section 8: worst case under 15% of a 128-frame block on the MPC One, typical presets under 5% |
| Memory | about 3 MB of wavetables, built once and shared by every instance |
| Inspiration | the SPECS.md of *Panacea* by KRRSRR (MIT). Ideas only, no code; credited in the README |

## 1. What a wavetable is (the idea the whole plugin teaches)

A normal synth oscillator plays one fixed shape (saw, square). A **wavetable** is a stack of
16 different single-cycle shapes, called **frames**, like the pages of a flip-book. The
**Position** knob chooses which page you hear; between two pages the synth blends them. Turn
Position while a note plays, or let an envelope or LFO turn it, and the tone changes shape
over time. That moving tone is what wavetable synths are for.

## 2. Signal flow (per voice)

```
note -> pitch (octave, glide, bend ±2 st, LFO vibrato)
     -> WAVETABLE OSC: table, position (+env, +LFO, +mod wheel), warp (+env, +LFO),
                       unison 1-5 copies, detuned and spread in stereo
     +  SUB: sine one octave below, mono
     -> FILTER (12 dB state-variable: LP / BP / HP), cutoff +key track +env +LFO
     -> x AMP ENV x velocity
sum of voices -> Output level -> L/R
```

Modulation (envelope, LFO, position, warp, cutoff) is worked out every 16 samples per voice and
ramped per sample, so it is smooth but cheap.

## 3. The oscillator

### 3.1 Tables (all generated in code; no files to license)

8 tables, each 16 frames of one cycle. They are made from simple formulas when the plugin loads,
so nothing is copied and no sample licence is needed. Each table shows a different way of
building a wavetable:

| # | Table | Frame 1 → frame 16 | Idea it shows |
|---|---|---|---|
| 0 | **Shapes** | sine → triangle → saw → square | Morphing between classic shapes |
| 1 | **Harmonics** | 1 harmonic → 16 harmonics added one at a time | Additive building: position = brightness |
| 2 | **Pulse** | square (50%) → thin pulse (5%) | Pulse width as a table (PWM without a PWM knob) |
| 3 | **Vowels** | "ah" → "eh" → "ee" → "oh" → "oo" | Formants: shaping the spectrum like a mouth |
| 4 | **Sync** | saw with a hard-synced partner climbing 1× → 8× | A "baked" hard-sync sweep |
| 5 | **FM** | sine with FM index 0 → 8 | FM sounds stored as frames |
| 6 | **Fold** | sine, wavefolded harder and harder | Distortion stored as frames |
| 7 | **Steps** | smooth saw → 4-step staircase | Lo-fi, quantised digital shapes |

### 3.2 No aliasing (band-limited playback)

High notes on a bright frame would fold harmonics back as harsh, out-of-tune noise ("aliasing").
To prevent it, every frame is stored at about 10 brightness levels ("mipmaps"): the full frame,
then copies with the top half of the harmonics removed, and so on. Each voice picks the level
whose highest harmonic stays below 0.45 × the sample rate, once per 16 samples.

Build: each frame's spectrum is found with a small FFT (written for this plugin), then each
level is rebuilt with an inverse FFT. Top level 2048 samples, smaller levels down to 256 samples.
Built once, in a function-local `static` (shared by all instances, thread-safe), never in
`process()`. Target build time: under 50 ms on a desktop, about 1 s on the MPC One at plugin load.

### 3.3 Warp (bending the playback, not the table)

Warp changes **how the frame is read**, a second way to change the tone on top of Position.
`Warp Amt` 0% always sounds identical to Warp Off.

| Mode | What it does | Sounds like |
|---|---|---|
| Off | reads the frame straight | the table as stored |
| **Sync** | reads the frame 1× → 4× faster and restarts each cycle | hard-sync lead, tearing sweep |
| **Bend** | squeezes the first half of the cycle, stretches the second | nasal, phase-distortion tone |
| **Mirror** | plays the frame forwards then backwards | hollow, organ-like, an octave of brightness |
| **Quantize** | snaps the read position to 64 → 4 steps | bit-crushed, gritty digital |

Sync, Bend and Mirror add harmonics, so the voice moves up a mip level to stay alias-free.
Quantize is meant to be gritty and its steps are not band-limited (said in the README).

### 3.4 Unison

`Unison` 1–5 copies of the oscillator per voice. `Detune` spreads their pitch up to ±50 cents,
`Spread` spreads them left–right. Each copy starts at a random phase so stacks sound thick, not
phasey. Level is scaled by 1/√copies so the volume stays about the same.

## 4. Parameters (order and ids permanent once built)

Parameter 0 is Output, harmless when the host nudges it. New parameters go at the end only.

| # | id | Name | Range / choices | Default |
|---|---|---|---|---|
| 0 | `output` | Output | −24…+6 dB | −3 dB |
| 1 | `table` | Table | Shapes, Harmonics, Pulse, Vowels, Sync, FM, Fold, Steps | Shapes |
| 2 | `position` | Position | 0–100% | 67% (the saw frame) |
| 3 | `warp` | Warp | Off, Sync, Bend, Mirror, Quantize | Off |
| 4 | `warpamt` | Warp Amt | 0–100% | 0 |
| 5 | `unison` | Unison | 1–5 | 1 |
| 6 | `detune` | Detune | 0–100% (0…±50 ct) | 25 |
| 7 | `spread` | Spread | 0–100% | 60 |
| 8 | `sub` | Sub Level | 0–100% | 0 |
| 9 | `octave` | Octave | −3…+3 | 0 |
| 10 | `voicemode` | Voice Mode | Poly, Mono, Legato | Poly |
| 11 | `glide` | Glide | 0–1000 ms | 0 |
| 12 | `velamp` | Vel>Amp | 0–100% | 50 |
| 13 | `ftype` | Filter | LP, BP, HP | LP |
| 14 | `cutoff` | Cutoff | 20–18000 Hz (log) | 18000 |
| 15 | `reso` | Resonance | 0–100% | 10 |
| 16 | `keytrack` | Key Track | 0–100% | 50 |
| 17 | `aatk` | Amp Attack | 1–5000 ms | 2 |
| 18 | `adec` | Amp Decay | 1–5000 ms | 300 |
| 19 | `asus` | Amp Sustain | 0–100% | 80 |
| 20 | `arel` | Amp Release | 1–8000 ms | 250 |
| 21 | `matk` | Mod Attack | 1–5000 ms | 2 |
| 22 | `mdec` | Mod Decay | 1–5000 ms | 400 |
| 23 | `msus` | Mod Sustain | 0–100% | 0 |
| 24 | `mrel` | Mod Release | 1–8000 ms | 300 |
| 25 | `envpos` | Env>Pos | −100…+100% | 0 |
| 26 | `envwarp` | Env>Warp | −100…+100% | 0 |
| 27 | `envcut` | Env>Cutoff | −48…+48 st | 0 |
| 28 | `lfoshape` | LFO Shape | Sine, Tri, Saw, Square, S&H | Tri |
| 29 | `lforate` | LFO Rate | 0.05–20 Hz (log) | 1 Hz |
| 30 | `lfosync` | LFO Sync | Off, On | Off |
| 31 | `lfodiv` | LFO Div | SDK division list (1/64…2/1, T, D) | 1/4 |
| 32 | `lfopos` | LFO>Pos | 0–100% (LFO swings ± around Position) | 0 |
| 33 | `lfowarp` | LFO>Warp | 0–100% | 0 |
| 34 | `lfocut` | LFO>Cutoff | 0–48 st | 0 |
| 35 | `lfopitch` | LFO>Pitch | 0–100 ct (vibrato) | 0 |
| 36 | `wheelpos` | Wheel>Pos | −100…+100% | 0 |

37 parameters. Position and Warp Amt are clamped to 0–100% after all modulation is added. The
LFO is one per instance (not per voice), so chords move together; when synced it locks to the
MPC's song position while playing.

## 5. UI (three pages; each Q-Link bank is a column of four)

**WAVE** (front page: the sound):

| Wavetable | Unison | Play | Out |
|---|---|---|---|
| Table (menu) | Unison | Voice Mode (menu) | Output (fader) |
| Position | Detune | Glide | *OCKM WAVE badge* |
| Warp (menu) | Spread | Octave | |
| Warp Amt | Sub Level | Vel>Amp | |

**FILTER / ENV:**

| Filter | Amp Env | Mod Env | Env To |
|---|---|---|---|
| Filter (LP/BP/HP switch) | Attack | Attack | Env>Pos (bipolar) |
| Cutoff | Decay | Decay | Env>Warp (bipolar) |
| Resonance | Sustain | Sustain | Env>Cutoff (bipolar) |
| Key Track | Release | Release | *badge* |

**LFO / MOD:**

| LFO | LFO To | Wheel | |
|---|---|---|---|
| Shape (menu) | LFO>Pos | Wheel>Pos (bipolar) | *badge* |
| Rate (shown when Sync Off) / Div (shown when Sync On) | LFO>Warp | | |
| Sync (switch) | LFO>Cutoff | | |
| | LFO>Pitch | | |

Every knob gets help text that says, in one line, what it does to the sound. (build) The ui.json format has no help text, so the README's tables carry it instead. Badges sit at WAVE (Out, row 3), FILTER (Env To, row 4) and MOD (Wheel, row 3); LFO Rate and LFO Div each have their own slot, and only one shows at a time.

## 6. Factory presets (24 built-in programs, numbered as lessons)

The MPC's program list shows them in order, so the numbers read like a course. Lessons 01–16
each demonstrate **one** idea (README section "Lessons" explains each in plain English and says
which knob to turn to hear it). 17–24 are ready-to-use house/techno sounds that combine the ideas.
Every preset peaks below 0 dBFS with a 4-note chord at full velocity.

| # | Name | The idea | How it's set up |
|---|---|---|---|
| 01 | Init Saw | One frame = one cycle | Shapes, Position on the saw frame, nothing moving (= all defaults) |
| 02 | Shape Morph | Position morph | Shapes, slow LFO>Pos sweeping sine → square |
| 03 | Wheel Scan | Position by hand | Harmonics, Wheel>Pos +100%: push the mod wheel to add harmonics |
| 04 | Pluck by Table | Position envelope | Harmonics, Env>Pos short decay: brightness falls without a filter |
| 05 | Pluck by Filter | Compare with 04 | Same pluck from Shapes + filter envelope: hear the difference |
| 06 | Table PWM | Pulse table | Pulse, LFO>Pos: pulse-width modulation |
| 07 | Talking Pad | Formant table | Vowels, LFO synced 1/2 on Position |
| 08 | Sync Sweep | Baked hard sync | Sync table, Env>Pos rising sweep |
| 09 | FM Bell | FM frames | FM table, Env>Pos falling: bright strike, mellow tail |
| 10 | Fold Growl | Folding frames | Fold table, Mono, LFO>Pos: growling bass |
| 11 | Step Grit | Quantised frames | Steps table, chiptune-ish lead |
| 12 | Warp Sync | Sync warp | Shapes sine + Sync warp, Env>Warp: sync from a plain sine |
| 13 | Warp Bend | Bend warp | Saw + Bend, LFO>Warp: moving phase-distortion tone |
| 14 | Warp Mirror | Mirror warp | Harmonics + Mirror: hollow organ |
| 15 | Unison Stack | Unison | Saw, 5 copies, wide detune and spread |
| 16 | Aliasing Free | Mipmaps | Bright Sync frame played high: stays clean (README explains why) |
| 17 | House Stab | — | Harmonics, short Env>Pos + filter env, 3 copies |
| 18 | Deep Chord | — | Vowels, low cutoff, soft attack, chord-friendly |
| 19 | Sub Bass | — | Shapes sine/triangle + Sub, Mono |
| 20 | Techno Rumble | — | Fold, low octave, slow LFO>Pos, Mono |
| 21 | Acid-ish Lead | — | Sync table, Legato + glide, resonant LP envelope |
| 22 | Wide Pad | — | Shapes, 5 copies, slow attack, slow LFO>Pos |
| 23 | Digital Arp | — | Steps + Quantize warp, short pluck |
| 24 | Vowel Bass | — | Vowels, Mono, Env>Pos falling |

## 7. Note handling

- Poly: 6 voices with the SDK's `VoiceAllocator` (oldest note stolen, sustain pedal, bend ±2 st).
- Mono: last-note priority; Legato: same, but no envelope retrigger while a note is held, and
  Glide only slides between overlapping notes.
- All-notes-off silences everything within the release time.

## 8. CPU and memory budget (MPC One, 32-bit Cortex-A17)

| Case | Target on the MPC One | Desktop host_sim equivalent (÷20) |
|---|---|---|
| Typical preset, 4-note chord | < 5% of a 128-frame block | < 0.25% |
| Worst case: 6 voices × 5 unison, Sync warp, all modulation | < 15% of a 128-frame block | < 0.75% |

Why it fits: per oscillator copy per sample, one frame-pair read (4 table lookups and a few
multiply-adds, no `sin`/`exp`); worst case 30 copies. Pitch, mip level, filter coefficients
and envelope-driven values update every 16 samples. If the worst case misses the target, the
fallback is to cap total oscillator copies at 16 across all voices (fewer copies for the extra
notes), noted in the README.

Memory: 8 tables × 16 frames × ~5,400 floats ≈ 2.6 MB (build: measured), shared by every instance.

## 9. How it will be tested

1. `build.py` → 0 validator errors (native, arm32, arm64).
2. `host_sim ... test --wav build/test.wav` → no FAIL; every WARN explained.
3. `tests/harness.cpp` (includes the plugin source; ends with `ALL PASS`) checks what this SPEC promises:
   - every preset: 4-note chord at velocity 127 peaks below 0 dBFS, no NaN, no denormal tail;
   - program 0 equals the parameter defaults;
   - aliasing: Shapes saw at C7 has non-harmonic energy at least 60 dB below the fundamental;
   - morph: Shapes position 0% is a near-pure sine (harmonic 2+ below −40 dB), 100% is square-like (odd harmonics only);
   - position sweep: moving Position 0→100% over 1 s gives no clicks (sample-to-sample jumps bounded);
   - Warp Amt 0% is identical to Warp Off, for every mode;
   - unison: with Spread 0 left equals right; with Unison 5 and Spread 100 they differ;
   - voice limit: 8 held notes play 6 voices and steal without clicks;
   - Mono/Legato: overlapping notes don't retrigger in Legato; Glide reaches the target pitch;
   - all-notes-off: silent within 1 s;
   - table build time and worst-case render time, printed and checked against section 8.
4. Demo WAV: the harness renders a short phrase of every preset into `build/demo.wav`.
5. `package.py`, then hand-over with the SD-card steps from HOUSE-STYLE.md.

## 10. Not in v1

- A second wavetable oscillator, a noise source, and oscillator FM/ring between oscillators.
- Loading your own wavetables from `Content/` (the format and a `User` table slot are an obvious v2).
- More than one LFO, a third envelope, a mod matrix, macros.
- Effects (chorus, delay, reverb, distortion): use the MPC's own insert effects.
- A second filter, filter drive, 24 dB or ladder filter types.
- More than 6 voices, MPE, poly aftertouch, an arpeggiator.
- A drawn wavetable display on the screen (the MPC UI format cannot draw live waveforms).

## 11. Open questions for Simon

1. **Voice count.** 6 voices keeps the worst case safe on the MPC One. *Recommendation: 6.*
2. **Tint.** Champagne or bronze? *Recommendation: champagne (a pale gold), so it is clearly
   different from silver OCKM-03 and dark gunmetal OCKM Gate.*
3. **Preset count.** 24 (16 lessons + 8 track-ready sounds). More is easy later, as long as the
   order of the first 24 stays fixed. *Recommendation: 24 for v1.*

## 12. Decisions taken while building (build)

- Parameters, ids, ranges, defaults, pages and the 24 presets are as specified above. uid `O57W`.
- Wavetable levels: level k keeps harmonics 1…512>>k, stored at 4 samples per period of its top
  harmonic (2048 down to 256 samples). Build time 40 ms on a desktop; 2.6 MB.
- Mirror warp is a blend between the normal read and a forwards-then-backwards read (Warp Amt 0%
  = normal), because a moving mirror point produced very steep slopes at low amounts. Quantize
  is bypassed below 0.5% so 0% is identical to Off.
- The warps are not fully alias-free (the harness measures it): at 50% warp the loudest unwanted
  tone is 36–51 dB down at C5 and 25–39 dB down at C7. Warp Off measures 78 dB down at C7. The
  README says so.
- Voices whose amp envelope has decayed to silence with Sustain 0% free themselves even while the
  key is held, so plucks cost no CPU after they fade.
- CPU (harness, desktop): 4-note chord 0.22% (Deep Chord) to 0.31% (Wide Pad, 5 copies); worst
  case 0.70%. Estimated MPC One (×20): about 4.5%, 6% and 14%. Not yet measured on hardware.
- The sub is a sine read from the Shapes table's first frame, so it shares the alias-free tables.
- All sound off (CC 120) fades every voice out in 5 ms; all notes off (CC 123) uses the release.
