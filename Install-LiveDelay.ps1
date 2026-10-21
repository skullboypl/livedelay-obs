# LiveDelay installer for end users (Windows x64).
# Run from the unzipped release (next to obs-plugins\ and data\), or from this repo.
# Prefer ProgramData (no admin). Portable: -IntoObsFolder -ObsRoot "D:\obs-studio"
# ASCII-only. UTF-8 BOM OK for -File; do not serve via irm|iex.
#Requires -Version 5.1
[CmdletBinding()]
param(
	[string]$ObsRoot = '',
	[switch]$IntoObsFolder,
	[switch]$NoPrompt
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms | Out-Null

function Write-Info([string]$m) { Write-Host $m -ForegroundColor Cyan }
function Write-Ok([string]$m) { Write-Host $m -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host $m -ForegroundColor Yellow }
function Write-Err([string]$m) { Write-Host $m -ForegroundColor Red }

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
	# Always return Object[] — a single string makes $roots[0] mean the char 'C'.
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
}

Install-ToProgramData -dllSrc $payload.Dll -dataSrc $payload.Data -obsVer $ver | Out-Null

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

Write-Host ""
Write-Ok "Done. Start OBS -> Docks -> LiveDelay / Tools -> LiveDelay."
Write-Host "If plugins still fail to load:"
Write-Host "  1) %AppData%\obs-studio\logs\  -> last log, search livedelay or LoadLibrary"
Write-Host "  2) Install VC++ Redistributable x64 (VS 2015-2022)"
Write-Host "  3) OBS 31+ x64 (not 32-bit / not an ancient Steam build)"
Write-Host ""
