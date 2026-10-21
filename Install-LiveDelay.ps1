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
	[switch]$SkipVcRedist,
	[switch]$NoStartObs,
	[int]$ObsVerifySeconds = 30
)

$ErrorActionPreference = 'Stop'
$script:ExitCode = 0
$script:FatalMessage = $null

try {
	Add-Type -AssemblyName System.Windows.Forms | Out-Null
} catch {
	# Fallback: console-only mode
}

function Write-Info([string]$m) { Write-Host $m -ForegroundColor Cyan }
function Write-Ok([string]$m) { Write-Host $m -ForegroundColor Green }
function Write-Warn([string]$m) { Write-Host $m -ForegroundColor Yellow }
function Write-Err([string]$m) { Write-Host $m -ForegroundColor Red }

function Show-UserMessage {
	param(
		[string]$Title,
		[string]$Message,
		[ValidateSet('Info', 'Error', 'Warn')]$Kind = 'Info'
	)
	try {
		$icon = switch ($Kind) {
			'Error' { [System.Windows.Forms.MessageBoxIcon]::Error }
			'Warn'  { [System.Windows.Forms.MessageBoxIcon]::Warning }
			default { [System.Windows.Forms.MessageBoxIcon]::Information }
		}
		[void][System.Windows.Forms.MessageBox]::Show($Message, $Title, [System.Windows.Forms.MessageBoxButtons]::OK, $icon)
	} catch {
		Write-Host ""
		Write-Host "=== $Title ===" -ForegroundColor Yellow
		Write-Host $Message
		Write-Host "===============" -ForegroundColor Yellow
		try { [void](Read-Host "Press Enter to continue") } catch { }
	}
}

function Test-IsAdmin {
	try {
		$p = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
		return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
	} catch {
		return $false
	}
}

function Find-ReleasePayload {
	$here = $PSScriptRoot
	$candidates = @(
		@{ Dll = Join-Path $here 'obs-plugins\64bit\livedelay-obs.dll'; Data = Join-Path $here 'data\obs-plugins\livedelay-obs' },
		@{ Dll = Join-Path $here 'obs-plugin\build\Release\livedelay-obs.dll'; Data = Join-Path $here 'obs-plugin\data' }
	)
	foreach ($c in $candidates) {
		try {
			if ((Test-Path -LiteralPath $c.Dll) -and (Test-Path -LiteralPath $c.Data)) { return $c }
		} catch { }
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
		try {
			if (-not $p) { return }
			$p = [Environment]::ExpandEnvironmentVariables($p).TrimEnd('\')
			if ($p.Length -lt 4) { return }
			$exe = Join-Path $p 'bin\64bit\obs64.exe'
			if (Test-Path -LiteralPath $exe) { [void]$list.Add($p) }
		} catch { }
	}
	try {
		& $add 'C:\Program Files\obs-studio'
		& $add 'C:\Program Files (x86)\obs-studio'
		foreach ($key in @('HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio')) {
			try {
				if (Test-Path $key) {
					$val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).'(default)'
					if (-not $val) { $val = (Get-ItemProperty $key -ErrorAction SilentlyContinue).InstallPath }
					& $add $val
				}
			} catch { }
		}
		Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall' -ErrorAction SilentlyContinue |
			ForEach-Object {
				try {
					$p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
					if ($p.DisplayName -match 'OBS Studio' -and $p.InstallLocation) { & $add $p.InstallLocation }
				} catch { }
			}
		foreach ($s in @(
			'C:\Program Files (x86)\Steam\steamapps\common\OBS Studio',
			'C:\Program Files\Steam\steamapps\common\OBS Studio'
		)) { & $add $s }
	} catch { }
	# Always return Object[] - a single string makes $roots[0] mean the char 'C'.
	return @($list | Select-Object -Unique)
}

function Pick-ObsRoot {
	try {
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
	} catch {
		Write-Warn "Folder picker failed: $_"
	}
	return $null
}

function Test-VcRedistX64 {
	try {
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
	} catch {
		return @{ Ok = $false; Version = ''; Missing = @('check-failed'); Detail = "$_" }
	}
}

