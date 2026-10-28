@echo off
rem A launcher window: choose the game and its profile (late DLSS, frame generation, 30 fps cap, patch file...), then Launch.
rem It runs run-dlss.ps1 with those settings; the settings are remembered in _dlss\launcher.json (see DLSS-README.md).
rem If the launcher itself fails, a message box shows why (also in _dlss\launcher-crash.txt).
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0run-gui.ps1"
