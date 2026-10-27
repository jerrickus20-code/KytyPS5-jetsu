# DLSS frame generation, the 30 fps cap and late DLSS (this fork)

This fork is Jetsku's U59 emulator with the frame generation work of jerrickus20-code/KytyPS5 added, step by step:

1. **DLSS frame generation** (NVIDIA Streamline, `src/local/frame-gen.cpp`) and the **30 fps cap** (F4). Demon's Souls, an NVIDIA RTX 40 or newer GPU.
2. Demon's Souls' 1280x720 render size.
3. Late DLSS: the game's 720p picture upscaled to 1440p by DLSS before the interface is drawn.

## What you need

- A Windows PC with an NVIDIA RTX 40-series (or newer) GPU and a current driver; hardware-accelerated GPU scheduling on.
- NVIDIA's Streamline runtime (not shipped here): download the Streamline SDK 2.14.x from <https://github.com/NVIDIA-RTX/Streamline/releases> and point `-StreamlineDir` at its `bin\x64` folder (it holds `sl.interposer.dll`). The emulator only loads files signed by NVIDIA.

## Start it

Extract the archive, then run `run-dlss.cmd` (the game folder is asked once and remembered):

```
run-dlss.cmd -Game C:\games\PPSA01341 -StreamlineDir C:\streamline-sdk\bin\x64
```

`run-dlss.ps1` applies `u59-preset.json` like the launcher does, adds `KYTY_FRAMEGEN=1` and starts the game fullscreen. Its console output is also written to `_dlss\run.log`: send that file with a problem report. `-Windowed`, `-FrameGen 0`, `-Set KYTY_X=1,KYTY_Y=2` and `-Patch <file>` are the other options.

## In the game

| Key | What it does |
| --- | --- |
| F4 | The 30 fps cap on or off. With frame generation the screen shows 60: DLSS-G makes one frame between two of the game's, which needs a steady 30 on a 60 Hz display. |
| F12 | The performance overlay (as F2), with a DLSS-G line. |
| F6 | DLSS anti-aliasing / late DLSS on or off, once those steps are in. |

The switches are remembered in `_dlss\toggles.txt`. A line with the switch's new state is shown for a few seconds in the top right corner.

## Known limits of this step

- The 1280x720 render size and late DLSS are not in this build yet.
- The window is stretched to the screen as Jetsku does (no aspect-fit option).
