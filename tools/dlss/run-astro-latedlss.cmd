@echo off
rem Astro's Playroom (PPSA01325, 01.905.000): late DLSS 720p -> 1440p (preset M), DLSS frame generation, and the F4 30 fps cap
rem at the game's normal speed (loader\astroFrameRate). The game renders 1280x720 through the "Fixed render resolution 1280x720"
rem mod of its patch file (-Patch <PPSA01325.json>); the output stays 3840x2160. Settings come from profiles\PPSA01325.txt.
rem Arguments go to run-dlss.ps1: -Game <folder>, -StreamlineDir <folder>, -Patch <file>, -Windowed, -Set KYTY_X=1.
rem In the game: Shift+F6 late DLSS on/off, F4 the 30 fps cap, F12 the performance overlay.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dlss.ps1" -LateDlss 1440 -FrameGen 1 -Set KYTY_LATE_DLSS_HDR=1,KYTY_DIRECT_MV=uv,KYTY_DIRECT_MV_SIGN=-1,KYTY_DIRECT_DEPTH_INVERTED=0,KYTY_LATE_JITTER=0,KYTY_SAMPLER_LOD_BIAS=-1,KYTY_DLSS_PRESET=M %*
if %errorlevel% neq 0 pause