function Install-VcRedistX64 {
	$url = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
	$out = Join-Path $env:TEMP ('vc_redist.x64-' + [guid]::NewGuid().ToString('N') + '.exe')
	Write-Info "Downloading Microsoft Visual C++ Redistributable x64..."
	Write-Host "  $url"
	$p = $null
	try {
		try {
			Invoke-WebRequest -Uri $url -OutFile $out -UseBasicParsing
		} catch {
			Write-Err "Download failed: $_"
			Write-Err "Install manually: $url"
			return $false
		}
		Write-Info "Installing VC++ Redistributable (quiet)..."
		$argList = @('/install', '/quiet', '/norestart')
		try {
			if (Test-IsAdmin) {
				$p = Start-Process -FilePath $out -ArgumentList $argList -Wait -PassThru
			} else {
				Write-Warn "Elevation required for VC++ install - accept the UAC prompt."
				$p = Start-Process -FilePath $out -ArgumentList $argList -Wait -PassThru -Verb RunAs
			}
		} catch {
			Write-Err "Could not start VC++ installer: $_"
			Write-Err "Install manually: $url"
			return $false
		}
	} finally {
		Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
	}
	$code = if ($null -ne $p) { [int]$p.ExitCode } else { -1 }
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
	try {
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
		$null = Install-VcRedistX64
		$st2 = Test-VcRedistX64
		if (-not $st2.Ok) {
			Write-Err "VC++ still missing after install attempt."
			Write-Err "Download: https://aka.ms/vs/17/release/vc_redist.x64.exe"
			return $false
		}
		Write-Ok "VC++ runtime: $($st2.Version)"
		return $true
	} catch {
		Write-Warn "VC++ check/install error (continuing): $_"
		return $false
	}
}

function Remove-ConflictingLivedelay([string]$obsRoot) {
	if (-not $obsRoot) { return }
	$targets = @(
		(Join-Path $obsRoot 'obs-plugins\64bit\livedelay-obs.dll'),
		(Join-Path $obsRoot 'data\obs-plugins\livedelay-obs')
	)
	foreach ($t in $targets) {
		try {
			if (Test-Path -LiteralPath $t) {
				Write-Warn "Removing conflicting old copy: $t"
				Remove-Item -LiteralPath $t -Recurse -Force -ErrorAction Stop
			}
		} catch {
			Write-Warn "Could not remove (permissions?): $t"
			Write-Warn "$_"
		}
	}
}

