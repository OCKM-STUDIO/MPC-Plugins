# OCKM Gate

A tempo-locked trance gate and pump effect for Akai MPC standalone hardware, by OCKM. It chops, pumps and pulses whatever runs through it, in a rhythm you program, tap in or pick from presets, locked to the MPC's sequencer.

## Requirements

- MPC OS 3.10.1 or later.
- Preferences → Enhancements (Beta) → turn on **Enable Enhancements** and **External Plugin Support**.
- Install: copy the `OCKM - VST - OCKM Gate` folder into `VST2 Plugins` at the root of a USB stick or SD card, insert it and restart the MPC. Add it as an insert effect on a track, program or the master (External Plugins → OCKM Gate).

## How it follows the MPC

Effects on the MPC get no MIDI, but they do get the tempo and song position, so the gate stays on the beat:
- **Playing:** steps are locked to the song position and the pattern restarts every bar, so it stays in time through stop, start and loops.
- **Stopped:** with **Run Stopped** On (default) it keeps pulsing at the MPC's tempo so you can hear it while auditioning; Off passes audio untouched.

## Setting the rhythm

The pattern is 16 steps; each has an **On/Off** switch (STEPS page) and a **Level** (LEVELS page) for accents and ghost steps.
- **Presets:** pick one of the 8 built-in programs, then edit.
- **Tap it in:** turn on **Tap Rec**, start the MPC, and touch **TAP** (or turn its Q-Link) in time. Each tap switches on the step nearest to it; the first tap after arming clears the old pattern (levels are kept). Turn Tap Rec off when done, then fine-tune on the STEPS and LEVELS pages. Taps are ignored while the MPC is stopped.
- **Steps** (1–16) sets the pattern length inside each bar; **Rate** the step size (1/8 … 1/32, with triplets); **Swing** delays every second step.

## The pulse: Mode and shape

| Mode | What an On step does |
|---|---|
| Gate | Opens over Attack, stays open for Length, closes over Release: the classic trance chop |
| Pump | Ducks at the step start, then swells back over Release: a sidechain pump. Off steps keep recovering |
| Smooth | A rounded swell peaking at Length: a soft tremolo pulse |
| PingPong | Like Gate, but alternate On steps go left and right (Width sets how far) |

**Depth** sets how far it closes; **Mix** blends with the dry signal; **Curve** shapes the release from slow-then-fast (−) to snappy (+). **Release** runs up to 400% of a step, so a pump can recover across several steps. Every edge is smoothed (at least 0.5 ms) so the hardest settings don't click.

## Pages

- **GATE:** Mode, Depth, Mix, Width | Attack, Length, Release, Curve | Rate, Steps, Swing | Tap Rec, TAP, Run Stopped, Output.
- **STEPS:** 16 on/off switches, steps 1–16 in reading order.
- **LEVELS:** 16 level knobs, same order. The Q-Link columns cover steps 1/5/9/13 etc.

## Presets

Init · Classic 16ths · Offbeat Pump · Quarter Pump · 3-3-2 · Ghost Chop · Smooth Pulse · Ping-Pong Stutter. Each sets a rhythm and a matching pulse shape.

## CPU

About 0.03–0.07% of one desktop core in the SDK's host simulator, so expect well under 2% on an MPC One. Not yet measured on hardware.

## Needs checking on hardware

When a tap switches a step on, the plugin tells the MPC so the STEPS page redraws. If the MPC doesn't redraw straight away, the rhythm still plays correctly and the switches update when you change page.

## Not in v1

Pad or MIDI triggering (the MPC doesn't send MIDI to effects), more than 16 steps, per-step pan or filter, a filter gate, sidechain from another track, tap offset calibration.

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
