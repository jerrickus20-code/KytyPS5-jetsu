@echo off
rem DLSS frame generation and the 30 fps cap (docs/DLSS.md). Arguments go to run-dlss.ps1: -Game <folder>,
rem -StreamlineDir <folder>, -FrameGen 0, -Windowed, -Set KYTY_X=1, -Patch <file>.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-dlss.ps1" %*
if %errorlevel% neq 0 pause
