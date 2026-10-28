# A launcher window for the DLSS build: choose the game and a profile (the settings made for that game), adjust them if wanted,
# and start it. It runs run-dlss.ps1 with the chosen settings; nothing else is changed. Start it with run-gui.cmd.
#
# Games and their settings are remembered in _dlss\launcher.json. Profiles:
#   ASTRO BOT                    late DLSS 1080p -> 1440p (preset K), frame generation, 30 fps cap at normal speed
#   Astro's Playroom             late DLSS 720p -> 1440p (preset M), frame generation, 30 fps cap at normal speed
#   Demon's Souls                late DLSS 720p -> 1440p, frame generation
#   Frame generation only        any game: DLSS frame generation and the 30 fps cap, no late DLSS
#   Plain emulator               no DLSS at all
# A game's patch file (the community "Fixed render resolution", renderer, LOD... mods) is chosen with the Patch box; a file
# named patches\<title id>.json next to this script is picked up by itself.
param([switch]$ParseOnly)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$script:root = $PSScriptRoot
$script:state = Join-Path $script:root '_dlss'
New-Item -ItemType Directory -Force $script:state | Out-Null
$script:configFile = Join-Path $script:state 'launcher.json'
$script:runScript = Join-Path $script:root 'run-dlss.ps1'

# ---- profiles ----
$astroSet = 'KYTY_LATE_DLSS_HDR=1,KYTY_DIRECT_MV=uv,KYTY_DIRECT_MV_SIGN=-1,KYTY_DIRECT_DEPTH_INVERTED=0'
$script:profiles = [ordered]@{
	'astrobot' = @{ Name = 'ASTRO BOT: late DLSS 1080p to 1440p, frame generation, 30 fps'; Titles = @('PPSA21567', 'PPSA21564')
		LateDlss = 1440; FrameGen = 1; DlssPreset = 'K'; Set = "$astroSet,KYTY_SAMPLER_LOD_BIAS=-1"
		Note = 'Preset K is the cheapest that still looks clean. Use the community patch file (renderer, GI off, fixed 1080p, LOD x0.5) for the best speed.' }
	'playroom' = @{ Name = "Astro's Playroom: late DLSS 720p to 1440p, frame generation, 30 fps"; Titles = @('PPSA01325')
		LateDlss = 1440; FrameGen = 1; DlssPreset = 'M'; Set = "$astroSet,KYTY_LATE_JITTER=0,KYTY_SAMPLER_LOD_BIAS=-1"
		Note = 'Needs the patch file with the "Fixed render resolution 1280x720" mod.' }
	'demons' = @{ Name = "Demon's Souls: late DLSS 720p to 1440p, frame generation"; Titles = @('PPSA01340', 'PPSA01341', 'PPSA01342')
		LateDlss = 1440; FrameGen = 1; DlssPreset = 'M'; Set = ''
		Note = 'The game renders at 720p; DLSS upscales the finished picture to 1440p.' }
	'framegen' = @{ Name = 'Frame generation only (any game)'; Titles = @()
		LateDlss = 0; FrameGen = 1; DlssPreset = 'M'; Set = ''
		Note = 'DLSS frame generation and the 30 fps cap (F4), without late DLSS.' }
	'plain' = @{ Name = 'Plain emulator (no DLSS)'; Titles = @()
		LateDlss = 0; FrameGen = 0; DlssPreset = 'M'; Set = ''
		Note = 'The emulator as it is, no NVIDIA Streamline.' }
}
$script:knownGames = @{ 'PPSA21567' = 'ASTRO BOT'; 'PPSA21564' = 'ASTRO BOT'; 'PPSA01325' = "Astro's Playroom"
	'PPSA01340' = "Demon's Souls"; 'PPSA01341' = "Demon's Souls"; 'PPSA01342' = "Demon's Souls" }

function Get-ProfileKeyForTitle([string]$title) {
	foreach ($key in $script:profiles.Keys) { if ($script:profiles[$key].Titles -contains $title) { return $key } }
	return 'framegen'
}

# ---- games ----
function Resolve-GamePath([string]$path) {
	if (!$path) { return '' }
	if (Test-Path -LiteralPath $path -PathType Leaf) {
		if ($path -like '*.zar') { return $path }
		if ((Split-Path -Leaf $path) -eq 'eboot.bin') { return (Split-Path -Parent $path) }
		return ''
	}
	if (Test-Path -LiteralPath (Join-Path $path 'eboot.bin')) { return $path }
	$zar = @(Get-ChildItem -LiteralPath $path -Filter '*.zar' -File -ErrorAction SilentlyContinue)
	if ($zar.Count -eq 1) { return $zar[0].FullName }
	return ''
}

