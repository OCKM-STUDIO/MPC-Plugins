# OCKM BSS: specification (v1)

> **Governing rule: keep it lean. OCKM BSS does one job: a warm, round, soft bass whose filter
> you can really open up.** It is deliberately the opposite of OCKM-03: no squelch, no accent,
> no acid. At rest it is smooth and rounded; the Cutoff knob takes it from dark and muffled to
> bright and buzzy. Anything that does not serve that goes in section 9 ("Not in v1").
>
> **Two-knob rule (Simon):** most of the variety comes from **Cutoff + Decay**, and together they
> take the sound **from almost a kick drum (both down) to a full-on growl (both up)**. Every other
> knob is for setting up the character once; presets are mostly different spots on the
> Cutoff × Decay map. See section 3a.

Status: **built (v1.0.0), 2026-11-14.** Build 0 errors / 0 warnings, host_sim 19/19, harness ALL
PASS. Not yet tried on the MPC One. Section 12 lists what the build changed from this draft.

| Item | Value |
|---|---|
| Plugin name | `OCKM BSS` (permanent) |
| Manufacturer | `OCKM` |
| uid | `OgyT` (plugin.json and `MPC_FOURCC`; permanent) |
| Class / slug | `OCKMBSS` / `ockm-bss` |
| Output folder | `OCKM - VST - OCKM BSS/` |
| Kind | instrument, 0 in / 2 out (the bass is mono and centred; only the echoes are stereo) |
| Licence | MIT, Copyright (c) OCKM |
| Target | MPC One (32-bit ARM) first; arm64 comes for free |
| CPU budget | under 5% of one MPC One core per instance; hard ceiling 8% (section 8) |
| Skin tint | **olive** (new: a dull green anodised metal; the other four tints are taken) |

## 1. Voices: mono, with optional glide

**Decision: monophonic, last-note priority, one voice.** Reasons:

- A bassline is one note at a time. Mono keeps the low end clean (no two sub notes beating
  against each other when a release overlaps the next note).
- Glide only makes sense in mono.
- CPU is constant and small: one voice is always the worst case.

Note handling:

- Note stack of up to 16 held keys, last-note priority. Releasing the newest note while an older
  one is held goes back to the older note.
- **Glide** (0–100%, about 0 … 500 ms). At 0% there is no glide and **every note re-plucks** the
  envelopes, even if notes overlap in the MPC sequence (so accidental overlaps never swallow a
  pluck). Above 0%, a note that **overlaps** the previous one glides to its pitch and does **not**
  retrigger the envelopes (legato). A note that doesn't overlap starts fresh, without glide. So in
  the MPC sequence, lengthen a note into the next one to make it slide.
- Glide is exponential in pitch (constant time whatever the interval).
- Velocity: see the filter (section 3) and amp (section 4).
- Pitch bend, mod wheel and other controllers are ignored. All-notes-off clears the stack and
  releases.

## 2. Oscillators: the round core

Three sources, summed, all mono, all following the glided pitch:

1. **Core**: a rounded sine (the SDK's parabolic sine, which already carries a trace of odd
   harmonics, so it reads a little like a softened triangle). Always on: it is the body of the
   sound.
2. **Sub**: a sine one octave below, phase-locked to the core (derived from the same phase, so it
   never drifts or cancels). Level 0–100%.
3. **Saw**: a band-limited (PolyBLEP) saw at the played pitch. Level 0–100%. This is the "touch of
   saw" that gives the filter something to open up into: with Saw at 0 the sound stays soft even
   with the filter fully open.
   - **Detune** (0–100% = 0 … 20 cents sharp) detunes only the saw against the core, for slow
     movement. At 0% the saw is phase-locked to the core.