function Test-PluginLoad([string]$dllPath, [string]$obsRoot) {
	if (-not $obsRoot) { return 'skip (no OBS path)' }
	try {
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
		[void][LiveDelayNativeLoad]::SetDllDirectory($bin)
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
	} catch {
		return "LoadLibrary probe error: $_"
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

function Get-ObsLogDir {
	return (Join-Path $env:AppData 'obs-studio\logs')
}

function Get-NewestObsLog {
	try {
		$dir = Get-ObsLogDir
		if (-not (Test-Path -LiteralPath $dir)) { return $null }
		return Get-ChildItem -LiteralPath $dir -Filter '*.txt' -ErrorAction SilentlyContinue |
			Sort-Object LastWriteTime -Descending |
			Select-Object -First 1
	} catch {
		return $null
	}
}

function Read-ObsLogText([string]$path) {
	try {
		# OBS may still be writing; share read.
		$fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
		try {
			$sr = New-Object System.IO.StreamReader($fs)
			try { return $sr.ReadToEnd() } finally { $sr.Close() }
		} finally { $fs.Close() }
	} catch {
		try { return (Get-Content -LiteralPath $path -Raw -ErrorAction Stop) } catch { return '' }
	}
}

function Find-LivedelayLogErrors([string]$text) {
	if (-not $text) { return @() }
	$hits = New-Object System.Collections.Generic.List[string]
	foreach ($line in ($text -split "`r?`n")) {
		if ($line -match 'livedelay' -and ($line -match '(?i)fail|error|LoadLibrary|cannot|unable|exception|crash')) {
			[void]$hits.Add($line.Trim())
		} elseif ($line -match "(?i)Failed to load module.*livedelay|LoadLibrary failed for.*livedelay") {
			[void]$hits.Add($line.Trim())
		}
	}
	return @($hits | Select-Object -Unique)
}

function Start-ObsAndVerify {
	param(
		[string]$ObsExe,
		[int]$TimeoutSec = 30
	)
	$result = @{ Ok = $false; Error = $null; Warning = $null; Log = $null; Detail = $null }
	if (-not $ObsExe -or -not (Test-Path -LiteralPath $ObsExe)) {
		$result.Error = 'OBS exe path missing - cannot auto-start.'
		return $result
	}

	$before = Get-Date
	$prev = Get-NewestObsLog
	$prevPath = if ($prev) { $prev.FullName } else { '' }
	$prevLen = if ($prev) { [int64]$prev.Length } else { [int64]0 }

	Write-Info "Starting OBS..."
	Write-Host "  $ObsExe"
	try {
		Start-Process -FilePath $ObsExe -WorkingDirectory (Split-Path -Parent $ObsExe) -ErrorAction Stop | Out-Null
	} catch {
		$result.Error = "Could not start OBS: $_"
		return $result
	}

	Start-Sleep -Seconds 2
	if (-not (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
		# Fallback: maybe still starting
		Start-Sleep -Seconds 2
		if (-not (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
			$result.Error = 'OBS did not stay running after start (crashed or blocked).'
			return $result
		}
	}

	Write-Info "Waiting for OBS log (plugin load check, up to ${TimeoutSec}s)..."
	$deadline = (Get-Date).AddSeconds([Math]::Max(8, $TimeoutSec))
	$logFile = $null
	$text = ''
	while ((Get-Date) -lt $deadline) {
		Start-Sleep -Milliseconds 800
		try {
			$n = Get-NewestObsLog
			if ($n) {
				$isNew = ($n.FullName -ne $prevPath) -or ($n.LastWriteTime -ge $before) -or ([int64]$n.Length -gt $prevLen)
				if ($isNew) {
					$logFile = $n
					$text = Read-ObsLogText $n.FullName
					# Module load usually appears after a few KB / "loading module" lines
					if (($text.Length -gt 2500) -or ($text -match '(?i)livedelay|Failed to load module|loading module')) {
						Start-Sleep -Seconds 2
						$text = Read-ObsLogText $n.FullName
						break
					}
				}
			}
		} catch { }
		if (-not (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
			$result.Error = 'OBS exited while loading (check crash / Safe Mode).'
			if ($logFile) { $result.Log = $logFile.FullName; $result.Detail = (Find-LivedelayLogErrors (Read-ObsLogText $logFile.FullName)) -join "`n" }
			return $result
		}
	}

	if (-not $logFile) {
		$fallback = Get-NewestObsLog
		if ($fallback) {
			$logFile = $fallback
			$text = Read-ObsLogText $fallback.FullName
		}
	}

	if ($logFile) { $result.Log = $logFile.FullName }
	$errs = Find-LivedelayLogErrors $text
	if ($errs.Count -gt 0) {
		$result.Ok = $false
		$result.Error = "OBS failed to load livedelay-obs."
		$result.Detail = ($errs -join "`n")
		return $result
	}

	if (-not (Get-Process -Name obs64 -ErrorAction SilentlyContinue)) {
		$result.Error = 'OBS is not running after verify window.'
		return $result
	}

	$result.Ok = $true
	if (-not $logFile) {
		$result.Warning = 'OBS started but log was not readable yet. Check Docks -> LiveDelay manually.'
	} else {
		$result.Detail = 'No livedelay LoadLibrary/fail lines in OBS log.'
	}
	return $result
}

function Finish-Installer {
	param(
		[int]$Code,
		[string]$ErrorText,
		[switch]$KeepOpen
	)
	$script:ExitCode = $Code
	if ($Code -ne 0 -and $ErrorText) {
		$script:FatalMessage = $ErrorText
		Write-Err $ErrorText
		Show-UserMessage -Title 'LiveDelay install problem' -Message $ErrorText -Kind Error
		if (-not $NoPrompt -or $KeepOpen) {
			try { [void](Read-Host 'Press Enter to close') } catch { }
		}
	} elseif ($Code -eq 0) {
		Write-Ok 'Install OK - closing.'
		Start-Sleep -Seconds 2
	}
	exit $Code
}

# =============================================================================
Write-Host ""
Write-Host " LiveDelay - OBS plugin installer" -ForegroundColor White
Write-Host " ================================" -ForegroundColor DarkGray

try {
	if (Get-Process obs64, obs32 -ErrorAction SilentlyContinue) {
		Finish-Installer -Code 1 -ErrorText "Close OBS and run the installer again." -KeepOpen
	}

	$payload = Find-ReleasePayload
	Write-Info "DLL:  $($payload.Dll)"
	Write-Info "Data: $($payload.Data)"

	$vcOk = $false
	try { $vcOk = Ensure-VcRedist -Skip:$SkipVcRedist } catch { Write-Warn "VC++ step: $_"; $vcOk = $false }

	$obsRoots = @()
	try { $obsRoots = @(Find-ObsRoots) } catch { Write-Warn "OBS detect: $_"; $obsRoots = @() }

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
				try {
					$pick = Read-Host "Install number (Enter = ProgramData only)"
					if ($pick -match '^\d+$') {
						$n = [int]$pick - 1
						if ($n -ge 0 -and $n -lt $obsRoots.Count) { $obsRoot = [string]$obsRoots[$n] }
					}
				} catch { }
			} else {
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
			Finish-Installer -Code 1 -ErrorText "LiveDelay needs OBS Studio 31+. You have $ver - update OBS." -KeepOpen
		}
		Write-Ok "OBS version OK for LiveDelay (31+). Detected $ver."
	} else {
		Write-Warn "OBS not detected - installing to ProgramData only. Install OBS 31+ (x64), then restart OBS."
	}

	try { Remove-ConflictingLivedelay -obsRoot $obsRoot } catch { Write-Warn "Conflict cleanup: $_" }

	$pluginRoot = $null
	try {
		$pluginRoot = Install-ToProgramData -dllSrc $payload.Dll -dataSrc $payload.Data -obsVer $ver
	} catch {
		Finish-Installer -Code 1 -ErrorText "ProgramData install failed: $_" -KeepOpen
	}

	$portableHint = $false
	if ($obsRoot) {
		try {
			if ((Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode')) -or
				(Test-Path -LiteralPath (Join-Path $obsRoot 'portable_mode.txt'))) {
				$portableHint = $true
			}
		} catch { }
	}

	if ($IntoObsFolder -or $portableHint) {
		if (-not $obsRoot) {
			Finish-Installer -Code 1 -ErrorText "IntoObsFolder / portable needs -ObsRoot." -KeepOpen
		}
		try {
			Install-IntoObsTree -obsRoot $obsRoot -dllSrc $payload.Dll -dataSrc $payload.Data
		} catch {
			Finish-Installer -Code 1 -ErrorText "Could not write into OBS folder (permissions?). $_" -KeepOpen
		}
	} elseif ($obsRoot -and -not $NoPrompt) {
		Write-Host ""
		Write-Info "Default install is ProgramData (recommended, no admin)."
		Write-Info "Portable OBS: re-run with -IntoObsFolder -ObsRoot `"$obsRoot`""
	}

	$probeDll = Join-Path $pluginRoot 'bin\64bit\livedelay-obs.dll'
	Write-Info "LoadLibrary probe..."
	$loadErr = $null
	try { $loadErr = Test-PluginLoad -dllPath $probeDll -obsRoot $obsRoot } catch { $loadErr = "$_" }
	if ($null -eq $loadErr) {
		Write-Ok "LoadLibrary OK (DLL + VC++ + OBS Qt/obs deps resolve)."
	} elseif ($loadErr -match '^skip') {
		Write-Warn "LoadLibrary $loadErr"
	} else {
		Write-Err $loadErr
		$hint = if (-not $vcOk) {
			"Fix: install VC++ Redistributable x64, reboot if asked, then re-run installer."
		} else {
			"DLL did not load. Check OBS is x64 31+, remove old livedelay copies, see %AppData%\obs-studio\logs\"
		}
		# Still try starting OBS so log can confirm; treat as soft fail until log verify
		Write-Warn $hint
	}

	Write-Host ""
	Write-Ok "Files installed."

	if ($NoStartObs) {
		Write-Warn "NoStartObs set - skip OBS launch/verify."
		if (-not $vcOk) { Finish-Installer -Code 2 -ErrorText "Installed, but VC++ Redistributable x64 is missing." -KeepOpen }
		Finish-Installer -Code 0
	}

	if (-not $obsExe) {
		Finish-Installer -Code 1 -ErrorText "Plugin files are in ProgramData, but OBS was not found to auto-start.`nInstall OBS 31+ x64, then open OBS -> Docks -> LiveDelay." -KeepOpen
	}

	$verify = $null
	try {
		$verify = Start-ObsAndVerify -ObsExe $obsExe -TimeoutSec $ObsVerifySeconds
	} catch {
		$verify = @{ Ok = $false; Error = "OBS verify crashed: $_"; Detail = $null; Log = $null; Warning = $null }
	}

	if (-not $verify.Ok) {
		$msg = @()
		$msg += $(if ($verify.Error) { $verify.Error } else { 'OBS plugin verify failed.' })
		if ($verify.Detail) { $msg += ''; $msg += 'Log details:'; $msg += $verify.Detail }
		if ($verify.Log) { $msg += ''; $msg += "Log file: $($verify.Log)" }
		$msg += ''
		$msg += 'Try: VC++ x64 redistributable, delete OBS\obs-plugins\64bit\livedelay-obs.dll leftovers, OBS 31+ x64.'
		Finish-Installer -Code 1 -ErrorText ($msg -join "`n") -KeepOpen
	}

	if ($verify.Warning) { Write-Warn $verify.Warning }
	if ($verify.Detail) { Write-Ok $verify.Detail }
	if ($verify.Log) { Write-Host "  log: $($verify.Log)" -ForegroundColor DarkGray }
	Write-Ok "OBS running. Open Docks -> LiveDelay / Tools -> LiveDelay."
	if (-not $vcOk) {
		Finish-Installer -Code 2 -ErrorText "OBS started, but VC++ was missing earlier - if the plugin dialog appears, install VC++ x64 and reboot." -KeepOpen
	}
	Finish-Installer -Code 0
} catch {
	Finish-Installer -Code 1 -ErrorText "Installer failed: $($_.Exception.Message)`n$($_.ScriptStackTrace)" -KeepOpen
}
