# DLSS frame generation, the 30 fps cap and late DLSS (this fork)

This fork is Jetsku's U59 emulator with the frame generation work of jerrickus20-code/KytyPS5 added, step by step:

1. **DLSS frame generation** (NVIDIA Streamline, `src/local/frame-gen.cpp`) and the **30 fps cap** (F4). Demon's Souls, an NVIDIA RTX 40 or newer GPU.
2. **Demon's Souls' 1280x720 render size** (`KYTY_RENDER_HEIGHT=720`, `src/loader/demonsSoulsRenderSize.cpp`): the game's performance mode renders its 3D at 1280x720 instead of 2560x1440; the output stays 4K.
3. **Late DLSS** (`run-latedlss-1440.cmd`, `KYTY_LATE_DLSS=1440`): the game's finished 720p picture (its TAA skipped, its bloom and depth of field at 720) upscaled to 1440p by one DLSS super resolution pass before the interface is drawn.

## What you need

- A Windows PC with an NVIDIA RTX 40-series (or newer) GPU and a current driver; hardware-accelerated GPU scheduling on.
- NVIDIA's Streamline runtime (not shipped here): download the Streamline SDK 2.14.x from <https://github.com/NVIDIA-RTX/Streamline/releases> and point `-StreamlineDir` at its `bin\x64` folder (it holds `sl.interposer.dll`). The emulator only loads files signed by NVIDIA.

## Start it

Extract the archive, then run `run-dlss.cmd` (the game folder is asked once and remembered):

```
run-dlss.cmd -Game C:\games\PPSA01341 -StreamlineDir C:\streamline-sdk\bin\x64
```

`run-dlss.ps1` applies `u59-preset.json` like the launcher does, adds `KYTY_FRAMEGEN=1` and starts the game fullscreen. Its console output is also written to `_dlss\run.log`: send that file with a problem report. `-Windowed`, `-FrameGen 0`, `-RenderHeight 720`, `-LateDlss 1440`, `-Set KYTY_X=1,KYTY_Y=2` and `-Patch <file>` are the other options.

`run-latedlss-1440.cmd` is `run-dlss.cmd -LateDlss 1440`: the 3D at 1280x720, late DLSS to 2560x1440 with DLSS anti-aliasing (the game's TAA is not run), frame generation on. `run-dlss.cmd -RenderHeight 720` alone renders at 720 and leaves the upscaling to the game and the window.

## In the game

| Key | What it does |
| --- | --- |
| F4 | The 30 fps cap on or off. With frame generation the screen shows 60: DLSS-G makes one frame between two of the game's, which needs a steady 30 on a 60 Hz display. |
| F12 | The performance overlay (as F2), with a DLSS-G line. |
| F6 | DLAA on or off (`KYTY_DLSS`, not in the late DLSS launcher). |
| Shift+F6 | Late DLSS on or off (off: the game's own TAA and stretch, to compare). |
| Ctrl+F6, Ctrl+Shift+F6 | NIS sharper / softer (`KYTY_NIS`). |

The switches are remembered in `_dlss\toggles.txt`. A line with the switch's new state is shown for a few seconds in the top right corner.

## Switches

| Switch | What it does |
| --- | --- |
| `KYTY_RENDER_HEIGHT=<360-1440>` | The 3D's height in the game's performance mode (16:9, width a multiple of 8). Unset or 1440: the game as it is. |
| `KYTY_LATE_DLSS=<720-2160>` | DLSS super resolution of the finished picture to that height, in place of the game's stretch (needs frame generation). |
| `KYTY_LATE_DLSS_AA=1` | The game's TAA is not run; DLSS anti-aliases the jittered picture as well. |
| `KYTY_LATE_DLSS_HDR=1` | The picture handed to DLSS is HDR (default SDR). |
| `KYTY_DLSS_PRESET=<A-O>` | The DLSS preset (`M`, `K`...). |

## Known limits of this step

- Late DLSS keeps the game's 4K output: DLSS's 2560x1440 picture is stretched to 4K by the game's own final composite and scaled to the screen. The game's own 1440p output mode, the cheat keys and the give items panel of jerrickus20-code/KytyPS5 are not ported yet.
- If the game's final composite continues one of Jetsku's draw runs (`KYTY_DRAW_RUN`), its picture is not upscaled for that frame; the log says `Late DLSS: the final composite continued a draw run`. Then try `-Set KYTY_DRAW_RUN=0`.
- Only Demon's Souls 01.007.000 and 01.005.000's video settings are known to the render size patch; elsewhere the log says `the video settings of a known build not found` and the 2560x1440 is kept (late DLSS then upscales it).
- The window is stretched to the screen as Jetsku does (no aspect-fit option).
