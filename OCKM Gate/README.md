# OCKM Gate

A tempo-locked trance gate and pump effect for Akai MPC standalone hardware, by OCKM. It chops, pumps and pulses whatever runs through it, in a rhythm you program or pick from 18 presets, locked to the MPC's sequencer.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta) → turn on **Enable Enhancements** and **External Plugin Support**.
- Install: copy the `OCKM - VST - OCKM Gate` folder into `VST2 Plugins` at the root of a USB stick or SD card, insert it and restart the MPC. Add it as an insert effect on a track, program or the master (External Plugins → OCKM Gate).

## How it follows the MPC

Effects on the MPC get no MIDI, but they do get the tempo and song position, so the gate stays on the beat:
- **Playing:** steps are locked to the song position, so it stays in time through stop, start and loops. The pattern restarts every bar when it fits in one (16 × 1/16); a longer one restarts every 2 or 4 bars, counted from the start of the sequence (16 × 1/8 = 2 bars, 16 × 1/4 = 4 bars).
- **Stopped:** with **Run Stopped** On (default) it keeps pulsing at the MPC's tempo so you can hear it while auditioning; Off passes audio untouched.

## Setting the rhythm

The pattern is 16 steps; each has an **On/Off** switch (STEPS page) and a **Level** (LEVELS page) for accents and ghost steps.
- **Presets:** pick one of the 18 built-in programs, then edit.
- **Steps** (1–16) sets the pattern length; **Rate** the step size (1/4, 1/8, 1/16, 1/32 and the 1/8 and 1/16 triplets); **Swing** delays every second step.
- The Rate menu lists 1/4 last (after 1/32), because it was added after the first version and older projects must keep their rate.

## The pulse: Mode and shape

| Mode | What an On step does |
|---|---|
| Gate | Opens over Attack, stays open for Length, closes over Release: the classic trance chop |
| Pump | Ducks at the step start, then swells back over Release: a sidechain pump. Off steps keep recovering |
| Smooth | A rounded swell peaking at Length: a soft tremolo pulse |
| PingPong | Like Gate, but alternate On steps go left and right (Width sets how far) |

**Depth** sets how far it closes; **Mix** blends with the dry signal; **Curve** shapes the release from slow-then-fast (−) to snappy (+). **Release** runs up to 400% of a step, so a pump can recover across several steps. Every edge is smoothed (at least 0.5 ms) so the hardest settings don't click.

## Pages

- **GATE:** Pulse: Mode, Depth, Mix, Width | Shape: Attack, Length, Release, Curve | Rhythm: Rate, Steps, Swing, Run Stopped | Output (fader).
- **STEPS:** 16 on/off switches, steps 1–16 in reading order.
- **LEVELS:** 16 level knobs, same order. The Q-Link columns cover steps 1/5/9/13 etc.

## Presets

Each sets a rhythm and a matching pulse shape.

- **Original 8:** Init · Classic 16ths · Offbeat Pump · Quarter Pump · 3-3-2 · Ghost Chop · Smooth Pulse · Ping-Pong Stutter.
- **House / techno pumps:** 2 and 4 Duck (pump on beats 2 and 4) · Swung 16th Pump (every 16th, accented, 58% swing) · Half-Time Pump (1/4, ducks on beats 1 and 3).
- **Trance gates:** Gallop 16ths (X.XX repeated) · Triplet Roll (1/16T, 12 steps, accent every third) · 32nd Stutter (1/32, accent every fourth).
- **Broken / syncopated:** Dotted 8ths (3-3-3-3-4) · Clave 3-2.
- **Slow 1/4 pulses:** Slow Swell (a soft swell on every beat) · Bar Swell (one long swell per bar).

## CPU

About 0.03–0.07% of one desktop core in the SDK's host simulator, so expect well under 2% on an MPC One. Not yet measured on hardware.

## Tested on an MPC One (2026-11-14)

- Loads, plays and stays in time with the sequencer. The STEPS switches sit correctly next to their labels, and a step switch turns off and on with one touch.
- **Tap Rec was dropped:** tapping on the touch screen didn't land the rhythm where it was meant to go. TAP and Tap Rec are no longer on the screen; the presets cover the common rhythms instead. The two parameters still exist inside the plugin (Tap Rec is off), so projects saved with the first build load unchanged.
- Added after the test: the 1/4 rate and 10 more presets. These have been checked on the computer but not yet on the MPC.

## Not in v1

Pad or MIDI triggering (the MPC doesn't send MIDI to effects), tapping a rhythm in (tried and dropped, see above), more than 16 steps, per-step pan or filter, a filter gate, sidechain from another track.

## Build and test

Run these from the MPC-Plugins repo root. `SDK` is the MPC Plugin SDK folder:

```
SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}
```

```
python3 $SDK/tools/build.py "OCKM Gate"
$SDK/.sdk-tools/host_sim "OCKM Gate"/build/native/*.dylib test --wav "OCKM Gate/build/test.wav"
clang++ -std=c++17 -O2 -I$SDK/framework "-IOCKM Gate/src" "OCKM Gate/tests/harness.cpp" -o "OCKM Gate/build/harness" && "OCKM Gate/build/harness" "OCKM Gate/build/demo-presets.wav"
python3 $SDK/tools/package.py "OCKM Gate"
```

To redraw the skin after changing `ui.json`: build with `--target native`, run `python3 "OCKM Gate/make_skin.py" --tint gunmetal`, then build again.

## Licence

MIT. Copyright (c) OCKM. Built with the MIT-licensed MPC Plugin SDK; all DSP written for this plugin. No third-party code.
