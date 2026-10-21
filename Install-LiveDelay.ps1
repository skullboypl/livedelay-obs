# LiveDelay installer for end users (Windows x64).
# Run from the unzipped release (next to obs-plugins\ and data\), or from this repo.
# Prefer ProgramData (no admin). Portable: -IntoObsFolder -ObsRoot "D:\obs-studio"
# ASCII-only. UTF-8 BOM OK for -File; do not serve via irm|iex.
# Always return Object[] - a single string makes $roots[0] mean the char 'C'.
#Requires -Version 5.1
[CmdletBinding()]
param(
	[string]$ObsRoot = '',
	[switch]$IntoObsFolder,
	[switch]$NoPrompt,
	[switch]$SkipVcRedist
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms | Out-Null

function Write-Info([string]$m) { Write-Host $m -ForegroundColor Cyan }
function Write-Ok([string]$m) { Write-Host $m -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host $m -ForegroundColor Yellow }
function Write-Err([string]$m) { Write-Host $m -ForegroundColor Red }

function Test-IsAdmin {
	$p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
	return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Find-ReleasePayload {
	$here = $PSScriptRoot
	$candidates = @(
		@{ Dll = Join-Path $here 'obs-plugins\64bit\livedelay-obs.dll'; Data = Join-Path $here 'data\obs-plugins\livedelay-obs' },
		@{ Dll = Join-Path $here 'obs-plugin\build\Release\livedelay-obs.dll'; Data = Join-Path $here 'obs-plugin\data' }
	)
	foreach ($c in $candidates) {
		if ((Test-Path -LiteralPath $c.Dll) -and (Test-Path -LiteralPath $c.Data)) { return $c }
	}
	throw "livedelay-obs.dll + data not found. Run this script from the unzipped LiveDelay zip (next to obs-plugins and data)."
}

function Get-ObsVersion([string]$obsExe) {
	try {
		return [version](Get-Item -LiteralPath $obsExe).VersionInfo.FileVersion
	} catch {
		return $null
	}
}

function Find-ObsRoots {
	$list = New-Object System.Collections.Generic.List[string]
	$add = {
		param($p)
		if (-not $p) { return }
		$p = [Environment]::ExpandEnvironmentVariables($p).TrimEnd('\')
		$exe = Join-Path $p 'bin\64bit\obs64.exe'
		if (Test-Path -LiteralPath $exe) { [void]$list.Add($p) }
	}
	& $add 'C:\Program Files\obs-studio'
	& $add 'C:\Program Files (x86)\obs-studio'
	foreach ($key in @('HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio')) {
		if (Test-Path $key) {
			$val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).'(default)'
			if (-not $val) { $val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).InstallPath }
			& $add $val
		}
	}
	Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
		ForEach-Object {
			$p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
			if ($p.DisplayName -match 'OBS Studio' -and $p.InstallLocation) { & $add $p.InstallLocation }
		}
	foreach ($s in @(
		'C:\Program Files (x86)\Steam\steamapps\common\OBS Studio',
		'C:\Program Files\Steam\steamapps\common\OBS Studio'
	)) { & $add $s }
	# Always return Object[] - a single string makes $roots[0] mean the char 'C'.
	return @($list | Select-Object -Unique)
}

function Pick-ObsRoot {
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = 'Select OBS folder (the one with bin\64bit\obs64.exe) or portable root.'
	$dlg.ShowNewFolderButton = $false
	if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
	$root = $dlg.SelectedPath.TrimEnd('\')
	if (Test-Path -LiteralPath (Join-Path $root 'bin\64bit\obs64.exe')) { return $root }
	if (Test-Path -LiteralPath (Join-Path $root 'obs64.exe')) {
		return (Split-Path (Split-Path $root -Parent) -Parent)
	}
	Write-Warn 'Selected folder has no bin\64bit\obs64.exe.'
	return $null
}

function Test-VcRedistX64 {
	foreach ($key in @(
		'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
		'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64'
	)) {
		if (Test-Path $key) {
			$p = Get-ItemProperty $key -ErrorAction SilentlyContinue
			if ($p -and $p.Installed -eq 1 -and [int]$p.Major -ge 14) {
				$ver = if ($p.Version) { [string]$p.Version } else { "v$($p.Major).$($p.Minor).$($p.Bld)" }
				return @{ Ok = $true; Version = $ver }
			}
		}
	}
	$need = @('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')
	$sys = Join-Path $env:SystemRoot 'System32'
	$missing = @($need | Where-Object { -not (Test-Path -LiteralPath (Join-Path $sys $_)) })
	if ($missing.Count -eq 0) {
		return @{ Ok = $true; Version = 'System32 DLLs present' }
	}
	return @{ Ok = $false; Version = ''; Missing = $missing }
}

function Install-VcRedistX64 {
	$url = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
	$out = Join-Path $env:TEMP ('vc_redist.x64-' + [guid]::NewGuid().ToString('N') + '.exe')
	Write-Info "Downloading Microsoft Visual C++ Redistributable x64..."
	Write-Host "  $url"
	Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
	Write-Info "Installing VC++ Redistributable (quiet)..."
	$args = @('/install', '/quiet', '/norestart')
	try {
		if (Test-IsAdmin) {
			$p = Start-Process -FilePath $out -ArgumentList $args -Wait -PassThru
		} else {
			Write-Warn "Elevation required for VC++ install - accept the UAC prompt."
			$p = Start-Process -FilePath $out -ArgumentList $args -Wait -PassThru -Verb RunAs
		}
	} catch {
		Write-Err "Could not start VC++ installer: $_"
		Write-Err "Install manually: $url"
		return $false
	} finally {
		Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
	}
	$code = if ($null -ne $p) { [int]$p.ExitCode } else { -1 }
	# 0 = ok, 1638 = newer already installed, 3010 = reboot required
	if ($code -eq 0 -or $code -eq 1638 -or $code -eq 3010) {
		Write-Ok "VC++ Redistributable OK (exit $code)."
		if ($code -eq 3010) { Write-Warn "Windows may need a reboot before OBS loads the plugin." }
		return $true
	}
	Write-Err "VC++ installer failed (exit $code). Manual: $url"
	return $false
}

function Ensure-VcRedist {
	param([switch]$Skip)
	Write-Info "Checking Visual C++ Redistributable x64 (required by livedelay-obs)..."
	$st = Test-VcRedistX64
	if ($st.Ok) {
		Write-Ok "VC++ runtime: $($st.Version)"
		return $true
	}
	Write-Warn "VC++ runtime missing: $($st.Missing -join ', ')"
	if ($Skip) {
		Write-Warn "SkipVcRedist set - OBS will fail to load the plugin until you install VC++ x64."
		return $false
	}
	$ok = Install-VcRedistX64
	$st2 = Test-VcRedistX64
	if (-not $st2.Ok) {
		Write-Err "VC++ still missing after install attempt."
		Write-Err "Download: https://aka.ms/vs/17/release/vc_redist.x64.exe"
		return $false
	}
	Write-Ok "VC++ runtime: $($st2.Version)"
	return $ok
}

function Remove-ConflictingLivedelay([string]$obsRoot) {
	# Old manual unzip into Program Files often leaves a broken DLL that OBS still tries to load.
	$targets = New-Object System.Collections.Generic.List[string]
	if ($obsRoot) {
		[void]$targets.Add((Join-Path $obsRoot 'obs-plugins\64bit\livedelay-obs.dll'))
		[void]$targets.Add((Join-Path $obsRoot 'data\obs-plugins\livedelay-obs'))
	}
	foreach ($t in $targets) {
		if (Test-Path -LiteralPath $t) {
			Write-Warn "Removing conflicting old copy: $t"
			try {
				Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction Stop
			} catch {
				Write-Warn "Could not remove (permissions?): $t"
				Write-Warn "$_"
			}
		}
	}
}

function Test-PluginLoad([string]$dllPath, [string]$obsRoot) {
	if (-not $obsRoot) { return 'skip (no OBS path)' }
	$bin = Join-Path $obsRoot 'bin\64bit'
	if (-not (Test-Path -LiteralPath (Join-Path $bin 'obs.dll'))) {
		return 'skip (obs.dll not found next to obs64.exe)'
	}
	if (-not ([Management.Automation.PSTypeName]'LiveDelayNativeLoad').Type) {
		Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class LiveDelayNativeLoad {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern bool SetDllDirectory(string lpPathName);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern IntPtr LoadLibrary(string lpFileName);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool FreeLibrary(IntPtr hModule);
}
"@
	}
	$prevOk = [LiveDelayNativeLoad]::SetDllDirectory($bin)
	try {
		$h = [LiveDelayNativeLoad]::LoadLibrary($dllPath)
		if ($h -eq [IntPtr]::Zero) {
			$err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
			return "LoadLibrary failed Win32=$err (missing VC++ / wrong OBS / corrupt DLL)"
		}
		[void][LiveDelayNativeLoad]::FreeLibrary($h)
		return $null
	} finally {
		[void][LiveDelayNativeLoad]::SetDllDirectory($null)
	}
}

function Install-ToProgramData([string]$dllSrc, [string]$dataSrc, [version]$obsVer) {
	$root = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs'
	$dataDst = Join-Path $root 'data'
	$legacyBin = Join-Path $root 'bin\64bit'
	New-Item -ItemType Directory -Force -Path $dataDst, $legacyBin | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $legacyBin 'livedelay-obs.dll') -Force
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $root 'livedelay-obs.dll') -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force
	Write-Ok "Installed to ProgramData (no admin):"
	Write-Host "  $root"
	if ($obsVer -and $obsVer.Major -ge 33) {
		Write-Info "OBS $($obsVer.Major)+ -> flat DLL + legacy bin\64bit."
	} else {
		Write-Info "OBS loads: ...\livedelay-obs\bin\64bit\livedelay-obs.dll"
	}
	return $root
}

