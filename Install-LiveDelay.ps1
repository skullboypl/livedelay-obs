# LiveDelay installer for end users (Windows x64).
# Run from the unzipped release folder (next to obs-plugins\ and data\), or from this repo.
# Prefer ProgramData (no admin). Portable OBS: pick the folder that contains obs64.exe / bin\64bit.
#Requires -Version 5.1
[CmdletBinding()]
param(
	# OBS root (folder with bin\64bit\obs64.exe) or empty = auto / pick
	[string]$ObsRoot = '',
	# Force Program Files layout into $ObsRoot (needs write access / often admin)
	[switch]$IntoObsFolder,
	# Skip GUI folder picker when auto-detect fails
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
	throw "Nie znaleziono livedelay-obs.dll + data. Uruchom skrypt z rozpakowanego zipa LiveDelay (obok folderow obs-plugins i data)."
}

function Get-ObsVersion([string]$obsExe) {
	try {
		$v = [version](Get-Item -LiteralPath $obsExe).VersionInfo.FileVersion
		return $v
	} catch { return $null }
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
	# Installer registry
	foreach ($key in @(
		'HKLM:\SOFTWARE\OBS Studio',
		'HKLM:\SOFTWARE\WOW6432Node\OBS Studio'
	)) {
		if (Test-Path $key) {
			$val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).'(default)'
			if (-not $val) { $val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).InstallPath }
			& $add $val
		}
	}
	# Uninstall entry
	Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
		ForEach-Object {
			$p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
			if ($p.DisplayName -match 'OBS Studio' -and $p.InstallLocation) { & $add $p.InstallLocation }
		}
	# Steam
	$steamLibs = @(
		'C:\Program Files (x86)\Steam\steamapps\common\OBS Studio',
		'C:\Program Files\Steam\steamapps\common\OBS Studio'
	)
	foreach ($s in $steamLibs) { & $add $s }
	return $list | Select-Object -Unique
}

function Pick-ObsRoot {
	$dlg = New-Object System.Windows.Forms.FolderBrowserDialog
	$dlg.Description = "Wybierz folder OBS (ten z bin\64bit\obs64.exe) albo folder portable."
	$dlg.ShowNewFolderButton = $false
	if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
	$root = $dlg.SelectedPath.TrimEnd('\')
	if (Test-Path -LiteralPath (Join-Path $root 'bin\64bit\obs64.exe')) { return $root }
	if (Test-Path -LiteralPath (Join-Path $root 'obs64.exe')) {
		# User picked bin\64bit
		return (Split-Path (Split-Path $root -Parent) -Parent)
	}
	Write-Warn "W wybranym folderze nie ma bin\64bit\obs64.exe."
	return $null
}

function Install-ToProgramData([string]$dllSrc, [string]$dataSrc, [version]$obsVer) {
	$root = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs'
	$dataDst = Join-Path $root 'data'
	$legacyBin = Join-Path $root 'bin\64bit'
	New-Item -ItemType Directory -Force -Path $dataDst, $legacyBin | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $legacyBin 'livedelay-obs.dll') -Force
	# OBS 33+: flat layout (DLL next to data). Harmless on older OBS (ignored).
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $root 'livedelay-obs.dll') -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force
	Write-Ok "Zainstalowano do ProgramData (bez admina):"
	Write-Host "  $root"
	if ($obsVer -and $obsVer.Major -ge 33) {
		Write-Info "OBS $($obsVer.Major)+ -> uklad nowy (DLL w folderze wtyczki) + legacy bin\64bit."
	} else {
		Write-Info "OBS laduje: …\livedelay-obs\bin\64bit\livedelay-obs.dll"
	}
	return $root
}

function Install-IntoObsTree([string]$obsRoot, [string]$dllSrc, [string]$dataSrc) {
	$dllDst = Join-Path $obsRoot 'obs-plugins\64bit\livedelay-obs.dll'
	$dataDst = Join-Path $obsRoot 'data\obs-plugins\livedelay-obs'
	New-Item -ItemType Directory -Force -Path (Split-Path $dllDst), $dataDst | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination $dllDst -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force
	# Portable OBS 33+ also reads <root>\plugins\livedelay-obs\
	$portable = Join-Path $obsRoot 'plugins\livedelay-obs'
	New-Item -ItemType Directory -Force -Path (Join-Path $portable 'data'), (Join-Path $portable 'bin\64bit') | Out-Null
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $portable 'livedelay-obs.dll') -Force
	Copy-Item -LiteralPath $dllSrc -Destination (Join-Path $portable 'bin\64bit\livedelay-obs.dll') -Force
	Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination (Join-Path $portable 'data') -Recurse -Force
	Write-Ok "Skopiowano do folderu OBS: $obsRoot"
	Write-Warn "Uklad obs-plugins\64bit jest legacy (OBS 34 moze go pominac). ProgramData jest zalecany."
}