function Get-TitleId([string]$path) {
	try {
		if (Test-Path -LiteralPath $path -PathType Container) {
			$sfo = Join-Path $path 'sce_sys\param.sfo'
			if (Test-Path -LiteralPath $sfo) {
				$text = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($sfo))
				$match = [regex]::Match($text, '(PPSA|CUSA)\d{5}')
				if ($match.Success) { return $match.Value }
			}
		}
	} catch { }
	$match = [regex]::Match($path, '(PPSA|CUSA)\d{5}')
	if ($match.Success) { return $match.Value }
	return ''
}

function Get-GameLabel($g) {
	$name = if ($script:knownGames.ContainsKey($g.title)) { $script:knownGames[$g.title] } else { Split-Path -Leaf $g.path }
	$id = if ($g.title) { " [$($g.title)]" } else { '' }
	return "$name$id  -  $($g.path)"
}

function New-Game([string]$path) {
	$title = Get-TitleId $path
	$g = @{ path = $path; title = $title; profile = (Get-ProfileKeyForTitle $title); lateDlss = 0; frameGen = 1; dlssPreset = 'M'
		windowed = $false; patch = ''; set = ''; emuArgs = ''; live = $false }
	Apply-Profile $g $g.profile
	$auto = Join-Path $script:root ("patches\" + $title + '.json')
	if ($title -and (Test-Path -LiteralPath $auto)) { $g.patch = $auto }
	return $g
}

function Apply-Profile($g, [string]$key) {
	$p = $script:profiles[$key]
	$g.profile = $key
	$g.lateDlss = $p.LateDlss
	$g.frameGen = $p.FrameGen
	$g.dlssPreset = $p.DlssPreset
	$g.set = $p.Set
}

# ---- saved settings ----
$script:games = New-Object System.Collections.ArrayList
$script:streamline = ''
$script:lastGame = ''
function Load-Config {
	$legacy = Join-Path $script:state 'streamline.txt'
	if (Test-Path -LiteralPath $legacy) { $script:streamline = (Get-Content -LiteralPath $legacy -Raw).Trim() }
	if (!(Test-Path -LiteralPath $script:configFile)) {
		$old = Join-Path $script:state 'game.txt'
		if (Test-Path -LiteralPath $old) {
			$path = (Get-Content -LiteralPath $old -Raw).Trim()
			if (Resolve-GamePath $path) { [void]$script:games.Add((New-Game $path)) }
		}
		return
	}
	try {
		$json = Get-Content -LiteralPath $script:configFile -Raw | ConvertFrom-Json
		if ($json.streamline) { $script:streamline = [string]$json.streamline }
		if ($json.lastGame) { $script:lastGame = [string]$json.lastGame }
		foreach ($item in @($json.games)) {
			if (!$item.path) { continue }
			$g = New-Game ([string]$item.path)
			foreach ($name in @('title', 'profile', 'dlssPreset', 'patch', 'set', 'emuArgs')) { if ($null -ne $item.$name) { $g[$name] = [string]$item.$name } }
			foreach ($name in @('lateDlss', 'frameGen')) { if ($null -ne $item.$name) { $g[$name] = [int]$item.$name } }
			foreach ($name in @('windowed', 'live')) { if ($null -ne $item.$name) { $g[$name] = [bool]$item.$name } }
			if (!$script:profiles.Contains($g.profile)) { $g.profile = 'framegen' }
			[void]$script:games.Add($g)
		}
	} catch { }
}
function Save-Config {
	try {
		$config = [ordered]@{ streamline = $script:streamline; lastGame = $script:lastGame; games = @($script:games | ForEach-Object { $_ }) }
		$config | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $script:configFile -Encoding UTF8
	} catch { }
}

# ---- the command ----
function Quote([string]$text) {
	if ($text -match '[\s,]') { return ('"' + $text + '"') }
	return $text
}

function Build-Arguments($g) {
	$a = @('-Game', (Quote $g.path), '-FrameGen', "$($g.frameGen)")
	if ($g.frameGen -gt 0 -and $g.lateDlss -gt 0) { $a += @('-LateDlss', "$($g.lateDlss)") }
	if ($g.windowed) { $a += '-Windowed' }
	if ($script:streamline -and $g.frameGen -gt 0) { $a += @('-StreamlineDir', (Quote $script:streamline)) }
	if ($g.patch) { $a += @('-Patch', (Quote $g.patch)) }
	$pairs = @()
	if ($g.set) { $pairs += ($g.set -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
	if ($g.frameGen -gt 0 -and $g.lateDlss -gt 0 -and $g.dlssPreset) { $pairs += "KYTY_DLSS_PRESET=$($g.dlssPreset)" }
	if ($g.live) { $pairs += ('KYTY_LIVE_PATCH=' + (Join-Path $script:state 'live.txt')) }
	if ($pairs.Count -gt 0) { $a += @('-Set', ('"' + ($pairs -join ',') + '"')) }
	if ($g.emuArgs) { $a += @('-EmuArgs', ('"' + $g.emuArgs + '"')) }
	return $a
}

# ---- window ----
$font = New-Object System.Drawing.Font('Segoe UI', 9)
$form = New-Object System.Windows.Forms.Form
$form.Text = 'KytyPS5 DLSS launcher'
$form.Font = $font
$form.AutoScaleDimensions = New-Object System.Drawing.SizeF(96, 96)
$form.AutoScaleMode = 'Dpi'
$form.ClientSize = New-Object System.Drawing.Size(700, 640)
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'

function Add-Label([string]$text, [int]$x, [int]$y, [int]$w = 120) {
	$c = New-Object System.Windows.Forms.Label
	$c.Text = $text; $c.Location = New-Object System.Drawing.Point($x, $y + 3); $c.Size = New-Object System.Drawing.Size($w, 20)
	$form.Controls.Add($c); return $c
}
function Add-Control($c, [int]$x, [int]$y, [int]$w, [int]$h = 24) {
	$c.Location = New-Object System.Drawing.Point($x, $y); $c.Size = New-Object System.Drawing.Size($w, $h)
	$form.Controls.Add($c); return $c
}
function Add-Button([string]$text, [int]$x, [int]$y, [int]$w, [int]$h = 26) {
	$b = New-Object System.Windows.Forms.Button
	$b.Text = $text; $b.UseVisualStyleBackColor = $true
	return (Add-Control $b $x $y $w $h)
}

[void](Add-Label 'Game' 16 14 80)
$gameBox = Add-Control (New-Object System.Windows.Forms.ComboBox) 100 14 574 24
$gameBox.DropDownStyle = 'DropDownList'
$addFolder = Add-Button 'Add folder...' 100 46 110
$addZar = Add-Button 'Add .zar...' 216 46 110
$removeGame = Add-Button 'Remove' 332 46 80
$titleLabel = Add-Label 'Title id' 440 46 56
$titleBox = Add-Control (New-Object System.Windows.Forms.TextBox) 500 47 100 24

[void](Add-Label 'Profile' 16 90 80)
$profileBox = Add-Control (New-Object System.Windows.Forms.ComboBox) 100 90 574 24
$profileBox.DropDownStyle = 'DropDownList'
foreach ($key in $script:profiles.Keys) { [void]$profileBox.Items.Add($script:profiles[$key].Name) }
$noteLabel = Add-Control (New-Object System.Windows.Forms.Label) 100 120 574 36
$noteLabel.ForeColor = [System.Drawing.Color]::DimGray

$group = Add-Control (New-Object System.Windows.Forms.GroupBox) 16 162 668 330
$group.Text = 'Settings'
function Add-InGroup($c, [int]$x, [int]$y, [int]$w, [int]$h = 24) {
	$c.Location = New-Object System.Drawing.Point($x, $y); $c.Size = New-Object System.Drawing.Size($w, $h)
	$group.Controls.Add($c); return $c
}
function Add-GroupLabel([string]$text, [int]$x, [int]$y, [int]$w = 130) {
	$c = New-Object System.Windows.Forms.Label
	$c.Text = $text
	[void](Add-InGroup $c $x ($y + 3) $w 20); return $c
}
[void](Add-GroupLabel 'Late DLSS output' 14 26)
$lateBox = Add-InGroup (New-Object System.Windows.Forms.ComboBox) 150 26 150
$lateBox.DropDownStyle = 'DropDownList'
foreach ($item in @('Off', '1080p', '1440p', '2160p')) { [void]$lateBox.Items.Add($item) }
[void](Add-GroupLabel 'DLSS preset' 330 26 90)
$presetBox = Add-InGroup (New-Object System.Windows.Forms.ComboBox) 424 26 70
$presetBox.DropDownStyle = 'DropDownList'
foreach ($letter in [char[]]'ABCDEFGHIJKLMNO') { [void]$presetBox.Items.Add([string]$letter) }
$presetHint = Add-GroupLabel 'K: light, M: best quality' 504 26 150
$presetHint.ForeColor = [System.Drawing.Color]::DimGray

$frameCheck = Add-InGroup (New-Object System.Windows.Forms.CheckBox) 14 64 300
$frameCheck.Text = 'DLSS frame generation (needs NVIDIA Streamline)'
$windowCheck = Add-InGroup (New-Object System.Windows.Forms.CheckBox) 14 92 300
$windowCheck.Text = 'Windowed (otherwise fullscreen)'
$liveCheck = Add-InGroup (New-Object System.Windows.Forms.CheckBox) 14 120 640
$liveCheck.Text = 'Live patch switch (change patch-file mods while the game runs; see DLSS-README)'

[void](Add-GroupLabel 'Patch file' 14 156)
$patchBox = Add-InGroup (New-Object System.Windows.Forms.TextBox) 150 156 396
$patchBrowse = Add-InGroup (New-Object System.Windows.Forms.Button) 552 155 50 26
$patchBrowse.Text = '...'
$patchClear = Add-InGroup (New-Object System.Windows.Forms.Button) 606 155 48 26
$patchClear.Text = 'Clear'

[void](Add-GroupLabel 'Streamline folder' 14 192)
$slBox = Add-InGroup (New-Object System.Windows.Forms.TextBox) 150 192 396
$slBrowse = Add-InGroup (New-Object System.Windows.Forms.Button) 552 191 50 26
$slBrowse.Text = '...'
$slNote = Add-GroupLabel 'all games' 606 192 54
$slNote.ForeColor = [System.Drawing.Color]::DimGray

[void](Add-GroupLabel 'Extra settings' 14 228)
$setBox = Add-InGroup (New-Object System.Windows.Forms.TextBox) 150 228 504
[void](Add-GroupLabel 'Emulator arguments' 14 264)
$emuBox = Add-InGroup (New-Object System.Windows.Forms.TextBox) 150 264 504
$hint = Add-GroupLabel 'Extra settings: KYTY_X=1,KYTY_Y=2. Emulator arguments: for example --screen-width 2560 --screen-height 1600.' 14 296 640
$hint.ForeColor = [System.Drawing.Color]::DimGray
$hint.Size = New-Object System.Drawing.Size(640, 20)

[void](Add-Label 'Command' 16 502 80)
$preview = Add-Control (New-Object System.Windows.Forms.TextBox) 100 502 574 52
$preview.Multiline = $true; $preview.ReadOnly = $true; $preview.ScrollBars = 'Vertical'

$launch = Add-Button 'Launch' 100 570 130 34
$launch.Font = New-Object System.Drawing.Font('Segoe UI', 10, [System.Drawing.FontStyle]::Bold)
$stop = Add-Button 'Stop' 240 570 80 34
$stop.Enabled = $false
$openLog = Add-Button 'Open log' 340 570 100 34
$openDir = Add-Button 'Open folder' 450 570 110 34
$status = Add-Control (New-Object System.Windows.Forms.Label) 100 612 574 20
$status.ForeColor = [System.Drawing.Color]::DimGray

# ---- state <-> controls ----
$script:loading = $false
$script:running = $null

function Current-Game {
	if ($gameBox.SelectedIndex -ge 0 -and $gameBox.SelectedIndex -lt $script:games.Count) { return $script:games[$gameBox.SelectedIndex] }
	return $null
}

function Refresh-Preview {
	$g = Current-Game
	if ($null -eq $g) { $preview.Text = ''; return }
	$preview.Text = 'run-dlss.ps1 ' + ((Build-Arguments $g) -join ' ')
}

function Read-Controls {
	$g = Current-Game
	if ($null -eq $g -or $script:loading) { return }
	$g.title = $titleBox.Text.Trim()
	$g.lateDlss = @(0, 1080, 1440, 2160)[[Math]::Max(0, $lateBox.SelectedIndex)]
	$g.dlssPreset = [string]$presetBox.SelectedItem
	$g.frameGen = [int]$frameCheck.Checked
	$g.windowed = $windowCheck.Checked
	$g.live = $liveCheck.Checked
	$g.patch = $patchBox.Text.Trim()
	$g.set = $setBox.Text.Trim()
	$g.emuArgs = $emuBox.Text.Trim()
	$script:streamline = $slBox.Text.Trim()
	Refresh-Preview
}

function Show-Game {
	$g = Current-Game
	$script:loading = $true
	try {
		$keys = @($script:profiles.Keys)
		if ($null -eq $g) {
			$titleBox.Text = ''; $patchBox.Text = ''; $setBox.Text = ''; $emuBox.Text = ''; $noteLabel.Text = 'Add a game first.'
			$launch.Enabled = $false
			return
		}
		$launch.Enabled = ($null -eq $script:running)
		$profileBox.SelectedIndex = [Math]::Max(0, $keys.IndexOf($g.profile))
		$noteLabel.Text = $script:profiles[$g.profile].Note
		$titleBox.Text = $g.title
		$lateBox.SelectedIndex = [Math]::Max(0, @(0, 1080, 1440, 2160).IndexOf([int]$g.lateDlss))
		$presetBox.SelectedItem = $g.dlssPreset
		if ($presetBox.SelectedIndex -lt 0) { $presetBox.SelectedItem = 'M' }
		$frameCheck.Checked = [bool]$g.frameGen
		$windowCheck.Checked = [bool]$g.windowed
		$liveCheck.Checked = [bool]$g.live
		$patchBox.Text = $g.patch
		$setBox.Text = $g.set
		$emuBox.Text = $g.emuArgs
		$slBox.Text = $script:streamline
	} finally { $script:loading = $false }
	Refresh-Preview
}

function Refill-Games([string]$select) {
	$script:loading = $true
	try {
		$gameBox.Items.Clear()
		foreach ($g in $script:games) { [void]$gameBox.Items.Add((Get-GameLabel $g)) }
		$index = 0
		for ($i = 0; $i -lt $script:games.Count; $i++) { if ($script:games[$i].path -eq $select) { $index = $i } }
		if ($script:games.Count -gt 0) { $gameBox.SelectedIndex = $index }
	} finally { $script:loading = $false }
	Show-Game
}

function Add-GameFromPath([string]$path) {
	$resolved = Resolve-GamePath $path
	if (!$resolved) {
		[void][System.Windows.Forms.MessageBox]::Show("No game found in '$path' (needs eboot.bin, or a single .zar archive).", 'Add game', 'OK', 'Warning')
		return
	}
	foreach ($g in $script:games) { if ($g.path -eq $resolved) { Refill-Games $resolved; return } }
	[void]$script:games.Add((New-Game $resolved))
	$script:lastGame = $resolved
	Refill-Games $resolved
	Save-Config
}

$gameBox.Add_SelectedIndexChanged({
	if ($script:loading) { return }
	$g = Current-Game
	if ($null -ne $g) { $script:lastGame = $g.path }
	Show-Game
})
$profileBox.Add_SelectedIndexChanged({
	if ($script:loading) { return }
	$g = Current-Game
	if ($null -eq $g) { return }
	$key = @($script:profiles.Keys)[$profileBox.SelectedIndex]
	Apply-Profile $g $key
	Show-Game
})
foreach ($control in @($titleBox, $patchBox, $setBox, $emuBox, $slBox)) { $control.Add_TextChanged({ Read-Controls }) }
foreach ($control in @($lateBox, $presetBox)) { $control.Add_SelectedIndexChanged({ Read-Controls }) }
foreach ($control in @($frameCheck, $windowCheck, $liveCheck)) { $control.Add_CheckedChanged({ Read-Controls }) }

$addFolder.Add_Click({
	$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
	$dialog.Description = 'Choose the game folder (the one with eboot.bin and sce_sys), or a folder with one .zar archive'
	if ($dialog.ShowDialog() -eq 'OK') { Add-GameFromPath $dialog.SelectedPath }
})
$addZar.Add_Click({
	$dialog = New-Object System.Windows.Forms.OpenFileDialog
	$dialog.Filter = 'Game archive (*.zar)|*.zar|eboot.bin|eboot.bin|All files (*.*)|*.*'
	if ($dialog.ShowDialog() -eq 'OK') { Add-GameFromPath $dialog.FileName }
})
$removeGame.Add_Click({
	$g = Current-Game
	if ($null -eq $g) { return }
	$script:games.Remove($g)
	Refill-Games ''
	Save-Config
})
$patchBrowse.Add_Click({
	$dialog = New-Object System.Windows.Forms.OpenFileDialog
	$dialog.Filter = 'Game patch (*.json)|*.json|All files (*.*)|*.*'
	if ($dialog.ShowDialog() -eq 'OK') { $patchBox.Text = $dialog.FileName }
})
$patchClear.Add_Click({ $patchBox.Text = '' })
$slBrowse.Add_Click({
	$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
	$dialog.Description = "NVIDIA Streamline's runtime folder (the one with sl.interposer.dll; the SDK's bin\x64)"
	if ($dialog.ShowDialog() -eq 'OK') { $slBox.Text = $dialog.SelectedPath }
})
$openLog.Add_Click({
	$log = Join-Path $script:state 'run.log'
	if (Test-Path -LiteralPath $log) { Start-Process notepad.exe -ArgumentList ('"' + $log + '"') } else { $status.Text = 'No log yet (_dlss\run.log).' }
})
$openDir.Add_Click({ Start-Process explorer.exe -ArgumentList ('"' + $script:root + '"') })

# ---- start and watch the game ----
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 1000
$timer.Add_Tick({
	if ($null -eq $script:running) { return }
	if (!$script:running.HasExited) { return }
	$code = $script:running.ExitCode
	$script:running = $null
	$stop.Enabled = $false
	$launch.Enabled = ($null -ne (Current-Game))
	if ($code -ne 0) {
		$text = ''
		foreach ($name in @('launcher-error.txt', 'run-error.log')) {
			$file = Join-Path $script:state $name
			if (Test-Path -LiteralPath $file) { $text += (@(Get-Content -LiteralPath $file -Tail 12) -join "`r`n") + "`r`n" }
		}
		$status.Text = "The game ended with code $code."
		[void][System.Windows.Forms.MessageBox]::Show("The game ended with code $code.`r`n`r`n$text`r`nThe full log is _dlss\run.log (Open log).", 'KytyPS5', 'OK', 'Warning')
	} else {
		$status.Text = 'The game ended.'
	}
})
$timer.Start()

$launch.Add_Click({
	$g = Current-Game
	if ($null -eq $g -or $null -ne $script:running) { return }
	Read-Controls
	if (!(Resolve-GamePath $g.path)) { [void][System.Windows.Forms.MessageBox]::Show("The game is no longer at '$($g.path)'.", 'Launch', 'OK', 'Warning'); return }
	if ($g.patch -and !(Test-Path -LiteralPath $g.patch)) { [void][System.Windows.Forms.MessageBox]::Show("The patch file '$($g.patch)' does not exist.", 'Launch', 'OK', 'Warning'); return }
	if ($g.frameGen -gt 0 -and !$script:streamline -and !(Test-Path -LiteralPath (Join-Path $script:root 'streamline'))) {
		$answer = [System.Windows.Forms.MessageBox]::Show("Frame generation needs NVIDIA's Streamline runtime (sl.interposer.dll) and no folder is set. Choose it with the '...' button next to Streamline folder.`r`n`r`nStart anyway?", 'Launch', 'YesNo', 'Question')
		if ($answer -ne 'Yes') { return }
	}
	$script:lastGame = $g.path
	Save-Config
	$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
	$argumentText = '-NoProfile -ExecutionPolicy Bypass -File "' + $script:runScript + '" ' + ((Build-Arguments $g) -join ' ')
	$errorFile = Join-Path $script:state 'launcher-error.txt'
	$outFile = Join-Path $script:state 'launcher-out.txt'
	try {
		$script:running = Start-Process -FilePath $powershell -ArgumentList $argumentText -WorkingDirectory $script:root -WindowStyle Hidden -PassThru `
			-RedirectStandardError $errorFile -RedirectStandardOutput $outFile
		$launch.Enabled = $false
		$stop.Enabled = $true
		$status.Text = 'Running: ' + (Get-GameLabel $g)
	} catch {
		$script:running = $null
		[void][System.Windows.Forms.MessageBox]::Show("Could not start: $($_.Exception.Message)", 'Launch', 'OK', 'Error')
	}
})
$stop.Add_Click({
	if ($null -eq $script:running) { return }
	try { & taskkill.exe /T /F /PID $script:running.Id | Out-Null } catch { }
	foreach ($process in @(Get-Process -Name 'kyty_emulator' -ErrorAction SilentlyContinue)) { try { $process.Kill() } catch { } }
	$status.Text = 'Stopped.'
})

$form.Add_FormClosing({
	Read-Controls
	Save-Config
})

if ($ParseOnly) { return }
Load-Config
Refill-Games $script:lastGame
if ($script:games.Count -eq 0) { $status.Text = "Add a game with 'Add folder...' or 'Add .zar...'." }
[void]$form.ShowDialog()