function Install-IntoObsTree([string]$obsRoot, [string]$dllSrc, [string]$dataSrc) {
	$dllDst = Join-Path $obsRoot 'obs-plugins\64bit\livedelay-obs.dll'
	$dataDst = Join-Path $obsRoot 'data\obs-plugins\livedelay-obs'
	New-Item -ItemType Directory -Force -Path (Split-Path $dllDst), $dataDst | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination $dllDst -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force
	$portable = Join-Path $obsRoot 'plugins\livedelay-obs'
	New-Item -ItemType Directory -Force -Path (Join-Path $portable 'data'), (Join-Path $portable 'bin\64bit') | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $portable 'livedelay-obs.dll') -Force
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $portable 'bin\64bit\livedelay-obs.dll') -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination (Join-Path $portable 'data') -Recurse -Force
	Write-Ok "Copied into OBS folder: $obsRoot"
	Write-Warn "obs-plugins\64bit is legacy (OBS 34 may skip it). ProgramData is preferred."
}

Write-Host ""
Write-Host " LiveDelay - OBS plugin installer" -ForegroundColor White
Write-Host " ================================" -ForegroundColor DarkGray

if (Get-Process obs64, obs32 -ErrorAction SilentlyContinue) {
	Write-Err "Close OBS and run the installer again."
	exit 1
}

