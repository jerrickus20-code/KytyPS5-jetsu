@echo off
rem ASTRO BOT (PPSA21567, 01.019.000): late DLSS from the game's render-size picture (1080p, 1368p with its dynamic
rem resolution) to 1440p (preset K: measured on an RTX 4060 laptop GPU, M costs about 8 ms per frame, K 3 ms, E 1.5 ms), DLSS frame generation, and the F4 30 fps cap at the game's normal speed
rem (loader\astroFrameRate). Settings come from profiles\PPSA21567.txt. KYTY_DRAW_RUN=0: the game's own upscaler pass
rem must not be skipped by a draw run (a late input draw that is not seen leaves that frame unscaled).
rem Arguments go to run-dlss.ps1: -Game <folder>, -StreamlineDir <folder>, -Windowed, -Set KYTY_X=1.
rem In the game: Shift+F6 late DLSS on/off, F4 the 30 fps cap, F12 the performance overlay.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dlss.ps1" -LateDlss 1440 -FrameGen 1 -Set KYTY_DRAW_RUN=0,KYTY_LATE_DLSS_HDR=1,KYTY_DIRECT_MV=uv,KYTY_DIRECT_MV_SIGN=-1,KYTY_DIRECT_DEPTH_INVERTED=0,KYTY_DLSS_PRESET=K %*
if %errorlevel% neq 0 pause
