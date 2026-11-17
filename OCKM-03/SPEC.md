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

Current version: **v1.3** (Echo; section 12). v1.2 (Mutate, key tracking, low cut; section 11) sounded great on Simon's MPC One. CPU on the MPC One is still to be measured.

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
- Effects: reverb, chorus, EQ, distortion types beyond the one drive; any delay beyond the one-knob Echo (section 12)
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

## 12. v1.3: Echo (built 2026-11-14)

Simon tried v1.2 on the MPC ("sounds great") and asked for "a small delay". He was offered the MPC's own insert delay (no plugin change) or a one-knob echo inside OCKM-03, and chose the knob. This is a deliberate exception to "no effects" in section 9, kept to a single control.

- **Parameter 30, `echo`, "Echo", 0–100 %, default 0** (appended; automatable; Q-Link 16 in Program mode).
- **Time:** a dotted 8th (0.75 beat) at the MPC tempo, 120 BPM when the MPC gives none, tempo limited to 40–300 BPM. The delay glides over 50 ms when the tempo changes. The buffer is 1.2 s, allocated in `prepare()`.
- **Amount:** the wet level (0.45 × Echo) and the feedback (0.15 + 0.5 × Echo, i.e. 15–65 %) rise together and are smoothed over 50 ms.
- **Tone:** everything written into the delay passes a one-pole 250 Hz high-pass and a 3.5 kHz low-pass, so even the first repeat is bass-light and each later one is thinner and darker. The dry signal is untouched.
- **Position:** after the low cut, before Volume and the output ceiling. Mono (output L = R), like the rest of the plugin.
- **Bypass:** at Echo 0 the echo does nothing at all. After the knob returns to 0, it runs until the tail has been below −120 dB for a whole delay time, then clears its buffer and bypasses again, so old echoes never reappear. Values below 1e-15 entering the line are flushed (no denormal tails).
- **UI:** the empty Voice slot (row 4), replacing the name badge.
- **Preset:** Mutant Squelch gets Echo 30. The other presets stay at 0.
- **CPU:** on the desktop harness, Echo at 60 % costs 0.249 % of real time against 0.243 % with Echo off (about +2 %).

