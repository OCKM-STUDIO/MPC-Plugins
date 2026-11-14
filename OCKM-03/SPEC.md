# OCKM-03 — specification (v1)

> **Governing rule: keep it lean. OCKM-03 does one job: squelchy, slidey acid basslines.** In Simon's words: "make sure it's lean - it's just there to do one job!"
>
> - One oscillator only. No extra oscillators, LFOs, effects chains, mod matrix or arpeggiator.
> - Drive stays, because it is part of the acid sound.
> - The parameter count stays small enough that the front page and its Q-Links cover nearly everything you play; the back page only holds the macro recipes.
> - Presets are few and good.
> - Anything that does not directly serve acid basslines goes in section 9 ("Not in v1"), not into the spec.
>
> When a decision in this document is unclear, this rule wins.

Status: **built; current version v1.2** (see the line below and section 11). v1.0 was built and tried on Simon's MPC One with the MPC arp (2026-11-14). It worked, but on Classic Squelch the controls had almost no effect. v1.1 returns to the classic controls and makes the voice work under an arp; see section 0. CPU on the MPC One is still to be measured.

Current version: **v1.2** (Mutate, key tracking, low cut; section 11). Not yet tested on the MPC.

## 0. v1.1 revision (after the first hardware test)

Measured cause (simulated 16th-note arp, every note at velocity 127, Classic Squelch):

- Every note was an accent. Accented notes used a fixed 150 ms filter decay, so **Decay did nothing**, and the accent build-up (cap 2.5) never drained between 16ths, so the filter stayed pushed open. With Env Mod's 7-octave range on top, the top half of Cutoff and Env Mod barely changed the sound.
- The Classic Squelch macro recipe pushed Resonance and Env Mod to the limit by **half** Squelch travel, after which Squelch, Resonance and Env Mod all stopped responding.
- Drive was fully on by 25% of its travel.

Changes, decided with Simon ("go back to the classic controls and make it work well for an arp"):

1. **Macros retired.** Parameters 11–25 stay registered with the same ids and order (identity is permanent), but they are marked not automatable, are on no page or Q-Link, and are **ignored by the DSP**. A project saved with v1.0 and a macro turned up therefore sounds like its base knobs. Sections 4 and 6 below describe v1.0 and are kept for the record.
2. **Accent decay follows Decay:** accented notes decay over min(Decay, 0.3·Decay + 120 ms), i.e. 50 ms to about 0.9 s, instead of a fixed 150 ms.
3. **Accent build-up:** cap 1.6 (was 2.5), drain time 0.25·Decay + 60 ms (was 0.5·Decay + 150 ms), push 1.0 octave per unit (was 1.4); direct accent push 1.2 octaves (was 1.5).
4. **Env Mod range:** 5.5 octaves at 100% (was 7).
5. **Drive:** gain into the clipper 1 + 11·d² (was 1 + 15·d), wet mix reaches 100% at d = 50% (was 25%).
6. **UI:** one page, three framed columns (Filter / Voice / Tone) plus the name badge; Program-mode Q-Links 12–16 unassigned. The Wave switch shows SAW / SQUARE printed under its buttons, and its on-screen captions are set to always show (they were set to hide when the data model is invalid, and Simon saw no captions on the MPC).
7. Version 1.1.0 (`plugin.json`, `PluginInfo.version` 1100). uid unchanged.

Harness checks added: retired parameters at any value render bit-identical to defaults; only the 11 classic parameters are automatable; under a velocity-127 arp, Decay (0 vs 100), Env Mod (25 vs 75) and Cutoff (25 vs 50) each change the post-attack brightness by at least 1.5–1.8×, and Drive's crest factor keeps falling from 25% to 50% to 100%.

OCKM-03 is a monophonic acid bass instrument, played from the MPC's own sequencer. Naming rule: no other companies' product or brand names anywhere in the code, UI, presets, README or this SPEC (see ../HOUSE-STYLE.md). Describe the sound as "acid bass".

