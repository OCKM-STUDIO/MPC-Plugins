# OCKM Gate — specification (v1)

Status: **built (v1), tested on an MPC One 2026-11-14.** Decisions taken while building are marked "(build)"; changes after the hardware test are marked "(hardware)" and listed in section 12.

OCKM Gate is a tempo-locked trance gate / pump **effect** for the MPC. It chops, pumps and pulses whatever runs through it, in a rhythm you program, tap in, or pick from presets. Inspiration: the rhythmic pulse sounds on Simon's groovebox. Naming rule: no third-party product or brand names in the code, UI, presets or README; describe it as "trance gate", "pump" or "slicer".

| Item | Value |
|---|---|
| Plugin name | `OCKM Gate` (permanent) |
| Manufacturer | `OCKM` |
| Kind | effect, 2 in / 2 out (insert effect on a track, program or master) |
| Output folder | `OCKM - VST - OCKM Gate/` |
| Project folder | `OCKM Gate/` in the MPC-Plugins repo |
| Licence | MIT, released from the OCKM STUDIO repo |
| Look | same brushed-metal family as OCKM-03, in a different tint (open question 1) |
| CPU | tiny (one gain per channel per sample); well under 2% of an MPC One core |

## 1. How it locks to the MPC

Effects get **no MIDI** on the MPC, and plugins cannot read the pads. They do get the MPC's **tempo and song position** on every audio block, so the gate follows the sequencer:

- **MPC playing:** the step grid is locked to the song position, so step 1 always lands on the beat, and stopping, starting and looping stay in sync.
- **MPC stopped:** the gate keeps running at the MPC's tempo from its own clock (switchable with `Run Stopped`), so you can hear it while auditioning pads. When the MPC starts, it snaps back to the song position.

## 2. The rhythm: 16 steps, three ways to set it

The pattern is 16 steps. Each step has an **On/Off switch** and a **Level** (0–100%). An Off step is silent (or not ducked, in Pump mode); an On step plays at its level. All three ways Simon asked for set the same 32 values:

1. **Step grid (on/off):** touch the 16 switches on the STEPS page.
2. **Levels (accents and ghost steps):** the 16 knobs on the LEVELS page. Q-Links work here.
3. **Preset rhythms:** the built-in programs (section 8) each load a rhythm plus a matching pump shape. Pick one, then edit.

Plus:

4. ~~**Tap it in (live)**~~ (hardware): dropped after the MPC test, see sections 7 and 12.

Other rhythm controls:
- `Steps`: pattern length, 1–16 (for 3-, 5- and 7-step polyrhythms against the bar).
- `Rate`: step length, one of 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/4 (1/4 added last: hardware). Default 1/16.
- The pattern restarts every N bars, N being the fewest whole bars that hold Steps × Rate (16 × 1/16 = 1 bar, 16 × 1/8 = 2, 16 × 1/4 = 4), counted from the song start (hardware).
- `Swing`: 50–75%, delays every second step.

## 3. The nature of the pulse: four modes and a shape

`Mode` picks the character; the shape knobs then fine-tune it.

| Mode | What an On step does | Sounds like |
|---|---|---|
| **Gate** | Opens over `Attack`, stays open for `Length` (% of the step), closes over `Release` | Classic hard trance chop |
| **Pump** | Ducks to (1 − Depth × Level) at the step start, then swells back over `Release` with `Curve` | Kick-triggered sidechain pump |
| **Smooth** | A rounded swell (raised cosine), peaking at `Length` into the step | Soft tremolo-style pulse |
| **Ping-Pong** | Like Gate, but alternate On steps go to the left and right (`Width` sets how far) | Moving, wide stereo gate |

