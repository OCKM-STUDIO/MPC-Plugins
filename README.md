# OCKM MPC Plugins

Plugins for Akai MPC standalone hardware by **OCKM**, built with inMusic's MIT-licensed
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). Made for house and techno on an
MPC One.

| Plugin | Kind | What it does |
|---|---|---|
| [OCKM-03](OCKM-03/) | Instrument | Lean monophonic acid bass: one oscillator, squelchy resonant ladder filter, slide, stacking accents and drive. Works with the MPC arp |
| [OCKM Gate](OCKM%20Gate/) | Effect | Tempo-locked 16-step trance gate and pump, with swing, ping-pong and tap-in |
| [OCKM Wave](OCKM%20Wave/) | Instrument | Wavetable synth that teaches as you play: 8 generated wavetables, morph, warp, unison, and 24 lesson presets |

## Installing on an MPC

1. Copy the `OCKM - VST - <Name>` folder into a folder named `VST2 Plugins` on your SD card or drive.
2. Your MPC needs **MPC OS 3.10.1 or later**.
3. On the MPC: **Preferences → Enhancements (Beta)**, then turn on **Enable Enhancements** and
   **External Plugin Support**.
4. Restart the MPC.

Plugins run without a sandbox. If one ever freezes the MPC, delete its folder from the card.

## Building

The SDK lives in its own folder. Set `MPC_SDK` if it is not at the default location:

```bash
SDK=${MPC_SDK:-/Users/simon/Projects/mpc-plugin-builder-sdk}
```

```bash
python3 $SDK/tools/build.py OCKM-03
```

Each plugin's README has its full build, test and skin commands. [HOUSE-STYLE.md](HOUSE-STYLE.md)
holds the rules every plugin here follows.

## Licence

MIT. See [LICENSE](LICENSE).
