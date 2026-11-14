# OCKM MPC Plugins

Plugins for Akai MPC standalone hardware by **OCKM**, built with inMusic's MIT-licensed
[MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk). Made for house and techno on an
MPC One.

| Plugin | Kind | What it does |
|---|---|---|
| [OCKM-03](OCKM-03/) | Instrument | Lean monophonic acid bass: one oscillator, squelchy resonant ladder filter, slide, stacking accents and drive. Works with the MPC arp |
| [OCKM Gate](OCKM%20Gate/) | Effect | Tempo-locked 16-step trance gate and pump, with swing, ping-pong and tap-in |
| [OCKM Wave](OCKM%20Wave/) | Instrument | Wavetable synth that teaches as you play: 8 generated wavetables, morph, warp, unison, and 24 lesson presets |
| [OCKM FM Kit](OCKM%20FM%20Kit/) | Instrument | Eight one-shot FM drums (kick, snare, clap, hats, toms, perc) with house and techno kits and a Main/Aux output pair |

## Installing on an MPC

1. Gather the latest build of every plugin into one folder:

   ```bash
   python3 sd_card.py
   ```

   This fills `SD Card/VST2 Plugins/` and warns if any plugin needs rebuilding first. On the SD
   card, open (or create) the `VST2 Plugins` folder and drag **everything inside**
   `SD Card/VST2 Plugins` into it. Don't drag the folder itself onto the card: Finder would replace
   the card's `VST2 Plugins` folder and remove any other plugins on it.
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
