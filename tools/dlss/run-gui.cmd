@echo off
rem A launcher window: choose the game and its profile (late DLSS, frame generation, 30 fps cap, patch file...), then Launch.
rem It runs run-dlss.ps1 with those settings; the settings are remembered in _dlss\launcher.json (see DLSS-README.md).
rem This console stays open only if the launcher fails to start, and then shows why (also in _dlss\launcher-crash.txt).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-gui.ps1"
if %errorlevel% neq 0 pause