$payload = Find-ReleasePayload
Write-Info "DLL:  $($payload.Dll)"
Write-Info "Data: $($payload.Data)"

# --- Dependencies: VC++ ---
$vcOk = Ensure-VcRedist -Skip:$SkipVcRedist

$obsRoots = @(Find-ObsRoots)
$obsExe = $null
$obsRoot = $ObsRoot

if (-not $obsRoot) {
	if ($obsRoots.Count -eq 1) {
		$obsRoot = [string]$obsRoots[0]
	} elseif ($obsRoots.Count -gt 1) {
		Write-Info "Found OBS installs:"
		for ($i = 0; $i -lt $obsRoots.Count; $i++) {
			$v = Get-ObsVersion (Join-Path $obsRoots[$i] 'bin\64bit\obs64.exe')
			$label = if ($v) { $v } else { '?' }
			Write-Host ("  [{0}] {1}  (v{2})" -f ($i + 1), $obsRoots[$i], $label)
		}
		if (-not $NoPrompt) {
			$pick = Read-Host "Install number (Enter = ProgramData only)"
			if ($pick -match '^\d+$') {
				$n = [int]$pick - 1
				if ($n -ge 0 -and $n -lt $obsRoots.Count) { $obsRoot = [string]$obsRoots[$n] }
			}
		} else {
			# Prefer newest detected OBS when non-interactive.
			$best = $null
			$bestVer = [version]'0.0'
			foreach ($r in $obsRoots) {
				$v = Get-ObsVersion (Join-Path $r 'bin\64bit\obs64.exe')
				if ($v -and $v -gt $bestVer) { $bestVer = $v; $best = $r }
			}
			if ($best) {
				$obsRoot = $best
				Write-Info "NoPrompt: using newest OBS $bestVer -> $obsRoot"
			}
		}
	} elseif (-not $NoPrompt) {
		Write-Warn "OBS not found automatically."
		$obsRoot = Pick-ObsRoot
	}
}