# --- main ---
Write-Host ""
Write-Host " LiveDelay - instalator wtyczki OBS" -ForegroundColor White
Write-Host " ==================================" -ForegroundColor DarkGray

if (Get-Process obs64, obs32 -ErrorAction SilentlyContinue) {
	Write-Err "Zamknij OBS i uruchom instalator ponownie."
	exit 1
}

$payload = Find-ReleasePayload
Write-Info "DLL:  $($payload.Dll)"
Write-Info "Data: $($payload.Data)"

$obsRoots = Find-ObsRoots
$obsExe = $null
$obsRoot = $ObsRoot

if (-not $obsRoot) {
	if ($obsRoots.Count -eq 1) {
		$obsRoot = $obsRoots[0]
	} elseif ($obsRoots.Count -gt 1) {
		Write-Info "Znalezione instalacje OBS:"
		for ($i = 0; $i -lt $obsRoots.Count; $i++) {
			$v = Get-ObsVersion (Join-Path $obsRoots[$i] 'bin\64bit\obs64.exe')
			Write-Host ("  [{0}] {1}  (v{2})" -f ($i + 1), $obsRoots[$i], $(if ($v) { $v } else { '?' }))
		}
		if (-not $NoPrompt) {
			$pick = Read-Host "Numer instalacji (Enter = ProgramData bez wskazywania OBS)"
			if ($pick -match '^\d+$') {
				$n = [int]$pick - 1
				if ($n -ge 0 -and $n -lt $obsRoots.Count) { $obsRoot = $obsRoots[$n] }
			}
		}
	} elseif (-not $NoPrompt) {
		Write-Warn "Nie znaleziono OBS automatycznie."
		$obsRoot = Pick-ObsRoot
	}
}

if ($obsRoot) {
	$obsExe = Join-Path $obsRoot 'bin\64bit\obs64.exe'
	if (-not (Test-Path -LiteralPath $obsExe)) {
		Write-Err "Brak obs64.exe w: $obsRoot"
		exit 1
	}
}

$ver = if ($obsExe) { Get-ObsVersion $obsExe } else { $null }
if ($ver) {
	Write-Info "OBS: $obsRoot (wersja pliku $ver)"
	if ($ver.Major -lt 31) {
		Write-Err "LiveDelay wymaga OBS Studio 31+. Masz $ver — zaktualizuj OBS."
		exit 1
	}
}

# Default: ProgramData (works for normal installs; portable ignores ProgramData)
Install-ToProgramData -dllSrc $payload.Dll -dataSrc $payload.Data -obsVer $ver | Out-Null

$portableHint = $false
if ($obsRoot) {
	# Heuristic: no Program Files / Steam "common" still can be portable if portable_mode file exists
	if (Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode') -or
		Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode.txt')) {
		$portableHint = $true
	}
}

if ($IntoObsFolder -or $portableHint) {
	if (-not $obsRoot) {
		Write-Err "Tryb IntoObsFolder / portable wymaga wskazania folderu OBS."
		exit 1
	}
	try {
		Install-IntoObsTree -obsRoot $obsRoot -dllSrc $payload.Dll -dataSrc $payload.Data
	} catch {
		Write-Err "Nie udalo sie zapisac do folderu OBS (brak uprawnien?). Uruchom PowerShell jako administrator albo uzyj samego ProgramData."
		Write-Err "$_"
		exit 1
	}
} elseif ($obsRoot -and -not $NoPrompt) {
	Write-Host ""
	Write-Info "Domyslnie wgrano do ProgramData (zalecane, bez admina)."
	Write-Info "Jesli uzywasz OBS portable — uruchom ponownie z: -IntoObsFolder -ObsRoot `"$obsRoot`""
}

Write-Host ""
Write-Ok "Gotowe. Uruchom OBS -> Panele -> LiveDelay / Narzedzia -> LiveDelay."
Write-Host "Jesli nadal 'wtyczki nie zaladowaly sie':"
Write-Host "  1) %AppData%\obs-studio\logs\  -> ostatni log, szukaj [livedelay] albo LoadLibrary"
Write-Host "  2) Zainstaluj VC++ Redistributable x64 (Visual Studio 2015-2022)"
Write-Host "  3) OBS 31+ x64 (nie 32-bit / nie zbyt stary Steam build)"
Write-Host ""
