@echo off
rem Late DLSS: Demon's Souls renders its 3D at 1280x720 (its TAA is skipped, the picture kept jittered), its bloom, depth
rem of field and the rest run at 720, then one DLSS super resolution pass anti-aliases and upscales that finished picture
rem to 2560x1440 before the interface is drawn over it (docs/DLSS.md). The game's output stays 4K. Needs DLSS frame
rem generation (NVIDIA's Streamline runtime: -StreamlineDir <folder>, remembered; or a "streamline" folder next to this file).
rem
rem In the game: Shift+F6 late DLSS on/off (off: the game's own TAA and stretch, to compare), F4 the 30 fps cap (60 shown
rem with frame generation), F12 the performance overlay. The switches are remembered in _dlss\toggles.txt.
rem Arguments go to run-dlss.ps1: -Game <folder>, -StreamlineDir <folder>, -Windowed, -Patch <file>, -Set KYTY_X=1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dlss.ps1" -LateDlss 1440 %*
if %errorlevel% neq 0 pause
