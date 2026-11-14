# OCKM house style

Every plugin in this repo, and every agent working on one, follows this file. The MPC Plugin
SDK's own rules (its `AGENTS.md` and `.claude/skills/`) apply as well. Where the two overlap,
this file adds to the SDK's rules and never relaxes them.

SDK folder: `$MPC_SDK`, defaulting to `/Users/simon/Projects/mpc-plugin-builder-sdk`. The SDK is
a separate git repo: read it, run its tools, never edit it.

## Identity

- Manufacturer is always **`OCKM`**: in `plugin.json` (`manufacturer`, `author`), in
  `PluginInfo.vendor`, and in the output folder name `OCKM - VST - <Name>`.
- Licence: **MIT**, `Copyright (c) OCKM`. Each README says so and names the licence of any DSP
  idea or data borrowed from elsewhere.
- **Permanent once built:** the `uid` (in `plugin.json` and `MPC_FOURCC`), every parameter `id`,
  and the parameter order. Add new parameters at the end only. Never rename, reorder or remove
  one, or Simon's MPC projects will load the wrong values.
- **No other companies' product or brand names** (for example Roland, TB-303, MC-101) anywhere:
  code, comments, UI, preset names, READMEs or SPECs. Describe the sound instead ("acid bass",
  "squelchy ladder filter").

## Lean

Each plugin does one job well. Anything beyond that goes in the SPEC's and README's
**"Not in v1"** list rather than being half-built.

## Workflow for each plugin

1. **SPEC.md first**: what it does, parameters with ids, ranges and defaults, pages and Q-Link
   columns, presets, CPU budget, "Not in v1", and how it will be tested. Get Simon's go-ahead
   before writing code.
2. **Build:** `python3 $SDK/tools/build.py "<Plugin folder>"` → 0 validator errors.
3. **host_sim:**
   `$SDK/.sdk-tools/host_sim "<Plugin>"/build/native/*.dylib test --wav "<Plugin>/build/test.wav"`
   → no FAIL, and every WARN understood and explained.
4. **tests/harness.cpp** checks the plugin's own behaviours (the things SPEC.md promises), not
   only what host_sim covers. It `#include`s the plugin source and ends with `ALL PASS`:
   `clang++ -std=c++17 -O2 -I$SDK/framework "-I<Plugin>/src" "<Plugin>/tests/harness.cpp" -o "<Plugin>/build/harness" && "<Plugin>/build/harness" "<Plugin>/build/demo.wav"`
5. **Demo WAV** of the presets, rendered by the harness into `build/`.
6. **Package:** `python3 $SDK/tools/package.py "<Plugin folder>"`.
7. **Hand-over to Simon:** the output folder path (`<Plugin>/build/OCKM - VST - <Name>`) and the
   SD-card steps:
   - Copy the `OCKM - VST - <Name>` folder into a folder named `VST2 Plugins` on the SD card or drive.
   - The MPC needs MPC OS 3.10.1 or later.
   - On the MPC: Preferences → Enhancements (Beta) → turn on **Enable Enhancements** and
     **External Plugin Support**.
   - Restart the MPC.
   - Plugins run unsandboxed. If the MPC freezes or crashes on a plugin, deleting its folder
     from the card is the fix.

Never call a plugin done without steps 2–4 passing. Build output (`build/`, `dist/`) is not
committed.

## Look

Battered brushed-metal hardware panels drawn by each plugin's `make_skin.py`, using Python's
standard library plus numpy, seeded so the output is repeatable. No other dependencies.

- **One tint per plugin**, so they can be told apart at a glance:
  OCKM-03 = **silver**, OCKM Gate = **gunmetal**. Still free: champagne, bronze (the `TINTS`
  table in `OCKM Gate/make_skin.py`). A new plugin picks a free one or adds its own.
- **Lettering:** dark ink on clean label strips. Names SemiBold, values Bold, about **13% larger**
  than `gen_ui.py`'s defaults.
- **Controls:** black knurled knobs with spun-metal caps; worn plastic buttons and switches that
  **light amber** when on.
- **Layout:** printed column headings. Each Q-Link bank is a **column** of four related controls.
  An **"OCKM <NAME>" badge** fills an empty slot.
- **TUI.json is edited as ordered key/value pairs**, never with a plain `dict`. It repeats
  `"version"` keys on purpose, and a dict would silently drop them. Copy the `Pairs` loader and
  dumper from an existing `make_skin.py`.
- Start a new `make_skin.py` by copying `OCKM Gate/make_skin.py` (it already takes `--tint`).
  The SDK is found through `MPC_SDK`. Re-run it after every `ui.json` change, because a
  `skins/` folder stops `build.py` from regenerating the skin.

## DSP

- Realtime-safe `process()`: no allocation, locks, logging or file I/O.
- **Parameter 0 is harmless**: master or output level, with no side effects (the MPC nudges it on
  every load).
- Expensive maths per block or at control rate, not per sample. No per-sample `exp`, `pow`,
  `sin` or `tan` in inner loops. The MPC One is a slow 32-bit ARM, so aim for a few percent of
  one core per instance.
- **No denormal tails**: flush or offset feedback paths, so host_sim reports none.
- **Output never clips**: every preset peaks below 0 dBFS. Check it in the harness.
- Content files (wavetables, samples) go in `Content/` and load in the constructor or `prepare()`
  via `mpc::contentPath()`, never from absolute paths.

## Never

- No jailbreaks, firmware patches or system modification (for example HAKAI). The only route
  onto the MPC is the official `VST2 Plugins` folder plus the preference switches, or the SDK's
  `tools/mpc_remote.py` (developer mode) when Simon asks for it.
- Don't copy code from other plugins or firmware. Taking ideas from MIT-licensed projects is
  fine; credit them in the README.
- Commit or push only when Simon asks.
