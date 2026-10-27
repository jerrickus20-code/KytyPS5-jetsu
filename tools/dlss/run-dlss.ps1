# Starts the emulator with DLSS frame generation (NVIDIA Streamline) and the in-game switches of docs/DLSS.md.
#
#   .\run-dlss.ps1 -Game C:\path\to\game   the game folder (the one with eboot.bin); remembered in _dlss\game.txt
#   .\run-dlss.ps1 -StreamlineDir <dir>    where NVIDIA's Streamline runtime is (sl.interposer.dll; the SDK's bin\x64);
#                                          remembered in _dlss\streamline.txt, else a "streamline" folder next to this file
#   .\run-dlss.ps1 -FrameGen 0             without frame generation (the cap and the overlay line stay)
#   .\run-dlss.ps1 -Windowed               a window instead of fullscreen
#   .\run-dlss.ps1 -Set KYTY_A=1,KYTY_B=2  more emulator switches (KYTY_*), after the preset's
#   .\run-dlss.ps1 -Patch <file.json>      a game patch (--game-patch)
# The emulator's console output goes to _dlss\run.log as well; send that file with a problem report.
param(
	[string]$Game = '',
	[string]$StreamlineDir = '',
	[int]$FrameGen = 1,
	[switch]$Windowed,
	[string]$Set = '',
	[string]$Patch = ''
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'kyty_emulator.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "kyty_emulator.exe is not next to run-dlss.ps1 ($root)" }
$state = Join-Path $root '_dlss'
New-Item -ItemType Directory -Force $state | Out-Null

function Test-Game([string]$path) { $path -and (Test-Path -LiteralPath (Join-Path $path 'eboot.bin')) }

# The game: -Game, else the last one given, else a folder dialog.
$gameFile = Join-Path $state 'game.txt'
$remember = [bool]$Game
if (!$Game -and (Test-Path -LiteralPath $gameFile)) { $Game = (Get-Content -LiteralPath $gameFile -Raw).Trim() }
if (!(Test-Game $Game)) {
	Add-Type -AssemblyName System.Windows.Forms
	$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
	$dialog.Description = 'Choose the game folder (the one with eboot.bin and sce_sys)'
	if ($dialog.ShowDialog() -eq 'OK') { $Game = $dialog.SelectedPath; $remember = $true }
}
if (!(Test-Game $Game)) { throw "no eboot.bin in '$Game'" }
if ($remember) { Set-Content -LiteralPath $gameFile -Value $Game -Encoding UTF8 }

# The Streamline runtime.
$slFile = Join-Path $state 'streamline.txt'
if ($StreamlineDir) {
	Set-Content -LiteralPath $slFile -Value $StreamlineDir -Encoding UTF8
} elseif (Test-Path -LiteralPath $slFile) {
	$StreamlineDir = (Get-Content -LiteralPath $slFile -Raw).Trim()
}

# The release settings (u59-preset.json: the launcher applies them to its game, so a direct start does too).
foreach ($name in @(Get-ChildItem Env: | Where-Object { $_.Name -like 'KYTY_*' -or $_.Name -like 'TRACY_*' } | ForEach-Object { $_.Name })) {
	Remove-Item -LiteralPath "Env:$name"
}
$presetFile = Join-Path $root 'u59-preset.json'
if (Test-Path -LiteralPath $presetFile) {
	$preset = Get-Content -LiteralPath $presetFile -Raw | ConvertFrom-Json
	foreach ($property in $preset.PSObject.Properties) { Set-Item -LiteralPath "Env:$($property.Name)" -Value ([string]$property.Value) }
} else {
	Write-Host "warning: no u59-preset.json next to run-dlss.ps1: the emulator runs with its defaults"
}

# DLSS frame generation and the in-game switches (src/local/frame-gen.h).
if ($FrameGen -gt 0) { $env:KYTY_FRAMEGEN = "$FrameGen" }
if ($StreamlineDir) { $env:KYTY_STREAMLINE_DIR = $StreamlineDir }
if (!$env:KYTY_TOGGLE_FILE) { $env:KYTY_TOGGLE_FILE = (Join-Path $state 'toggles.txt') }
foreach ($pair in ($Set -split ',' | Where-Object { $_ })) {
	$index = $pair.IndexOf('=')
	if ($index -lt 1) { throw "-Set takes KEY=VALUE pairs, not '$pair'" }
	Set-Item -LiteralPath ("Env:" + $pair.Substring(0, $index).Trim()) -Value $pair.Substring($index + 1)
}

$arguments = @('--game', $Game)
if (!$Windowed) { $arguments += '--fullscreen' }
if ($Patch) { $arguments += @('--game-patch', (Resolve-Path -LiteralPath $Patch).Path) }

Write-Host "game: $Game"
Write-Host "frame generation: $(if ($FrameGen -gt 0) { "on ($FrameGen generated frame per rendered one)" } else { 'off' }); Streamline: $(if ($StreamlineDir) { $StreamlineDir } else { '(streamline folder next to the emulator)' })"
Set-Location -LiteralPath $root
& $exe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $state 'run.log')
exit $LASTEXITCODE