Harness checks (all passing):
- Echo 0: silence after the note.
- The first repeat lands a dotted 8th later at 125 BPM and at 100 BPM (within 1,200 samples; the offset is the note's own attack).
- At Echo 100, repeats fall below −60 dB within 5 s.
- Turning Echo off and on again brings back no stale echoes.
- The first repeat's low/high harmonic ratio is under half the dry note's (measured 1.98 against 13.95).
- Every preset with Echo at maximum peaks below 0 dBFS (worst −1.5 dBFS).

host_sim: 19/19, no warnings.

## 13. v1.4: closer to the classic circuit (built 2026-11-14)

Simon asked for four changes taken from a public circuit study of the classic acid bass unit (the GPL-3 "DW-303" project). Only the **behaviour** described in its circuit notes is used; no code is copied, so OCKM-03 stays MIT. No parameters are added, removed or reordered: projects load the same values but sound closer to the original.

1. **Accent sweep tied to Resonance.** An accent produces a voltage that follows the filter envelope (`Accent × env`). In the original circuit this voltage also charges a capacitor through a diode, so the capacitor only charges up. The capacitor charges with a time constant of about 32 ms at Resonance 0 and 50 ms at Resonance 100, and it drains with a time constant of about 100 ms. The accent's push on the cutoff is a blend of the two. At low Resonance it is mostly the direct voltage, a quick blip. At high Resonance it is mostly the capacitor, so the push swells later ("wow") and builds up over a run of accents. The capacitor cannot charge above the direct voltage, so a constant stream of accents (an arp) cannot pin the filter open. This replaces the v1.1 accent accumulator. Full push at Accent 100: 3 octaves.
2. **Accent volume as a punch.** The extra level of an accented note follows the accent voltage above a threshold, and dies away with it: `boost = 1 + 1.6 × max(Accent × env − 0.1, 0) / 0.9`, so up to about +8 dB at the very start. Before, it lasted for the whole note.
3. **Env Mod in both directions.** The envelope rests below the cutoff reference, so turning up Env Mod also lowers the resting cutoff: `envOctaves = EnvMod × 5.5 × (env − 0.3)`. At 100 % the cutoff sweeps from 1.65 octaves below the knob setting to 3.85 octaves above it. Presets are re-balanced for this.
4. **Diode-ladder filter.** The four-pole ladder is replaced by a model of a diode ladder:
   - **Stages:** four coupled capacitor stages, where each stage loads its neighbours. The bottom capacitor is 18/33 the size of the other three.
   - **Saturation:** a tanh at the input transistor pair.
   - **Output:** taken from the top stage, with ×5 gain (unity at DC without the coupling stages).
   - **Coupling stages:** two fixed high-passes inside the resonance loop, as in the circuit. One is about 78 Hz on the output stage, which the output also passes through. The other is about 13 Hz in the feedback path.
   - **Solver:** six states, solved together each sample by zero-delay-feedback trapezoidal integration. The 6×6 matrix is inverted at control rate, every 16 samples. The filter still runs at 2× the host rate.
   - **Displayed cutoff:** the frequency where resonance peaks (the stage conductance is fc / 1.855).
   - **Resonance:** at maximum the loop gain is 90 % of the self-oscillation point at mid and high cutoffs. So it rings hard but never whistles on its own. The in-loop high-passes raise the self-oscillation point when the filter is nearly closed, so resonance there is gentler, as in the original.
   - **Level:** the filter's level loss at high Resonance is partly made up after the filter, so presets stay as loud as in v1.3.
   - **Not modelled:** the saturation of the inner stages.
   - **CPU:** the filter costs more than before, about 55 multiply-adds per oversampled sample.

**Tests to add** (in `tests/harness.cpp`):
- The filter on its own: an impulse at maximum Resonance dies away at five cutoffs. The resonant peak falls within a third of an octave of the displayed cutoff (1 kHz and 3 kHz). It is stable with ±100 inputs. The 78 Hz coupling stage cuts 40 Hz relative to 200 Hz.
- Accent sweep: at Resonance 90 the accented note's brightness peaks later than at Resonance 10, and a run of accents still builds up.
- Accent punch: an accented note's level 250 ms in is closer to an unaccented note's than at its start.
- Env Mod: with Env Mod 100, the late part of a long note is darker than with Env Mod 0 at the same Cutoff.
- All existing checks still pass: the arp knob checks, presets peaking below 0 dBFS with Mutate and Echo at maximum, and host_sim.
- Desktop CPU is compared with v1.3, and the new figure is measured on the MPC One.

### 13.1 As built

- **Presets:** Cutoff raised by 9 on presets 2–5 (Classic 52, Rubber 36, Scream 56, Mutant 49), making up for the darker filter and the lower resting cutoff. Rubber Sub's Volume is now −7 dB. The Init Acid defaults are unchanged. Loudness on the harness pattern is within about 1–2 dB of v1.3.
- **Harness peaks:**
  - demo: Init −3.95, Classic −2.96, Rubber −2.77, Scream −3.06, Mutant −2.53 dBFS
  - worst with Mutate at maximum: −4.35 dBFS
  - worst with Echo at maximum: −1.42 dBFS
- **Measured:**
  - the impulse dies away at every cutoff
  - the peak is 1 semitone below the displayed cutoff at 1 kHz and 3 kHz
  - 40 Hz / 200 Hz is 0.54
  - the accented note is brightest at 10 ms (Resonance 10) and 90 ms (Resonance 90)
  - accent level is 2.64× at the start and 1.62× at 250 ms
  - the late brightness of a held note falls from 0.0128 to 0.0100 with Env Mod 100
- **Existing slide test:** now runs at Cutoff 55. With the new resting cutoff and the 78 Hz coupling stage, the old setting left the filter so nearly closed that the brightness measure could not tell a re-pluck apart.
- **CPU:**
  - desktop: 0.36 % of real time playing 16ths, against 0.37 % for v1.3
  - host_sim: 19/19, average 0.24 %
  - MPC One: still to measure

## 14. v1.5: Pattern generator (approved and built 2026-11-17)

Simon asked for the "generate" idea from the GPL-3 `mpc-vst-acid` plugin, keeping OCKM-03's own Mutate. That plugin makes no sound: it sends MIDI to other tracks through an unofficial route. Here the generator plays **OCKM-03's own voice**, so no MIDI output is needed. Only the behaviour described in its README is used; no code is read or copied, and the generator below is our own design. OCKM-03 stays MIT.

### 14.1 How you use it

1. Turn **Pattern** to **Hold**.
2. Hold a note (a pad, or one long note on the MPC grid). OCKM-03 plays a 16th-note acid line in that key for as long as the note is held. Releasing the note stops the line.
3. A different held note transposes the line, so drawing one long note per bar on the grid writes the key changes.
4. **Pattern #** chooses the line: each number (1–999) is a different line. Turning it is "Generate". The number is saved with the project, so a line always comes back.
5. **Density**, **Accent**, **Slide**, **Range** and **Length** reshape the current line live. Returning a knob to a value gives back exactly the same line.
6. **Mutate** works on generated notes exactly as on played ones (octave jumps, slide-ins and rests on the same steps of the bar).

With Pattern **Off** (the default), OCKM-03 behaves exactly as v1.4. Old projects are unaffected.

**Timing:**
- While the MPC sequence plays, steps sit on the song's 16th grid, using the same step clock as Mutate. Step `s` of the line plays on song step `s mod Length`, counted from the bar, so it stays locked when you loop or move the playhead.
- When the transport is stopped, the line runs from the moment the note is held, at the MPC's tempo (120 BPM if none).

**Why Hold, not free-running:** the line can be started, stopped and transposed from the MPC's own grid, which is the closest we can get to putting it "on the grid". (Plugins can't write notes to the grid.)

