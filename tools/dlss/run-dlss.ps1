# Starts the emulator with DLSS frame generation (NVIDIA Streamline) and the in-game switches of docs/DLSS.md.
#
#   .\run-dlss.ps1 -Game C:\path\to\game   the game folder (the one with eboot.bin) or its .zar archive; remembered in _dlss\game.txt
#   .\run-dlss.ps1 -StreamlineDir <dir>    where NVIDIA's Streamline runtime is (sl.interposer.dll; the SDK's bin\x64);
#                                          remembered in _dlss\streamline.txt, else a "streamline" folder next to this file
#   .\run-dlss.ps1 -FrameGen 0             without frame generation (the cap and the overlay line stay)
#   .\run-dlss.ps1 -Windowed               a window instead of fullscreen
#   .\run-dlss.ps1 -Discover              writes what a game's profile needs to _dlss\run.log (KYTY_PROFILE_DISCOVER; docs/DLSS.md,
#                                          "Game profiles"); run the game for a minute in a scene, then send the log
#   .\run-dlss.ps1 -RenderHeight 720       Demon's Souls' 3D at 1280x720 (KYTY_RENDER_HEIGHT; the output stays 4K)
#   .\run-dlss.ps1 -LateDlss 1440          late DLSS: the finished 720p picture upscaled by DLSS to 1440p before the
#                                          interface (KYTY_LATE_DLSS, with DLSS anti-aliasing, the game's TAA not run);
#                                          sets -RenderHeight 720 unless given
#   .\run-dlss.ps1 -Set KYTY_A=1,KYTY_B=2  more emulator switches (KYTY_*), after the preset's
#   .\run-dlss.ps1 -Patch <file.json>      a game patch (--game-patch)
#   .\run-dlss.ps1 -EmuArgs '--skip-notice-screen true'   more emulator command-line arguments, split at spaces
# The emulator's console output goes to _dlss\run.log as well; send that file with a problem report.
param(
	[string]$Game = '',
	[string]$StreamlineDir = '',
	[int]$FrameGen = 1,
	[switch]$Windowed,
	[switch]$NoAslrFix,
	[switch]$Discover,
	[int]$RenderHeight = 0,
	[int]$LateDlss = 0,
	[string]$Set = '',
	[string]$Patch = '',
	[string]$EmuArgs = '',
	[string]$EmuExe = ''
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'kyty_emulator.exe'
if ($EmuExe) { $exe = if ([IO.Path]::IsPathRooted($EmuExe)) { $EmuExe } else { Join-Path $root $EmuExe } }  # another emulator exe, to compare builds
if (!(Test-Path -LiteralPath $exe)) { throw "kyty_emulator.exe is not next to run-dlss.ps1 ($root)" }
$state = Join-Path $root '_dlss'
New-Item -ItemType Directory -Force $state | Out-Null

# A game is a folder with eboot.bin, or a ZArchive (.zar) file, or a folder that holds one .zar (the emulator reads archives itself).
function Resolve-Game([string]$path) {
	if (!$path) { return '' }
	if (Test-Path -LiteralPath $path -PathType Leaf) { if ($path -like '*.zar') { return $path } else { return '' } }
	if (Test-Path -LiteralPath (Join-Path $path 'eboot.bin')) { return $path }
	$zar = @(Get-ChildItem -LiteralPath $path -Filter '*.zar' -File -ErrorAction SilentlyContinue)
	if ($zar.Count -eq 1) { return $zar[0].FullName }
	return ''
}
function Test-Game([string]$path) { [bool](Resolve-Game $path) }

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
if (!(Test-Game $Game)) { throw "no eboot.bin (or single .zar archive) in '$Game'" }
if ($remember) { Set-Content -LiteralPath $gameFile -Value $Game -Encoding UTF8 }
$Game = Resolve-Game $Game

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
if ($LateDlss -gt 0) {
	if ($FrameGen -le 0) { throw '-LateDlss needs frame generation (-FrameGen 1): both use NVIDIA Streamline' }
	if ($RenderHeight -le 0) { $RenderHeight = 720 }
	$env:KYTY_LATE_DLSS = "$LateDlss"
	$env:KYTY_LATE_DLSS_AA = '1'
	if (!$env:KYTY_DLSS_PRESET) { $env:KYTY_DLSS_PRESET = 'M' }
}
if ($RenderHeight -gt 0) { $env:KYTY_RENDER_HEIGHT = "$RenderHeight" }
if ($Discover) { $env:KYTY_PROFILE_DISCOVER = '1' }
$env:KYTY_LOG_UNBUFFERED = '1'
if (!$env:KYTY_TOGGLE_FILE) { $env:KYTY_TOGGLE_FILE = (Join-Path $state 'toggles.txt') }
foreach ($pair in ($Set -split ',' | Where-Object { $_ })) {
	$index = $pair.IndexOf('=')
	if ($index -lt 1) { throw "-Set takes KEY=VALUE pairs, not '$pair'" }
	Set-Item -LiteralPath ("Env:" + $pair.Substring(0, $index).Trim()) -Value $pair.Substring($index + 1)
}

$arguments = @('--game', $Game)
if (!$Windowed) { $arguments += '--fullscreen' }
if ($EmuArgs) { $arguments += ($EmuArgs -split '\s+' | Where-Object { $_ }) }
if ($Patch) { $arguments += @('--game-patch', (Resolve-Path -LiteralPath $Patch).Path) }

Write-Host "game: $Game"
if ($RenderHeight -gt 0) { Write-Host "render height: $RenderHeight$(if ($LateDlss -gt 0) { "; late DLSS to $LateDlss" })" }
Write-Host "frame generation: $(if ($FrameGen -gt 0) { "on ($FrameGen generated frame per rendered one)" } else { 'off' }); Streamline: $(if ($StreamlineDir) { $StreamlineDir } else { '(streamline folder next to the emulator)' })"
Set-Location -LiteralPath $root
# The emulator's console output goes to run.log (and its errors to run-error.log); launch.txt says how it ended.
$runLog = Join-Path $state 'run.log'
$errLog = Join-Path $state 'run-error.log'
$launch = Join-Path $state 'launch.txt'
$quoted = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
"$(Get-Date -Format 'HH:mm:ss') starting: $exe $quoted" | Set-Content -LiteralPath $launch
# DLSS frame generation: NVIDIA's driver can refuse the Vulkan device Streamline asks for when the emulator runs without
# bottom-up ASLR (issue #45). The emulator is started here with it forced on for this one process (a process creation
# attribute: no Windows setting is changed). -NoAslrFix starts it the plain way.
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class KytyLauncher {
	[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
	struct STARTUPINFO { public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
		public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
		public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError; }
	[StructLayout(LayoutKind.Sequential)] struct STARTUPINFOEX { public STARTUPINFO StartupInfo; public IntPtr lpAttributeList; }
	[StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
	[StructLayout(LayoutKind.Sequential)] struct SECURITY_ATTRIBUTES { public int nLength; public IntPtr lpSecurityDescriptor; public int bInheritHandle; }
	[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
	static extern IntPtr CreateFileW(string name, uint access, uint share, ref SECURITY_ATTRIBUTES sa, uint disposition, uint flags, IntPtr template);
	[DllImport("kernel32.dll", SetLastError = true)]
	static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, int flags, ref IntPtr size);
	[DllImport("kernel32.dll", SetLastError = true)]
	static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
	[DllImport("kernel32.dll")] static extern void DeleteProcThreadAttributeList(IntPtr list);
	[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
	static extern bool CreateProcessW(string app, string command, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFOEX si, out PROCESS_INFORMATION pi);
	[DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
	[DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr handle, out uint code);
	[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);

	static IntPtr OpenLog(string path) {
		var sa = new SECURITY_ATTRIBUTES(); sa.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES)); sa.bInheritHandle = 1;
		IntPtr handle = CreateFileW(path, 0x40000000u, 3u, ref sa, 2u, 0x80u, IntPtr.Zero);
		if (handle == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
		return handle;
	}

	// Starts the program with bottom-up ASLR forced on, its console output in two files; returns its exit code.
	public static int Run(string exe, string arguments, string directory, string outPath, string errPath) {
		IntPtr output = OpenLog(outPath), error = OpenLog(errPath), list = IntPtr.Zero, policy = IntPtr.Zero;
		var info = new PROCESS_INFORMATION();
		try {
			IntPtr size = IntPtr.Zero;
			InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
			list = Marshal.AllocHGlobal(size);
			if (!InitializeProcThreadAttributeList(list, 1, 0, ref size)) throw new Win32Exception(Marshal.GetLastWin32Error());
			policy = Marshal.AllocHGlobal(8);
			Marshal.WriteInt64(policy, 0x10000L); // PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
			if (!UpdateProcThreadAttribute(list, 0, new IntPtr(0x20007), policy, new IntPtr(8), IntPtr.Zero, IntPtr.Zero))
				throw new Win32Exception(Marshal.GetLastWin32Error());
			var si = new STARTUPINFOEX();
			si.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
			si.StartupInfo.dwFlags = 0x100; // STARTF_USESTDHANDLES
			si.StartupInfo.hStdOutput = output; si.StartupInfo.hStdError = error;
			si.lpAttributeList = list;
			if (!CreateProcessW(exe, "\"" + exe + "\" " + arguments, IntPtr.Zero, IntPtr.Zero, true, 0x80000u, IntPtr.Zero, directory, ref si, out info))
				throw new Win32Exception(Marshal.GetLastWin32Error());
			WaitForSingleObject(info.hProcess, 0xFFFFFFFFu);
			uint code; GetExitCodeProcess(info.hProcess, out code);
			return unchecked((int)code);
		} finally {
			if (info.hProcess != IntPtr.Zero) CloseHandle(info.hProcess);
			if (info.hThread != IntPtr.Zero) CloseHandle(info.hThread);
			if (list != IntPtr.Zero) { DeleteProcThreadAttributeList(list); Marshal.FreeHGlobal(list); }
			if (policy != IntPtr.Zero) Marshal.FreeHGlobal(policy);
			CloseHandle(output); CloseHandle(error);
		}
	}
}
'@
$code = $null
if ($FrameGen -gt 0 -and !$NoAslrFix) {
	try { $code = [KytyLauncher]::Run($exe, $quoted, $root, $runLog, $errLog) }
	catch { Add-Content -LiteralPath $launch -Value "bottom-up ASLR launch failed ($($_.Exception.Message)): starting the plain way" }
}
if ($null -eq $code) {
	$process = Start-Process -FilePath $exe -ArgumentList $quoted -WorkingDirectory $root -PassThru -NoNewWindow `
		-RedirectStandardOutput $runLog -RedirectStandardError $errLog
	$process.WaitForExit()
	$code = $process.ExitCode
}
"$(Get-Date -Format 'HH:mm:ss') emulator exited with code $code (0x$('{0:X}' -f $code))" | Add-Content -LiteralPath $launch
Write-Host (Get-Content -LiteralPath $launch -Raw)
exit $code