| Item | Value |
|---|---|
| Plugin name | `OCKM-03` (permanent; working title was "MPC03") |
| Manufacturer | `OCKM` (Simon's artist name; permanent) |
| Output folder | `OCKM - VST - OCKM-03/` |
| Distribution | public release from the OCKM STUDIO repo (zip from `tools/package.py`) |
| Licence | MIT (`"license": "MIT"` in plugin.json; a LICENSE file in the OCKM STUDIO repo) |
| Kind | instrument, 0 in / 2 out |
| Project folder | `OCKM-03/` in the MPC-Plugins repo |
| Target | MPC One (32-bit ARM) first; the arm64 build comes for free |
| CPU budget | well under 15% of one MPC One core per instance; aim for under 8% |

## 1. Architecture: keep the voice and the note handling apart

A step sequencer may be added later, so the code is split into three parts that only talk through small interfaces:

1. **NoteInput**: turns MIDI into musical events. It keeps a note stack (last-note priority) and decides, for each event, `pitch`, `slide` (yes/no) and `accent` (yes/no). It knows nothing about DSP.
2. **AcidVoice**: the sound engine. It gets three calls and nothing else: `noteOn(pitch, accent, slide)`, `noteOff()` and `setTargets(...)`. It knows nothing about MIDI.
3. **Plugin**: owns the parameters, works out the macro offsets (section 4) and passes the resulting effective values to the voice once per block, smoothed.

A later internal sequencer would be a second source of `noteOn/noteOff` events alongside NoteInput, with no change to AcidVoice.

## 2. Note handling (from the MPC sequencer)

- **Mono, last-note priority**, with a note stack of up to 16 held notes.
- **Slide**: if a new note-on arrives while another note is still held (overlapping notes in the MPC's sequence), the voice glides to the new pitch over `Slide Time` and does **not** retrigger the envelopes. Without overlap, the note starts fresh: no glide, and the envelopes retrigger.
- **Release of the newest note while an older one is still held**: return to the older note with a slide and no retrigger (see open question 4).
- **Accent**: a note is accented when its velocity ≥ `Accent Threshold` (parameter, default 100). Velocity has no other effect, matching the original (see open question 5). An accent:
  - boosts the amp level (scaled by `Accent`);
  - opens the filter cutoff further (scaled by `Accent`);
  - adds an extra filter-envelope push that **builds up over consecutive accented notes**. This is modelled as an accent accumulator (the original's capacitor). Each accented note charges it, and it discharges with a time constant tied to the filter decay. Close accents therefore stack into the rising "wow" sweep, and spaced-out accents don't.
  - For a slid note, the accent is taken from the new note's velocity, but the envelopes are not retriggered.
- **Gate**: note-off closes the gate. The amp envelope then releases quickly (fixed at about 8 ms, to avoid clicks); there is no release knob.
- Pitch bend, mod wheel and other controllers are ignored.
- All-notes-off / sustain: clear the note stack and release.

## 3. Voice (DSP)

Signal flow: **Oscillator → Filter → Amp (VCA) → Drive → Volume**

- **Oscillator**: one oscillator with a Saw/Square switch, band-limited with PolyBLEP (`framework/dsp`). The square is a slightly asymmetric pulse with a little low-pass shaping, rather than a perfect square, for the hollow, reedy character. `Tuning` is ±12 semitones (fine, in cents, inside that range). Glide moves exponentially toward the target pitch in the log-frequency domain.
- **Filter (the main DSP effort)**: a 4-pole resonant low-pass with a diode-ladder character, written as a zero-delay-feedback (TPT) model.
  - Gentle saturation inside the feedback loop, using a cheap polynomial or rational tanh approximation (no `tanhf` per sample).
  - Resonance goes up to just below self-oscillation. At high resonance the bass stays present, but thinner, as on the original.
  - Run the filter at 2× oversampling if the CPU budget allows; measure on the simulator and fall back to 1× with a pre-warped cutoff if it doesn't. The decision is recorded in the README.
  - Cutoff modulation goes through a log-frequency sum: `cutoff + envMod·env + accentPush + accentAccum`. Clamp to 20 Hz … 0.45·fs.
- **Filter envelope**: an instant attack, then an exponential decay set by `Decay` (about 200 ms to 2.5 s). On accented notes the decay is forced short (about 200 ms), as on the original, and the accent push adds on top.
- **Amp envelope**: an instant attack, a long fixed decay (about 3 s) while the gate is held, and the fixed fast release from section 2.
- **Drive**: a soft-clip waveshaper after the VCA, with gain compensation so turning it up doesn't just get louder. 0 = clean.
- **Denormals and smoothing**: the framework handles denormals. Every continuous parameter is smoothed for about 10 ms, so macro moves are click-free.
- **CPU**: no per-sample `exp`/`pow`/`tanh`. Compute cutoff coefficients once per 16-sample sub-block, or use a table or approximation.

## 4. Macros (v1.0 only; retired in v1.1, see section 0)

A Q-Link controls exactly one parameter, so each macro is a plugin parameter that moves several targets inside the plugin.

**Rule: macros add offsets and never overwrite.**

```
effective(target) = clamp( base(target) + Σ over macros [ macroValue(m) × depth(m, target) ], 0, 1 )
```

- All values are normalised (0–1 = the knob's full travel). `macroValue` runs 0…1, and `depth` runs −100%…+100% (i.e. −1…+1).
- Base knobs are never written by a macro. Turning every macro back to 0 gives exactly the base sound.
- The on-screen base knobs show the base value, not the effective value.
- Effective values are computed once per block, then smoothed.

### The four macros and their default recipes

| Macro | Target 1 | Target 2 | Target 3 |
|---|---|---|---|
| **Squelch** | Cutoff +40% | Resonance +60% | Env Mod +50% |
| **Bite** | Accent +50% | Decay −40% | Drive +20% |
| **Rubber** | Slide Time +50% | Decay +40% | Cutoff −15% |
| **Grit** | Drive +60% | Resonance −20% | — |

The Squelch row is Simon's worked example: at full Squelch, cutoff +40%, resonance +60% and env mod +50%, scaling linearly from 0. The other default depths are starting values to tune by ear.

### Macro recipes live in presets

Each macro/target cell above is its own **depth parameter** (bipolar, −100%…+100%), giving 11 depth parameters. They are ordinary parameters, so they are saved in the project state chunk, in built-in programs and in any preset the user saves on the MPC. Different presets can therefore carry differently tuned macros (a gentle Squelch on one, a vicious one on another).

## 5. Parameters (order and ids are permanent once built)

Parameter 0 is Volume: the host nudges parameter 0 on every load, so it must be harmless. Append new parameters at the end only.

| # | id | Name | Range / choices | Default |
|---|---|---|---|---|
| 0 | `volume` | Volume | −60…+6 dB | −6 dB |
| 1 | `wave` | Wave | Saw / Square | Saw |
| 2 | `cutoff` | Cutoff | 0–100% (log, about 40 Hz…8 kHz) | 30% |
| 3 | `reso` | Resonance | 0–100% | 60% |
| 4 | `envmod` | Env Mod | 0–100% | 50% |
| 5 | `decay` | Decay | 0–100% (about 200 ms…2.5 s) | 35% |
| 6 | `accent` | Accent | 0–100% | 50% |
| 7 | `tune` | Tuning | −12…+12 st (cents) | 0 |
| 8 | `slide` | Slide Time | 0–100% (about 10…300 ms) | 20% (about 60 ms) |
| 9 | `drive` | Drive | 0–100% | 0% |
| 10 | `accthr` | Accent Thr | velocity 1–127 | 100 |
| 11 | `m_squelch` | Squelch | 0–100% | 0 |
| 12 | `m_bite` | Bite | 0–100% | 0 |
| 13 | `m_rubber` | Rubber | 0–100% | 0 |
| 14 | `m_grit` | Grit | 0–100% | 0 |
| 15 | `sq_cut` | Sq>Cutoff | −100…+100% | +40 |
| 16 | `sq_res` | Sq>Reso | −100…+100% | +60 |
| 17 | `sq_env` | Sq>EnvMod | −100…+100% | +50 |
| 18 | `bt_acc` | Bt>Accent | −100…+100% | +50 |
| 19 | `bt_dec` | Bt>Decay | −100…+100% | −40 |
| 20 | `bt_drv` | Bt>Drive | −100…+100% | +20 |
| 21 | `rb_sld` | Rb>Slide | −100…+100% | +50 |
| 22 | `rb_dec` | Rb>Decay | −100…+100% | +40 |
| 23 | `rb_cut` | Rb>Cutoff | −100…+100% | −15 |
| 24 | `gr_drv` | Gr>Drive | −100…+100% | +60 |
| 25 | `gr_res` | Gr>Reso | −100…+100% | −20 |

That makes 26 parameters. Macro targets are fixed in v1; only their depths are editable.

**v1.1:** parameters 11–25 are retired: kept for project compatibility, not automatable, not shown, no effect. The live parameters are 0–10.

## 6. UI (`ui.json`, UI kit widgets, blocks of four)

**v1.1 layout (current):** one page "ACID".

| Filter (col 1) | Voice (col 2) | Tone (col 3) | (col 4) |
|---|---|---|---|
| Cutoff | Accent | Wave (switch, SAW / SQUARE printed below) | name badge |
| Resonance | Accent Thr | Tuning (bipolar) | |
| Env Mod | Slide Time | Drive | |
| Decay | *empty* | Volume (fader) | |

The v1.0 layout below is kept for the record.

The MPC One has 4 Q-Link knobs, which reach one block of four at a time; the screen shows a 4 × 4 grid per page.

**Page 1 "ACID"** (columns = Q-Link blocks):

| Filter (col 1) | Voice (col 2) | Tone (col 3) | Macros (col 4) |
|---|---|---|---|
| Cutoff (knob) | Accent (knob) | Wave (switch) | Squelch (knob) |
| Resonance (knob) | Decay (knob) | Drive (knob) | Bite (knob) |
| Env Mod (knob) | Slide Time (knob) | Tuning (knob, bipolar) | Rubber (knob) |
| Accent Thr (knob) | *empty* | Volume (fader) | Grit (knob) |

Cutoff and Resonance sit in the top-left, where the eye and hand go first, and the macros take a full column, so one Q-Link bank plays all four. Every sound and play parameter is on this page (15 of 26); only the macro depths live elsewhere.

**Page 2 "MACROS"** (one column per macro, bipolar knobs):

| Squelch | Bite | Rubber | Grit |
|---|---|---|---|
| Sq>Cutoff | Bt>Accent | Rb>Slide | Gr>Drive |
| Sq>Reso | Bt>Decay | Rb>Decay | Gr>Reso |
| Sq>EnvMod | Bt>Drive | Rb>Cutoff | *empty* |
| *text: "SQUELCH"* | *text: "BITE"* | *text: "RUBBER"* | *text: "GRIT"* |

There are only these two pages.

## 7. Factory presets (built-in programs, 4)

**v1.1:** the presets keep their base-sound values; their macro recipes are gone (the retired parameters stay at their defaults). Peaks under the harness demo pattern: Init Acid −7.3, Classic Squelch −6.9, Rubber Sub −5.3, Acid Scream −4.9 dBFS. The v1.0 text follows.

Each preset sets the base sound **and** its own macro recipe; all macros are at 0 on load (see open question 3).

1. **Init Acid**: neutral starting point with the default recipes.
2. **Classic Squelch**: saw, mid cutoff, high resonance; a strong Squelch recipe (cutoff +50, reso +70, env +60).
3. **Rubber Sub**: square, low cutoff, long slide; Rubber tuned deep (slide +80, decay +60, cutoff −30).
4. **Acid Scream**: high resonance, some drive, short decay. Bite is aggressive (accent +80, decay −60, drive +40), and Grit pulls resonance −40 while it pushes drive, to tame the scream.

## 8. Test plan (before handing over)

- `python3 $SDK/tools/build.py OCKM-03`: clean build with 0 validator errors.
- `.sdk-tools/host_sim … test --notes 36,36,48,39 --wav …`: no FAIL; the WARNs are understood.
- Extra checks in the simulator or a small harness:
  - overlapping notes slide without retriggering;
  - consecutive accents measurably raise the filter peak;
  - every macro at 0 renders bit-identical to a no-macro render;
  - the CPU figure is reported on the arm32 estimate.
- Then the MPC One by SD card. Copy the folder to `VST2 Plugins/`; this needs MPC OS 3.10.1+ and Preferences → Enhancements (Beta) → Enable Enhancements + External Plugin Support. Check the CPU meter with one instance while a 16th-note sequence plays.

## 9. Not in v1

These items are left out on purpose, under the governing rule. Some may come later; none belong in v1.

- Macros (built in v1.0, retired in v1.1 after the hardware test)
- Internal step sequencer (the code is structured for it; see section 1)
- Extra oscillators, sub-oscillator, noise
- LFOs, mod matrix, arpeggiator
- Effects: delay, reverb, chorus, EQ, distortion types beyond the one drive
- Editable macro target assignments, or more than four macros
- Polyphony and unison
- Velocity sensitivity beyond accent
- Pitch bend and mod wheel
- A release knob or an amp-envelope page
- More factory presets

## 10. Open questions

1. ~~Manufacturer name~~ **Resolved:** `OCKM`, released from the OCKM STUDIO repo.
2. ~~"MPC" in the plugin name~~ **Resolved:** renamed from the working title "MPC03" to `OCKM-03` before any build, so the public plugin does not borrow Akai's product name. Class name `OCKM03`, file slug `ockm-03`.
3. **Do presets store macro positions, or always load with macros at 0?** *Recommendation: macro positions are ordinary parameters, so they are saved, but every factory preset ships with all macros at 0, so a preset always opens on its base sound.*
4. **Releasing the newer of two held notes**: slide back to the older note, or go silent? *Recommendation: slide back without retriggering (classic mono-synth behaviour); a step-entry sequence on the MPC rarely leaves notes held anyway.*
5. **Velocity below the accent threshold**: ignore it (as the original does), or scale the level slightly? *Recommendation: ignore it in v1, so accents stay the only dynamic and patterns sound authentic.*
6. ~~Licence for the public release~~ **Resolved:** MIT, matching the SDK. Any DSP code borrowed from elsewhere must be MIT-compatible and credited in the README.

## 11. v1.2: Mutate, key tracking, low cut (built 2026-11-14)

Simon approved Mutate and asked for suggestions 2, 3 and 4 (key tracking, Add Rest, low cut) to go in from the start. The open questions were settled on the recommendations: octave jumps up only; a 1-bar repeat; slide-in without a filter re-pluck; a fifth preset; version 1.2.0. Add Rest replaced the proposed Add Accent (accents can already be set by velocity; gaps could not).

### 11.1 What it does

Mutate changes the notes OCKM-03 **plays**, not the notes on the MPC's grid. Each incoming note can, depending on its step in the bar, jump up an octave, slide in from the previous note, or be left out (a rest). With all three amounts at 0, OCKM-03 plays exactly what is programmed, and Variation makes no difference.

**Why it is inside OCKM-03 and not a separate plugin:** on the MPC, effect plugins receive no MIDI, and no plugin can send MIDI on to another plugin or back to the MPC.

**Lean check:** it serves acid basslines only. Four parameters, one Q-Link column, no new pages, a few dozen integer hashes per note-on and nothing per sample. It replaces the built-in sequencer idea (parked; see section 9).

### 11.2 Where it sits

`NoteInput` → **`Mutator`** → `AcidVoice` (section 1). The plugin works out the note's step, asks `Mutator::decide(step, variation, octave, slide, rest)` for a `Mutation {rest, octave, slide}`, and passes it to `NoteInput::noteOn`. `NoteInput` remembers each held key's **played** pitch (after an octave jump), so note-offs and the slide back to an older held note use the right pitch. `AcidVoice` gained one entry point, `slideIn()`.

### 11.3 Repeatable, exact, nested

- **Step:** the note's position since the bar start, rounded to the nearest 16th: `notePpq = ppqPos + sampleOffset × bpm / 60 / sampleRate`; `step = round(((notePpq − barStartPpq) mod beatsPerBar) × 4) mod stepsPerBar`, with `beatsPerBar` from the time signature (4/4 gives 16 steps). A 1-bar loop gets the same mutations on every pass.
- **Order:** for each Variation (1–64) and each lane (octave, slide, rest), the 16 steps are shuffled into a fixed order by an integer hash (`hash(step, variation, lane)`, then ranked among the 16 steps). An amount of A mutates the steps whose rank r satisfies `(r + 0.5) / 16 < A`.
- **So the amounts are exact:** 25% is always 4 of 16 steps and 50% always 8, for any Variation. (A first version compared each hash to the amount directly. It was repeatable, but a "50%" could mutate as few as 3 of 16 steps on some Variations, which feels wrong on a knob.)
- **Nested:** turning an amount up only adds steps; turning it down removes them in reverse order.
- **No playhead** (transport stopped or `ppqValid == false`, e.g. playing pads by hand): step = a note counter mod 16 that resets on transport start and all-notes-off.
- Bars with more than 16 steps (e.g. 7/4) reuse the order of `step mod 16`.
- **Hardware check still to do:** confirm that the MPC One reports `ppqPos` and `barStartPpq` while a sequence plays (listen for a repeating pattern with Add Rest at 40%). If `barStartPpq` is not provided it defaults to 0, and the modulo by `beatsPerBar` still lines steps up with the bar from song start.

### 11.4 The three mutations

1. **Oct Jump:** the played pitch is +12 semitones (unchanged if that would pass MIDI 127).
2. **Add Slide:** the note glides from the previous played pitch over `Slide Time`. If the previous note is still held, this is the normal slide. If it has been released, the amp re-opens but the **filter envelope is not re-plucked**. It only slides in when the previous played note started less than one bar earlier (2 s at 120 BPM in 4/4); otherwise the note plays normally.
3. **Add Rest:** the note is not played at all, and its note-off is ignored. Rests are decided first, so a rested step never also jumps or slides.

### 11.5 Parameters (appended; 0–25 unchanged)

| # | id | Name | Range | Default |
|---|---|---|---|---|
| 26 | `mu_oct` | Oct Jump | 0–100 % | 0 |
| 27 | `mu_sld` | Add Slide | 0–100 % | 0 |
| 28 | `mu_rst` | Add Rest | 0–100 % | 0 |
| 29 | `mu_var` | Variation | 1–64 | 1 |

All four are automatable, so Q-Link moves can be recorded into the MPC sequence. That makes 30 parameters, 15 of them live (0–10, 26–29).

### 11.6 Key tracking and low cut (no new parameters)

- **Key tracking:** the filter cutoff moves 0.5 octave per octave played, measured from C1 (MIDI 36) and following the glided pitch, limited to −3…+4 octaves. Octave-jumped and higher notes keep their brightness instead of being choked by the filter. Notes at C1 sound exactly as before; notes below C1 are a little darker than in v1.1, and notes above are brighter.
- **Low cut:** a fixed 30 Hz, 12 dB/octave high-pass (SDK `Biquad`, Q 0.707) after the drive, so sub-30 Hz rumble doesn't eat the kick's room or the master's headroom. It raises waveform peaks by about 3 dB at the same loudness (the phase shift reshapes the wave). Rubber Sub and Acid Scream were therefore turned down 3 dB (Volume −8 and −5 dB). Its state is cleared once the voice is silent and its tail is below −140 dB, so it never decays into denormals.

### 11.7 UI, Q-Links and presets

| Filter | Voice | Tone | Mutate |
|---|---|---|---|
| Cutoff | Accent | Wave (SAW / SQUARE) | Oct Jump |
| Resonance | Accent Thr | Tuning | Add Slide |
| Env Mod | Slide Time | Drive | Add Rest |
| Decay | *name badge* | Volume | Variation |

- Program-mode Q-Links: 1–11 → parameters 0–10, 12–15 → 26–29, 16 unassigned.
- Presets: the four existing presets keep Mutate at 0. A fifth, **Mutant Squelch** (Cutoff 40, Resonance 82, Env Mod 62, Decay 50, Accent 65, Oct Jump 25, Add Slide 30, Add Rest 20, Variation 7), is appended.
- Harness-demo peaks: Init Acid −4.3, Classic Squelch −4.2, Rubber Sub −4.1, Acid Scream −4.2, Mutant Squelch −4.0 dBFS. The worst preset with every Mutate amount at maximum peaks at −5.3 dBFS.

### 11.8 Tests (in `tests/harness.cpp`, all passing)

- Amounts nested (30% ⊂ 60%, every Variation and lane); a different Variation gives a different set; amounts exact (25% → 4, 50% → 8 of 16, Variations 1–8).
- Mutate at 0: output identical whatever the Variation.
- With a running MPC transport over 4 bars: Add Rest 40% rests the same steps in every bar, matching the Mutator's prediction from the playhead step; Oct Jump 50% jumps the predicted steps (pitch measured by zero crossings), the same in every bar.
- Oct Jump 100% with overlapping slid notes: every note-off still ends its jumped note.
- Add Slide after a released note: no filter re-pluck (brightness), and the pitch glides.
- Key tracking: at Cutoff 20%, C3 is better than −30 dB relative to C1 (measured −15.6 dB; untracked it would be around −45 dB).
- Low cut: a 16 Hz saw fundamental is reduced relative to its 33 Hz harmonic (0.81, against 2.0 for a plain saw).
- Every preset with Mutate at maximum peaks below 0 dBFS.
- host_sim: 19/19, 30 parameters round-trip, no denormals.