### 14.2 The generator (our own design)

- **Repeatable numbers:** for each step 0–31 and each lane (play, accent, slide, pitch, octave), a fixed number comes from an integer hash of (Pattern #, step, lane). We already use this approach in Mutate. The knobs only move thresholds against these numbers, so the line reshapes smoothly and returns exactly.
- **Exact amounts:** as in Mutate, steps are ranked by their number within the first `Length` steps.
  - Density 50 % with Length 16 plays exactly 8 steps.
  - Accent and Slide are shares of all `Length` steps, ranked independently of Density, so changing Density never moves the accents or slides. A slide into a rest becomes a plain note. (The draft said "share of played steps"; this was changed while building it, for that stability.)
  - Turning a knob up only adds steps.
- **Pitch:** the scale is natural minor from the held note. Each played step picks a scale degree from its pitch number, weighted towards the root (about 40 % root, then the 5th, the minor 3rd and the 7th, then the rest), which gives acid-style lines.
  - Range 1, 2 or 3 octaves: the octave number lifts a step up 1 or 2 octaves within that range.
  - Steps with an octave number near the top go up first, so Range 1 → 2 only adds jumps.
- **Gate:** a plain note lasts half a step. A slide note holds into the next step, giving OCKM-03's normal slide with no re-pluck.
- **Accent:** an accented step plays at velocity 127, others at 80, so Accent Thr still decides. At the default of 100, accents are accents.
- **CPU:** a few hashes per step. Nothing is added per sample.

### 14.3 Parameters (appended after 30)

| # | id | Name | Range | Default |
|---|---|---|---|---|
| 31 | `pt_mode` | Pattern | Off / Hold | Off |
| 32 | `pt_seed` | Pattern # | 1–999 | 1 |
| 33 | `pt_den` | Density | 0–100 % | 75 |
| 34 | `pt_acc` | Pat Accent | 0–100 % | 30 |
| 35 | `pt_sld` | Pat Slide | 0–100 % | 20 |
| 36 | `pt_rng` | Range | 1–3 oct | 1 |
| 37 | `pt_len` | Length | 1–32 steps | 16 |
| 38 | `pt_scl` | Scale | Minor / Phrygian / Dorian / Min Pent / Harm Min | Minor |

All are automatable, so knob moves can be recorded into the sequence.

### 14.4 UI

There will be a second page, **PATTERN**, with two blocks of four (columns), each with its own screen-mode Q-Links:
- **Line:** Pattern (switch), Pattern #, Length, Scale
- **Shape:** Density, Accent, Slide, Range

The ACID page is unchanged. Program-mode Q-Links are already all used (1–16), so the Pattern controls work through the screen's Q-Links on the PATTERN page.

### 14.5 Presets

Two more, appended:
- **Acid Machine:** the Classic Squelch sound with Pattern on (Pattern # 7, Density 75, Accent 30, Slide 20).
- **Mutant Machine:** the same with Mutate and a little Echo.

### 14.6 Tests

- **Repeatable:** the same Pattern # gives the same notes, and a different number gives different notes.
- **Exact amounts:** Density 50 % plays exactly 8 of 16 steps. Turning Density, Accent or Slide up only adds steps, and turning one back restores the line exactly.
- **Hold:** with Pattern Off the output is identical to v1.4. With Hold, nothing plays without a held note, and releasing it stops the line with no hanging note.
- **Transpose:** holding C2 instead of C1 raises every note by 12.
- **Transport lock:** step 5 of the line lands on the 6th 16th of the bar, the same in every bar, and Length 12 repeats every 12 steps.
- **Slides:** slide steps do not re-pluck the filter.
- **Mutate:** Add Rest still silences the predicted steps of a generated line.
- **Existing checks:** all still pass, and host_sim has no FAIL.

### 14.7 Not in v1.5

- Swing (the MPC's swing can't reach notes the plugin makes itself)
- Two lines and blend modes, step editing and locks, LFOs, auto-regenerate
- Sending the line to other tracks or hardware as MIDI
- Writing it onto the grid

### 14.8 As built

- **Names:** parameters 34 and 35 are named "Pat Accent" and "Pat Slide", because the screen finds controls by name and "Accent" is already taken.
- **Scale:** a knob with a menu overlay (5 choices). Pattern is a switch captioned OFF / HOLD.
- **Q-Links:** on the PATTERN page, the screen-mode Q-Links follow the two columns. Program-mode Q-Links are unchanged.
- **Timing:** `renderPattern()` steps through the block in order: MIDI first (so a key pressed on a step plays it), then the end of a note's gate, then the next 16th.
  - **On the grid:** song step `k` lands at sample `ceil((k/4 − ppqPos) / (bpm/60/sr))`.
  - **No double plays:** the last song step played is remembered, so a step never plays twice across block edges. That memory is cleared when the playhead jumps back (a loop).
  - **Grace:** a first key pressed up to a quarter step after a 16th plays that 16th at once.
  - **Transport stopped:** the line starts at the key press and steps every `sr·60/bpm/4` samples.
- **Switching:** turning Pattern on or off releases everything.
- **Harness results** (all passing):
  - **Generator:** exact amounts (40 Pattern #s), nesting, and Minor Pent using only its notes.
  - **Repeatable:** the same Pattern # gives the same line.
  - **Off and Hold:** Pattern Off is unaffected by the Pattern knobs; Hold is silent with no key, and a release stops cleanly, even with Slide, Mutate and Range 3.
  - **Grid:** onsets match the prediction over 4 bars (Density 50 % gives 32 notes), and Length 12 repeats every 12.
  - **Transpose:** holding C2 doubles the zero crossings of C1 (168 → 336).
  - **Slides and Mutate:** slide steps don't re-pluck (brightness 0.0278 → 0.0128), and Add Rest silences the predicted steps.
  - **Block size:** block sizes 37, 128 and 512 differ by less than −50 dB (largest 0.001, from per-block knob smoothing, not timing).
  - **Transport stopped:** the line starts on the key press.
- **Peaks:** demo peaks are Acid Machine −2.63 and Mutant Machine −2.76 dBFS. All 7 presets stay below 0 dBFS with Mutate and with Echo at maximum.
- **host_sim:** 19/19, 39 parameters round-trip, average 0.22 %.

## 15. v1.6: Grit (approved and built 2026-11-17)

Simon found v1.4 "a little too smooth". A second circuit-model project, the AGPL-3 `mpc-vst-acid303`, confirmed the v1.4 changes. It also showed what v1.4 had simplified: every transistor pair in the filter saturates, not only the input pair. Only the published behaviour is used (that project's README, and the public manual of the classic after-market mods); no code is read or copied.

1. **Filter correction (a v1.4 bug).** In the circuit, the input pair feeds a **current** into the bottom stage, which does not depend on the bottom stage's voltage. v1.4 modelled it as a conductance from the input voltage instead (`v0' = g·c0·(u − 2v0 + v1)`). That added damping to the bottom stage and needed a ×5 output gain.
   - **Fix:** `v0' = g·c0·(u − (v0 − v1))`, with unity output gain.
   - **Check:** with this fix the analysis gives a self-oscillation loop gain of about 17. That matches the circuit study's own figure of "≈ 17", which v1.4 did not.
   - **New constants:** resonant peak / stage conductance 1.39 (at mid and high cutoffs), Resonance 100 = 0.9 × 17.2.
2. **Inner-stage saturation.** Each rung current is `g·tanh(Δv)`, and the top termination is `g·tanh(v3)`, in units where the input pair is `g·tanh(x)` (the circuit's own scaling: everything is relative to 2·VT).
   - **Method:** the linear zero-delay solve predicts the states. Then one correction step replaces the linear rung currents with the tanh ones.
   - **Stability:** at the very top of the cutoff range the correction is scaled down so the step stays stable.
   - **Cost:** 4 more rational tanh per oversampled sample.
3. **Overdrive** (new parameter 39, `odrive`, 0–100 %, default 0). The oscillator level into the filter's input pair is ×1 at 0 and ×66.6 at 100, following the square of the knob. This is the classic mod's range. High settings square up the wave inside the filter and push the inner stages into saturation.
4. **Muffler** (new parameter 40, `muffle`, 0–100 %, default 0). Soft clipping of the amp's output, before Drive. It rounds off the loud start of notes and the accent peaks, for a fuzzier, rounder buzz:
   - the threshold falls from 0.5 to 0.1 as the knob rises
   - the clipped signal is fully mixed in by 25 %
   - make-up gain is ×1 to ×2.2
5. **Full Bass** (new parameter 41, `fullbs`, Off / On, default Off). This moves the filter's in-loop output coupling from 78 Hz to 16 Hz, so 32 Hz is down about 1 dB instead of 5 (the classic mod's "fuller bass"). Off keeps the authentic thin low end.
6. **Soft attack.** Normal notes now start over 3 ms, as on the original. Accented notes start over 0.5 ms, so accents punch through. Before, every note started over 1 ms.

**UI:** a third page, **GRIT**, with one block of four: Overdrive, Muffler, Full Bass (switch OFF / ON), and an empty slot. The ACID and PATTERN pages are unchanged.

**Presets:** existing presets keep the new knobs at 0 / Off, but they sound grittier because of the stage saturation, and they are re-levelled to v1.5 loudness. One more preset is appended: **Grit Squelch** (Overdrive, a little Muffler, Full Bass on).

**Tests:**
- The filter's DC gain is 1 at Resonance 0, with the coupling stages excluded.
- At Resonance 100 an impulse still dies away, now including saturation, at 100 Hz – 8 kHz, with and without Full Bass.
- The filter stays bounded with ±100 input and Overdrive 100 at maximum cutoff.
- Stage saturation adds harmonics: a loud sine through the filter has more 3rd harmonic than a quiet one, relative to its fundamental.
- Overdrive raises harmonic content.
- Muffler lowers the crest factor.
- Full Bass raises 40 Hz against 200 Hz.
- Normal notes start more slowly than accented ones.
- Every preset peaks below 0 dBFS with Overdrive and Muffler at maximum.
- All existing checks still pass, and host_sim has no FAIL.

### 15.1 As built

- **Levels:**
  - Make-up gain after the filter is 2.0 × (1 + 1.2 × Resonance), which puts the presets within 0.3 dB of v1.5's loudness.
  - Overdrive raises the filter's output by about 9 dB (Resonance 0) to 20 dB (Resonance 100), because a clipped input swamps the resonance's level loss. Most of it is taken back with `10^(−(8.7 + 11.5·√res)·(1 − 1/drive)/20)`, so Overdrive changes the tone, not the volume, within about 4 dB.
- **Grit Squelch:** Cutoff 46, Resonance 78, Env Mod 65, Decay 45, Accent 70, Overdrive 45, Muffler 20, Full Bass on, Volume −4 dB.
- **Measured:**
  - passband gain at Resonance 0: 0.96
  - 3rd harmonic / fundamental: 0.0002 quiet, 0.091 loud
  - even / odd harmonics: 1.26 at Overdrive 0, 0.86 at Overdrive 60
  - 40 Hz / 200 Hz: 0.57 with Full Bass off, 1.09 on
  - crest factor: 5.43 at Muffler 0, 2.84 at Muffler 100
  - level in the first 1.4 ms: 0.16 for a normal note, 0.38 for an accented one
  - worst preset peak with Overdrive, Muffler and Full Bass at maximum: −9.1 dBFS
  - demo peaks: −1.2 to −4.4 dBFS
- **CPU:**
  - desktop, playing 16ths: 0.42 % of real time against 0.30–0.33 % for v1.5 (about +30 %, from the 4 extra tanh per oversampled sample)
  - host_sim: 19/19, 42 parameters round-trip, average 0.25 %
  - MPC One: still to measure