The mix is level-compensated (more Sub or Saw doesn't just get louder) so presets stay within a
few dB of each other.

## 3. Filter: the main feature

Signal flow: **Core + Sub + Saw → Filter → Drive → Amp → DC/rumble cut → (+ Echo) → Volume → safety limit**

- **24 dB/octave resonant low-pass**, a zero-delay-feedback (TPT) four-pole ladder. It stays
  stable when its cutoff is swept fast, which is what the Cutoff Q-Link and the envelope do.
- **Cutoff knob law**: exponential, 0% ≈ 40 Hz (just the fundamental: muffled, round) to
  100% ≈ 10 kHz (bright, buzzy; see section 12). Each 10% of travel is about 0.8 octave, so every part of the
  knob's travel changes the sound by the same musical amount (no dead top half, no jumpy bottom).
- **Key tracking** (fixed, no knob): the cutoff follows the played note at half an octave per
  octave, measured from C1, so high notes keep their brightness and low notes don't get fizzy.
- **Resonance** 0–100%: from none to a strong, singing peak just short of self-oscillation.
  It **stays stable at 100%** (never runs away or blows up, whatever Cutoff, envelope or drive do).
  Gentle saturation inside the feedback loop (cheap polynomial, no `tanh` call) rounds the peak
  off musically. Low-end compensation: a 4-pole filter normally thins the bass as resonance rises;
  the input is boosted as resonance goes up so the bass stays full (about half compensated, so
  some of the classic thinning remains).
- **Filter envelope**: 1 ms attack, then exponential decay to zero, over the time set by
  **Decay** (section 3a).
  - **Env Amount** 0–100% = 0 … +6 octaves of cutoff at the peak. Default 50%, so Decay is
    always clearly audible; a set-and-forget knob.
- **Velocity → filter** (**Vel>Filter**, 0–100%): at velocity 127 the cutoff is exactly where the
  knobs put it; softer notes are darker, by up to 3 octaves at velocity 1 when Vel>Filter is 100%.
  (So presets sound "as designed" with Full Level on, and soft pad hits close the filter.)
- **Zipper-free sweeps**: Cutoff is smoothed in the pitch (log) domain over about 15 ms, and the
  filter coefficients are recomputed every 8 samples and interpolated per sample, so a Q-Link or
  automation sweep is smooth, never stepped.
- All cutoff contributions are summed in octaves, then clamped to 20 Hz … 0.45 × sample rate.

## 3a. Cutoff + Decay: from a kick to a full growl

The two playing knobs cover the whole journey on their own: **both at 0 is almost a kick drum;
both at 100 is a full-on growl.** Turning them up together goes thud → round sub → warm pluck →
rolling bass → buzzy bass → growl. Each knob also brings in a little of the character that its
end of the journey needs, so the other knobs can stay where they are.

**Decay** (0–100%, exponential, ≈ 60 ms … 3 s; section 12) shapes both envelopes together:

- the **filter envelope** decays over Decay;
- the **amp envelope** decays over 1.5 × Decay (so the note rings a little after the filter has
  closed: round, not chopped);
- **thump zone (Decay 0–25%)**: the sustain level is pulled down to nothing at Decay 0, and a
  quick **pitch drop** fades in at the start of each note: up to +2 octaves falling to the note
  over about 40 ms at Decay 0, none at Decay 25% and above. With a low Cutoff that is a kick-like
  thud tuned to the note you play.
- **hold zone (Decay 75–100%)**: the sustain level is pushed up toward full at Decay 100, so long
  notes hold and keep growling.
- between 25% and 75%, the Sustain knob is used as set.

**Cutoff** (0–100%, exponential, ≈ 40 Hz … 10 kHz) sets the brightness, and:

- **growl zone (Cutoff 60–100%)**: extra saw and extra drive fade in on top of the Saw and Drive
  knobs (up to +70% saw and +60% drive at Cutoff 100%), with a touch more resonance (+15%), so the
  top of the knob turns from bright into a thick, buzzing growl rather than just thin and fizzy.
- below 60%, Saw, Drive and Resonance are exactly as set.

These are **offsets added to the character knobs and clamped, never overwrites**: with the knobs
at their defaults the journey is as described; turning Saw or Drive up just starts the growl
earlier. All of it is worked out once per block and smoothed, so riding the two Q-Links is
click-free.

The map, with every other knob at its default:

| | Decay 0–15% | Decay 35–55% | Decay 85–100% |
|---|---|---|---|
| **Cutoff 0–15%** | kick-like thud | round, deep sub | dark, smooth held sub |
| **Cutoff 35–50%** | short, punchy pluck | warm rolling house bass | soft "wow" that slowly closes |
| **Cutoff 85–100%** | bright, snarling stab | buzzy driving bass | **full-on growl** |

- Both knobs are exponential, so every part of their travel matters (harness-checked).
- Cutoff and Decay are Q-Links 1 and 2, on the first Q-Link column, so one hand rides both.

## 4. Amp, drive and output

- **Amp envelope**: attack, decay (from the Decay knob, section 3a), sustain, release (SDK `ADSR`).
  - Attack 0–100% ≈ 1 ms … 1 s
  - Sustain 0–100%: how much level holds while the note is held, after the decay
  - Release 0–100% ≈ 5 ms … 3 s
  - Velocity changes the level gently and fixed: velocity 1 is 6 dB quieter than 127 (no knob).
- **Drive** 0–100%: gentle soft-clip after the filter (before the amp, so its tone doesn't change
  as a note fades), level-compensated so turning it up warms and thickens rather than just getting
  louder. 0% = clean.
- **DC / rumble cut**: a fixed 20 Hz one-pole high-pass, so nothing inaudible eats headroom.
- **Volume** (parameter 0): −60 … +6 dB, default −6 dB. Smoothed, no side effects.
- **Output never clips**: a transparent safety soft-limit above −3 dBFS keeps every setting under
  0 dBFS, and every preset is tuned to peak well below that (harness check).
- Denormals: filter and high-pass states are flushed once the voice is silent.

## 4a. Echo: a bass-safe delay

A tempo-synced delay built for bass (Simon: "let's get it in, with the echoes' low cut").

- **The echoes have no low end.** Everything that goes into the delay first loses its lows below
  about 250 Hz (24 dB/octave; section 12), and the repeats are darkened a little more on every pass (gentle
  low-pass at about 4.5 kHz in the feedback loop). The bass itself is never delayed, so the sub
  stays one tight, centred note that never smears into the kick; the echoes are soft, slightly
  dark repeats above it.
- **Added on top, never a crossfade**: Mix sets how loud the echoes are; the dry bass stays at
  full level whatever Mix does.
- **Ping-pong**: the repeats alternate left and right (first repeat left). This is the only
  stereo in the plugin; the bass stays dead centre.
- **Time** follows the MPC's tempo: 1/16, 1/8 triplet, 1/8, dotted 1/8, 1/4. If the MPC doesn't
  report a tempo, 120 BPM is used. Changing Time or the tempo moves the echo with a short
  (about 20 ms) crossfade, so there's no click or pitch warble.
- **Feedback** 0–100% maps to 0 … 80% gain per repeat, with a soft limit inside the loop: even at
  100% the repeats always die away and never run away.
- The echoes keep ringing after the note ends (that's the point); they are flushed to exact
  silence once below −120 dB, so no denormal tail.
- **Mix = 0 (the default) is exactly the dry sound**: echo off, no CPU spent on it.
- Memory: a 1.5 s stereo buffer (enough for 1/4 at 40 BPM), about 0.6 MB, allocated in
  `prepare()`, never in `process()`.

## 5. Parameters (order and ids permanent once built)

| # | id | Name | Range | Default |
|---|---|---|---|---|
| 0 | `volume` | Volume | −60 … +6 dB | −6 dB |
| 1 | `cutoff` | Cutoff | 0–100% (≈40 Hz … 10 kHz, exponential; growl zone, §3a) | 35% |
| 2 | `decay` | Decay | 0–100% (≈60 ms … 3 s; filter, 1.5× for amp; thump and hold zones, §3a) | 40% |
| 3 | `reso` | Resonance | 0–100% | 25% |
| 4 | `envamt` | Env Amount | 0–100% (0 … +6 oct) | 50% |
| 5 | `sub` | Sub | 0–100% | 50% |
| 6 | `saw` | Saw | 0–100% | 30% |
| 7 | `detune` | Detune | 0–100% (0 … 20 cents, saw only) | 10% |
| 8 | `glide` | Glide | 0–100% (off, then 10 … 500 ms) | 0% |
| 9 | `attack` | Attack | 0–100% (≈1 ms … 1 s) | 0% |
| 10 | `sustain` | Sustain | 0–100% | 25% |
| 11 | `release` | Release | 0–100% (≈5 ms … 3 s) | 10% |
| 12 | `drive` | Drive | 0–100% | 0% |
| 13 | `velfilt` | Vel>Filter | 0–100% | 30% |
| 14 | `echotime` | Echo Time | 1/16, 1/8T, 1/8, 1/8., 1/4 | 1/8. |
| 15 | `echofb` | Feedback | 0–100% (0 … 80% per repeat) | 35% |
| 16 | `echomix` | Echo Mix | 0–100% (0 = off) | 0% |

17 parameters, all automatable (Q-Link moves can be recorded into the sequence).

## 6. UI and Q-Links: two pages, four columns

Page 1 **"BASS"** (each column is one Q-Link bank of four, top to bottom = Q-Links 1–4):

| FILTER | OSC | AMP | ECHO |
|---|---|---|---|
| **Cutoff** | Sub | Attack | Drive |
| **Decay** | Saw | Sustain | Echo Time |
| Resonance | Detune | Release | Feedback |
| Env Amount | Glide | Volume (fader) | Echo Mix |

Page 2 **"SETUP"**: Vel>Filter, plus the OCKM BSS badge. (Drive sits at the top of the ECHO
column because page 1 is full; it is the column's "colour" knob.)

- Cutoff and Decay are Q-Links 1 and 2, with Resonance and Env Amount under them, so the
  first Q-Link column holds the whole filter, including the two playing knobs.
- Program-mode Q-Links 1–16 → page 1, column by column (Cutoff on Q-Link 1, Echo Mix on 16).
  Vel>Filter has no Program-mode Q-Link.
- Skin: copied from `OCKM Gate/make_skin.py`, adding an **olive** tint to its `TINTS` table
  (default `--tint olive`). Black knurled knobs, dark ink on label strips, printed column headings,
  amber-lit controls, battered brushed metal. The panel font gets any letters/digits these labels
  need (the FM Kit session found Gate's font lacks W, X, Z, J and most digits).

## 7. Factory presets (9)

Echo is off (Mix 0) except in two presets. All tuned for house and techno; all peak below 0 dBFS (target ≤ −3 dBFS) under the harness demo.
Following the two-knob rule, most presets share the default character and differ mainly in
Cutoff and Decay; only a few change the character knobs, and then only a little. Any preset can
be turned into any other spot on the map with the first two Q-Links.

| # | Name | Cutoff / Decay | Other changes |
|---|---|---|---|
| 1 | **Init Soft Bass** | 35 / 40 | the defaults: round core, half sub, a touch of saw |
| 2 | **Kick Thud** | 8 / 3 | Sub 70: almost a kick, tuned to the note |
| 3 | **Deep Round Sub** | 15 / 55 | Saw 10, Sub 70: warm sub for minimal techno |
| 4 | **Warm House Pluck** | 40 / 20 | Echo 1/8. at 15%: the bouncy deep-house pluck |
| 5 | **Rubber Glide** | 30 / 60 | Glide 35%, Resonance 40 |
| 6 | **Buzzy Techno** | 65 / 50 | Drive 20 |
| 7 | **Muffled Dub** | 12 / 85 | Release 35, Detune 30, Echo 1/4, Feedback 55, Mix 30: dub techno |
| 8 | **Full Growl** | 95 / 92 | Resonance 35, Detune 25 |
| 9 | **Open Sweep** | 20 / 30 | Resonance 55: start here and ride both Q-Links up to the growl |

## 8. CPU budget (MPC One, 32-bit ARM)

Mono, so the worst case is one voice, always. Per sample: three oscillator phases and a PolyBLEP
saw (~25 operations), the 4-pole ladder with in-loop saturation (~50), two envelopes, drive,
high-pass and limiter (~25), echo when Mix > 0 (~25, stereo): about 125 simple float operations
per sample, roughly 6 million per second at 48 kHz.

- Coefficients (one `exp2` for pitch and cutoff, one `tan` for the filter) every 8 samples,
  interpolated in between. No per-sample `exp`, `pow`, `sin`, `tan` or `tanh`.
- **Budget: under 5% of one MPC One core per instance; hard ceiling 8%.** On the desktop harness
  this corresponds to under 0.3% of one core (OCKM-03 measures about 0.2%), which the harness
  prints and checks.
- No oversampling. The saturation is gentle and the saw is band-limited, so aliasing stays low; if
  the harness hears audible aliasing at Cutoff 100% / Drive 100% on high notes, 2× oversampling of
  the filter would be considered and the budget re-measured.
- Memory: about 0.6 MB for the echo buffer (allocated in `prepare()`) plus a few hundred bytes of
  state; nothing allocated in `process()`.

## 9. Not in v1

Left out on purpose, under the governing rule:

- Polyphony, paraphony or unison (mono only)
- Stereo width (a bass belongs in the middle on a club system)
- Octave / tune knobs (use the MPC's program or track transpose)
- Extra waveforms or an oscillator shape knob; noise
- LFOs, mod matrix, arpeggiator, sequencer
- Filter types other than the 24 dB low-pass; filter envelope attack/sustain/release knobs
- Separate filter and amp decay knobs (one Decay drives both, by design)
- Knobs to change the zone amounts (thump pitch drop, growl saw/drive); they are fixed in v1
- A separate pitch-envelope knob (the thump zone covers the kick end)
- A velocity-to-amp knob (fixed gentle amount instead)
- Accent, slide-by-velocity or any acid behaviour (that's OCKM-03's job)
- Effects (chorus, reverb, EQ) beyond the one drive and the echo
- Echo extras: free time in ms, filter knobs for the echoes, a mono/ping-pong switch, modulation
- Pitch bend and mod wheel
- 2× oversampling (see section 8)
- More than 9 presets

## 10. Tests

1. **Build**: `python3 $SDK/tools/build.py "OCKM BSS"` → 0 validator errors.
2. **host_sim**: `test --wav build/test.wav` → no FAIL; every WARN explained.
3. **`tests/harness.cpp`** (includes the plugin source, ends with `ALL PASS`):
   - **Filter sweep opens smoothly**: a held saw-heavy note with Cutoff ramped 0 → 100% over
     4 s: brightness (spectral centroid per 20 ms window) rises monotonically across the sweep,
     with no window-to-window jump bigger than a set limit.
   - **No zipper noise**: Cutoff stepped in coarse Q-Link-sized jumps every block; the
     high-frequency energy of the difference signal stays below a threshold (no clicks per block).
   - **Knob law**: measured brightness changes by a similar amount in each quarter of the Cutoff
     travel (no dead zone).
   - **Cutoff + Decay give the variety**: a 4 × 4 grid of Cutoff × Decay (at default settings
     otherwise) gives clearly different sounds: brightness rises along Cutoff, note length and
     filter-close time rise along Decay, every step in each direction is measurably different,
     and each quarter of Decay's travel changes note length by a similar ratio.
   - **Kick to growl**: at Cutoff 0 / Decay 0 the pitch falls by at least an octave in the first
     30 ms and the note is below −40 dB within 300 ms (kick-like); at Cutoff 100 / Decay 100 a held
     note keeps at least half its level after 1 s and is far brighter and denser (lower crest
     factor) than the Init sound (built as: fuller, held level ≥ 6 dB above Init; section 12); brightness and loudness-duration rise step by step along the
     diagonal between them. Both corners peak below 0 dBFS.
   - **Zones are offsets**: below Cutoff 60% and between Decay 25–75%, the output is bit-identical
     to the same settings with the zones switched off in the code (they only act where promised).
   - **Resonance stable at full**: Resonance 100% with Cutoff, Env Amount and Drive swept across
     their ranges, every note from C0 to C5: output finite, no runaway, peak below 0 dBFS,
     decays to silence after release.
   - **Bass stays full with resonance**: fundamental level at Resonance 100% is within about
     6 dB of Resonance 0%.
   - **No clipping across presets**: every preset over the demo bassline at velocity 127 peaks
     below 0 dBFS (printed per preset), and the worst-case setting (everything at maximum,
     Volume +6 dB) stays below 0 dBFS.
   - **Glide**: overlapping notes with Glide 40% move pitch gradually (zero-crossing pitch
     measurement) and don't retrigger the filter envelope; with Glide 0% every note re-plucks;
     non-overlapping notes don't glide.
   - **Velocity**: velocity 40 is darker than 127 with Vel>Filter on, and identical with it at 0.
   - **Echo**: the echoes carry at least 24 dB less energy below 150 Hz than the dry bass; the
     first repeat lands on time (dotted 1/8 at 124 BPM = 362.9 ms, within 1 ms) on the left, the
     second on the right; Feedback 100% dies away below −60 dB within 10 s, no denormals; Mix 0
     is bit-identical to the echo-less code; Time changed mid-note gives no click; the dry part
     stays centred (L = R) with the echo off.
   - **Volume (parameter 0) harmless**: nudging it doesn't change anything but level.
   - CPU on the desktop printed and under 0.3% of one core.
4. **Demo WAV**: `build/demo.wav`, a 4-bar house bassline per preset, then a bassline on
   Open Sweep with Cutoff and Decay ridden from kick to growl and back, then a few bars of
   Warm House Pluck with the echo turned up.
5. **Package**, then `python3 sd_card.py` with no warnings, then hand-over with the SD-card steps.
6. On the MPC One (Simon): CPU meter with one instance playing a 16th-note line; feel of the
   Cutoff Q-Link across its whole travel.

## 11. Open questions for Simon

1. ~~Mono or a few voices?~~ **Resolved (Simon): one note at a time.**
1a. ~~What gives the variety?~~ **Resolved (Simon): mostly Cutoff + Decay** (section 3a).
2. Glide only on overlapping notes: built as recommended (Simon didn't object).
3. Olive tint: built as proposed.

## 12. Changes made while building (v1.0.0)

The harness found these; each is reflected in the code and README.

1. **Cutoff range 40 Hz … 10 kHz** (was ≈14 kHz). Above about 4 kHz a bass has little energy
   left, so the top quarter of the knob hardly changed the sound. Each 10% of travel is now
   about 0.8 octave. Measured top-end change per quarter of travel: 18.6, 28.7, 18.1, 5.5 dB; the
   top quarter is still the subtlest (fizz and air), flagged for the hardware feel check.
2. **Decay ≈ 60 ms … 3 s** (was ≈15 ms). Envelope times are "time to fall to about 2%"; with a
   15 ms floor the amp died in about 20 ms, a click rather than a kick. At Decay 0 the note is now
   below −40 dB within about 100 ms: a short, kick-like thud.
3. **Glide 10 … 500 ms** (law 10·50^x). With the first law, 35% was only 25 ms.
4. **Echo:** the low cut on what goes into the echo is **24 dB/octave** at 250 Hz (was 12), and
   each pass also loses lows below 200 Hz; feedback tops out at **80%** per repeat (was 85%).
   Measured: echoes carry 34 dB less energy below 150 Hz than the bass; Feedback 100% is 98 dB
   down after 10 s.
5. **Output +7 dB** (voice make-up gain), so presets peak at −4.7 … −7.8 dBFS instead of about
   −12 dBFS.
6. **Every control-rate value is ramped per sample** (and the filter coefficient gets a small
   extra lag), so there are no steps every 8 samples. Control-rate tone during a 0–100% sweep in
   0.15 s: −74 dB.
7. **Tests:** "brightness" is measured as a top-end index (energy above 250 Hz, 1 kHz and 4 kHz,
   averaged, in dB), because a Hz-style measure is swamped by the fundamental in the dark half.
   The growl corner is checked as "fuller (held level ≥ 6 dB above Init)" instead of "lower crest
   factor", which turned out not to track density for a resonant saw. The zipper check compares
   coarse per-block jumps with 8-sample steps (they measure the same).
8. Program-mode Q-Links use the same layout as page 1 on screen (all 16), instead of
   parameters 1–14 in order.

Harness results (selection): filter sweep rises with no dips and no jump over 2 dB per 46 ms;
kick corner falls from 130 Hz to 32 Hz in the first 60 ms; growl corner holds its level after 1 s
and is 13 dB fuller than Init; Resonance 100% peaks at −1.5 dBFS worst case across C0–C6, every
cutoff, envelope and drive; the fundamental at Resonance 100% is 3.6 dB below Resonance 0%; CPU
about 0.1% of one desktop core.