if ($obsRoot) {
	$obsRoot = [string]$obsRoot
	# Guard against accidental drive-letter / single-char paths (PS string indexing bug).
	if ($obsRoot.Length -lt 4) {
		Write-Warn "Ignoring invalid OBS path: '$obsRoot' (ProgramData install only)."
		$obsRoot = $null
	}
}
if ($obsRoot) {
	$obsExe = Join-Path $obsRoot 'bin\64bit\obs64.exe'
	if (-not (Test-Path -LiteralPath $obsExe)) {
		Write-Warn "obs64.exe missing in: $obsRoot - continuing with ProgramData only."
		$obsRoot = $null
		$obsExe = $null
	}
}

$ver = if ($obsExe) { Get-ObsVersion $obsExe } else { $null }
if ($ver) {
	Write-Info "OBS: $obsRoot (file version $ver)"
	if ($ver.Major -lt 31) {
		Write-Err "LiveDelay needs OBS Studio 31+. You have $ver - update OBS."
		exit 1
	}
	if ($ver.Major -ge 31) {
		Write-Ok "OBS version OK for LiveDelay (31+). Detected $ver."
	}
} else {
	Write-Warn "OBS not detected - installing to ProgramData only. Install OBS 31+ (x64), then restart OBS."
}

# Remove broken manual copies that cause "plugins failed to load"
Remove-ConflictingLivedelay -obsRoot $obsRoot

$pluginRoot = Install-ToProgramData -dllSrc $payload.Dll -dataSrc $payload.Data -obsVer $ver

$portableHint = $false
if ($obsRoot) {
	if ((Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode')) -or
		(Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode.txt'))) {
		$portableHint = $true
	}
}

if ($IntoObsFolder -or $portableHint) {
	if (-not $obsRoot) {
		Write-Err "IntoObsFolder / portable needs -ObsRoot."
		exit 1
	}
	try {
		Install-IntoObsTree -obsRoot $obsRoot -dllSrc $payload.Dll -dataSrc $payload.Data
	} catch {
		Write-Err "Could not write into OBS folder (permissions?). Run as admin or use ProgramData only."
		Write-Err "$_"
		exit 1
	}
} elseif ($obsRoot -and -not $NoPrompt) {
	Write-Host ""
	Write-Info "Default install is ProgramData (recommended, no admin)."
	Write-Info "Portable OBS: re-run with -IntoObsFolder -ObsRoot `"$obsRoot`""
}

# Probe: can Windows load the DLL with OBS Qt/obs on the search path?
$probeDll = Join-Path $pluginRoot 'bin\64bit\livedelay-obs.dll'
Write-Info "LoadLibrary probe..."
$loadErr = Test-PluginLoad -dllPath $probeDll -obsRoot $obsRoot
if ($null -eq $loadErr) {
	Write-Ok "LoadLibrary OK (DLL + VC++ + OBS Qt/obs deps resolve)."
} elseif ($loadErr -match '^skip') {
	Write-Warn "LoadLibrary $loadErr"
} else {
	Write-Err $loadErr
	if (-not $vcOk) {
		Write-Err "Fix: install VC++ Redistributable x64, reboot if asked, then re-run installer."
	} else {
		Write-Err "DLL did not load. Check OBS is x64 31+, remove old livedelay copies, see %AppData%\obs-studio\logs\"
	}
}

Write-Host ""
Write-Ok "Done. Start OBS -> Docks -> LiveDelay / Tools -> LiveDelay."
Write-Host "OBS 32.x (e.g. 32.2.2): ProgramData plugin path is correct."
Write-Host "If plugins still fail to load:"
Write-Host "  1) %AppData%\obs-studio\logs\  -> last log, search livedelay or LoadLibrary"
Write-Host "  2) VC++ Redistributable x64 (installer tries to install it automatically)"
Write-Host "  3) OBS 31+ x64 (not 32-bit / not an ancient Steam build)"
Write-Host "  4) Delete any leftover livedelay-obs.dll under OBS\obs-plugins\64bit"
Write-Host ""
if (-not $vcOk) { exit 2 }
