@echo off
rem ASTRO BOT (PPSA21567, 01.019.000): late DLSS from the game's render-size picture (1080p, 1368p with its dynamic
rem resolution) to 1440p, DLSS frame generation, and the F4 30 fps cap at the game's normal speed (loader\astroFrameRate;
rem below 30 fps the game's time step follows the real frame time, so a dip is not slow motion; KYTY_ASTRO_DYNAMIC_DT=0
rem turns that off). Settings come from profiles\PPSA21567.txt.
rem Preset K: measured on an RTX 4060 laptop GPU, M costs about 8 ms per frame, K 3 ms, E 1.5 ms.
rem
rem Game patch (recommended, much less CPU/GPU work in busy scenes): put the community patch file for 01.019.000 next to this
rem script as patches\PPSA21567.json (or set ASTRO_PATCH=<file>) with these mods enabled: "Select the existing non-tiled
rem deferred-lighting renderer", "Disable GI probes and lighting shaders", "Fixed render resolution 1920x1080", and
rem "Models: lower detail (LOD x0.5)". Without a file the game runs unpatched.
rem Live switch: KYTY_LIVE_PATCH=<file> below; write lines such as "lod x0.5 off" (a mod's number or part of its name, then
rem on/off/toggle), "list" or "mark <label>" into that file while the game runs; the answer is in <file>.result.
rem
rem Arguments go to run-dlss.ps1: -Game <folder>, -StreamlineDir <folder>, -Windowed, -Set KYTY_X=1.
rem In the game: Shift+F6 late DLSS on/off, F4 the 30 fps cap, F12 the performance overlay.
set PATCHARG=
if defined ASTRO_PATCH (
  set PATCHARG=-Patch "%ASTRO_PATCH%"
) else if exist "%~dp0patches\PPSA21567.json" (
  set PATCHARG=-Patch "%~dp0patches\PPSA21567.json"
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dlss.ps1" -LateDlss 1440 -FrameGen 1 %PATCHARG% -Set KYTY_LIVE_PATCH=%~dp0live.txt,KYTY_LATE_DLSS_HDR=1,KYTY_DIRECT_MV=uv,KYTY_DIRECT_MV_SIGN=-1,KYTY_DIRECT_DEPTH_INVERTED=0,KYTY_DLSS_PRESET=K %*
if %errorlevel% neq 0 pause