Shape controls (shared by all modes; each one's effect is described per mode in the UI help text):
- `Depth` 0–100%: how far the gate closes (100% = full silence between steps).
- `Attack` 0.5–50 ms: opening time; never below 0.5 ms, so there are no clicks.
- `Length` 5–100% of the step: gate open time (Gate, Ping-Pong) or swell peak (Smooth).
- `Release` 1–100% of the step: closing time (Gate, Ping-Pong) or recovery time (Pump).
- `Curve` −100…+100%: shape of the release/recovery, from snappy (exponential) to slow-then-fast (logarithmic). 0 = linear.
- `Width` 0–100%: stereo spread for Ping-Pong (0 = both sides, 100 = hard left/right).
- `Mix` 0–100%: dry/wet; 100% = fully gated.

Gain is smoothed per sample, and every edge is at least 0.5 ms long, so hard settings stay click-free.

## 4. Signal flow

```
input L/R -> step clock (song position or free run, swing, rate, length)
          -> per-step envelope (mode, attack, length, release, curve) x step level
          -> gain = 1 - Depth x (1 - envelope)    [per channel; Ping-Pong offsets L/R]
          -> mix with dry -> Output level -> L/R
```

Envelope maths runs per sample with only adds and multiplies; curve tables are worked out per block. Clamped everywhere; no allocation, locks or I/O in `process()`.

## 5. Parameters (order and ids permanent once built)

Parameter 0 is Output, which is harmless when the host nudges it on load. Append new parameters at the end only.

| # | id | Name | Range / choices | Default |
|---|---|---|---|---|
| 0 | `output` | Output | −24…+6 dB | 0 dB |
| 1 | `mode` | Mode | Gate / Pump / Smooth / PingPong | Gate |
| 2 | `depth` | Depth | 0–100% | 100 |
| 3 | `mix` | Mix | 0–100% | 100 |
| 4 | `rate` | Rate | 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/4 (1/4 appended: saved values are plain choice indices, so old projects keep their rate) (hardware) | 1/16 |
| 5 | `attack` | Attack | 0.5–50 ms | 2 ms |
| 6 | `length` | Length | 5–100% | 50 |
| 7 | `release` | Release | 1–400% of a step (a pump can recover across several steps) (build) | 15 |
| 8 | `curve` | Curve | −100…+100% | 0 |
| 9 | `swing` | Swing | 50–75% | 50 |
| 10 | `steps` | Steps | 1–16 | 16 |
| 11 | `width` | Width | 0–100% | 100 |
| 12 | `runstop` | Run Stopped | Off / On | On |
| 13 | `taprec` | Tap Rec | Off / On (not on screen since the hardware test) | Off |
| 14 | `tap` | TAP | button (any press = one tap; not on screen since the hardware test) | — |
| 15–30 | `on1`…`on16` | Step 1…16 | Off / On | all On (matches the "Init" preset, which the MPC loads first) (build) |
| 31–46 | `lv1`…`lv16` | Lvl 1…16 | 0–100% | 100 |

That makes 47 parameters.

## 6. UI (three pages; the Q-Link banks are columns)

**GATE** (the front page: the sound):

| Pulse | Shape | Rhythm | Output |
|---|---|---|---|
| Mode (menu) (build) | Attack | Rate (menu) | *OCKM GATE badge* |
| Depth | Length | Steps | *empty* |
| Mix (fader) | Release | Swing | *empty* |
| Width | Curve (bipolar) | Run Stopped (switch) | Output (fader) |

(hardware) TAP and Tap Rec were taken off this page; Run Stopped moved into the Rhythm column.

**STEPS:** 16 on/off switches, steps 1–16 in reading order (row 1 = steps 1–4).

**LEVELS:** 16 knobs, same order. A Q-Link column covers steps 1, 5, 9 and 13 (the downbeats of each quarter), which suits accent work.

## 7. Tap Rec: how it worked (dropped from the screen after the hardware test)

- The on-screen TAP is a parameter. Every press changes it; the plugin treats any change as one tap.
- The plugin remembers the latest song position from the audio thread. On a tap it works out the nearest step (taking `Rate`, `Swing` and `Steps` into account), switches that step's On/Off parameter on, and tells the MPC the parameter changed so the STEPS page updates.
- Taps only count while `Tap Rec` is On **and** the MPC is playing, so loading a project or a preset can never write a pattern.
- The first tap after arming clears all 16 On switches (levels are kept).
- **Limit:** the touch screen and UI add some delay. Snapping to the nearest 1/16 hides most of it, but very fast taps at 1/32 may land a step late. A `Tap Offset` setting is a possible later addition.
- **Hardware result:** the MPC does pick up step changes the plugin makes itself (the STEPS page showed them, and the switches stayed in step with the sound), but tapped rhythms didn't land where they were meant to. Tapping was dropped in favour of more presets. The code and both parameters stay, so ids and order are unchanged, but neither is on screen. `Tap Rec` defaults to Off, so the hidden code does nothing.

## 8. Factory presets (built-in programs, 18)

Each one sets the rhythm **and** the pulse shape.

1. **Init:** all 16 steps on at full level, Gate, 50% length (a straight 1/16 chop).
2. **Classic 16ths:** the trance-gate standard, an x.xx.xx.x.xx.x.xx-style 16th pattern with short release.
3. **Offbeat Pump:** Pump on the 8th offbeats; the four-on-the-floor sidechain feel.
4. **Quarter Pump:** Pump on every quarter, long recovery with a slow curve; the classic house duck.
5. **3-3-2:** dotted rhythm (steps 1, 4, 7, 9, 12, 15) with a hard gate.
6. **Ghost Chop:** full-level accents on the beat, ghost steps at 30% in between.
7. **Smooth Pulse:** Smooth mode, every 8th, a soft tremolo wash for pads.
8. **Ping-Pong Stutter:** Ping-Pong mode, 1/16, full width.

Added after the hardware test (appended, so 1–8 keep their numbers):

9. **2 and 4 Duck:** Pump on beats 2 and 4, recovering over 3 steps.
10. **Swung 16th Pump:** Pump on every 16th with accents and ghosts, 58% swing, 75% depth.
11. **Half-Time Pump:** 1/4 rate, Pump on beats 1 and 3, slow curve.
12. **Gallop 16ths:** the X.XX trance gallop, hard gate.
13. **Triplet Roll:** 1/16T, 12 steps, accent every third.
14. **32nd Stutter:** 1/32, accent every fourth, very short release.
15. **Dotted 8ths:** 3-3-3-3-4 (steps 1, 4, 7, 10, 13).
16. **Clave 3-2:** steps 1, 4, 7, 11, 13.
17. **Slow Swell:** 1/4 Smooth swell on every beat, 70% depth.
18. **Bar Swell:** 1/4 Smooth, one long swell per bar (steps 1, 5, 9, 13 span 4 bars).

## 9. Not in v1

MIDI or pad triggering (the MPC doesn't send MIDI to effects), tapping a rhythm in (tried, dropped after the hardware test), more than 16 steps, per-step pan or filter, a filter gate, sidechain from another track, and effects beyond the gate itself.

## 10. Open questions

1. **Colour of the brushed metal.** It should be the same worn style as OCKM-03, but in a different tint. *Recommendation: a dark gunmetal, a blue-grey anodised finish, with the same cream knob pointers, so the two plugins read as a family. I'll render two or three tints (gunmetal, champagne gold, bronze) to choose from before the final build.*
2. **Pattern length vs. bar.** With `Steps` below 16 the pattern drifts against the bar (e.g. 3 steps over 16ths), which is the point of polyrhythms, but it means the pattern doesn't restart on bar 1. *Recommendation: restart on every bar (so it always lines up with your sequence), with the polyrhythm happening inside the bar.*
3. **Mode as a 4-way switch on the front page, or a menu?** *Recommendation: a 4-way switch: one touch, and you see all four options.*

## 11. Decisions taken while building (build)

- Open questions 2 and 3 went with the recommendations, except Mode: the four names ("Smooth", "PingPong") do not fit the 58-pixel segments of a 4-way switch, so Mode is a menu like Rate.
- Every On step **triggers** an envelope that runs on across Off steps, so a pump keeps recovering through them and a long Release can span several steps.
- Taps are flagged on the UI thread and applied by the audio thread at the exact song position. Only a real change of the TAP value counts, so choosing a preset or loading a project with Tap Rec armed never writes a step.
- Patterns restart on every bar (open question 2).

## 12. Hardware test on the MPC One (2026-11-14)

| Check | Result | Action |
|---|---|---|
| STEPS page shows steps the plugin switched on itself (Tap Rec) | Works: the switches show the right state, and one touch toggles them | none |
| STEPS toggle switches line up with their labels and the panel art | Correct | none |
| Tapped rhythms land where intended | No | TAP and Tap Rec taken off the GATE page; 10 presets added instead. No Tap Offset parameter. |
| (Simon's request) a 1/4 rate | Added | `1/4` appended to Rate; patterns longer than a bar now span 2 or 4 bars instead of being cut off at the bar |

Still to hear on hardware: the 1/4 rate and the 10 new presets.
