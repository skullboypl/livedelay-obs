# Removes LiveDelay plugin files after OBS has released the DLL.
# Does not delete %AppData%\LiveDelay (settings and stream keys stay).
# Only a directory named livedelay-obs, or the file livedelay-obs.dll,
# and only under an OBS plugin path.
#Requires -Version 5.1
[CmdletBinding()]
param(
	[int]$ObsPid = 0,
	[string[]]$Target,
	[string]$UiTitle = 'LiveDelay - uninstall',
	[string]$UiWait = 'Waiting for OBS to close...',
	[string]$UiRemoving = 'Removing the plugin...',
	[string]$UiDone = 'The plugin has been removed. You can close this window.',
	[string]$UiFail = 'Could not remove the plugin: ',
	[string]$UiClose = 'Close',
	[string]$UiNone = 'Plugin files were not found.'
)

$ErrorActionPreference = 'Stop'

function Test-LivedelayPath([string]$path) {
	if ([string]::IsNullOrWhiteSpace($path)) { return $false }
	try { $full = [IO.Path]::GetFullPath($path) } catch { return $false }
	$full = $full.TrimEnd('\')
	if ($full.Length -lt 16) { return $false }
	$root = [IO.Path]::GetPathRoot($full).TrimEnd('\')
	if ($full -eq $root) { return $false }
	$name = [IO.Path]::GetFileName($full)
	if ($name -ne 'livedelay-obs' -and $name -ne 'livedelay-obs.dll') { return $false }
	$lower = $full.ToLowerInvariant()
	if ($lower -notmatch '\\obs-studio\\' -and $lower -notmatch '\\obs-plugins\\' -and $lower -notmatch '\\plugins\\') {
		return $false
	}
	return $true
}

function Add-Target([System.Collections.Generic.List[string]]$list, [string]$path) {
	if (-not (Test-Path -LiteralPath $path)) { return }
	if (-not (Test-LivedelayPath $path)) { return }
	$full = [IO.Path]::GetFullPath($path).TrimEnd('\')
	foreach ($e in $list) {
		if ($e -eq $full) { return }
	}
	[void]$list.Add($full)
}

function Get-DefaultTargets {
	$list = New-Object System.Collections.Generic.List[string]
	$pd = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs'
	Add-Target $list $pd
	$roots = @(
		'C:\Program Files\obs-studio',
		'C:\Program Files (x86)\obs-studio',
		'C:\Program Files\Steam\steamapps\common\OBS Studio',
		'C:\Program Files (x86)\Steam\steamapps\common\OBS Studio'
	)
	foreach ($obs in $roots) {
		Add-Target $list (Join-Path $obs 'obs-plugins\64bit\livedelay-obs.dll')
		Add-Target $list (Join-Path $obs 'data\obs-plugins\livedelay-obs')
		Add-Target $list (Join-Path $obs 'plugins\livedelay-obs')
	}
	return @($list)
}

$targets = New-Object System.Collections.Generic.List[string]
if ($Target -and $Target.Count -gt 0) {
	foreach ($t in $Target) { Add-Target $targets $t }
} else {
	foreach ($t in (Get-DefaultTargets)) { [void]$targets.Add($t) }
}

Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing | Out-Null
try {
	Add-Type -Name W -Namespace LdHide -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
'@
	$cw = [LdHide.W]::GetConsoleWindow()
	if ($cw -ne [IntPtr]::Zero) { [void][LdHide.W]::ShowWindow($cw, 0) }
} catch { }

$form = New-Object System.Windows.Forms.Form
$form.Text = $UiTitle
$form.ClientSize = New-Object System.Drawing.Size(440, 168)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.ControlBox = $false
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::FromArgb(22, 26, 34)
$brand = New-Object System.Windows.Forms.Label
$brand.Text = 'LiveDelay'
$brand.AutoSize = $true
$brand.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 13)
$brand.ForeColor = [System.Drawing.Color]::FromArgb(255, 59, 47)
$brand.Location = New-Object System.Drawing.Point(22, 14)
$status = New-Object System.Windows.Forms.Label
$status.Text = $UiWait
$status.Font = New-Object System.Drawing.Font('Segoe UI', 9.75)
$status.ForeColor = [System.Drawing.Color]::White
$status.Location = New-Object System.Drawing.Point(22, 52)
$status.Size = New-Object System.Drawing.Size(396, 40)
$bar = New-Object System.Windows.Forms.ProgressBar
$bar.Location = New-Object System.Drawing.Point(22, 100)
$bar.Size = New-Object System.Drawing.Size(396, 22)
$bar.Minimum = 0
$bar.Maximum = 100
$bar.Value = 4
$closeBtn = New-Object System.Windows.Forms.Button
$closeBtn.Text = $UiClose
$closeBtn.Enabled = $false
$closeBtn.Location = New-Object System.Drawing.Point(318, 130)
$closeBtn.Size = New-Object System.Drawing.Size(100, 28)
$closeBtn.Add_Click({ $form.Close() })
$form.Controls.AddRange(@($brand, $status, $bar, $closeBtn))
$form.Show()
$form.Activate()
$form.Refresh()
[void][System.Windows.Forms.Application]::DoEvents()

function Set-Step([string]$text, [int]$pct) {
	if ($pct -lt 0) { $pct = 0 }
	if ($pct -gt 100) { $pct = 100 }
	$status.Text = $text
	$bar.Value = $pct
	$form.Refresh()
	[void][System.Windows.Forms.Application]::DoEvents()
}

$logPath = Join-Path $env:TEMP 'livedelay-uninstall.log'
try {
	"uninstall start pid=$ObsPid" | Set-Content -LiteralPath $logPath -Encoding utf8
	Set-Step $UiWait 8
	$deadline = (Get-Date).AddMinutes(3)
	while ((Get-Date) -lt $deadline) {
		$alive = $false
		if ($ObsPid -gt 0) {
			$p = Get-Process -Id $ObsPid -ErrorAction SilentlyContinue
			if ($p) { $alive = $true }
		} else {
			$alive = [bool](Get-Process obs64, obs32 -ErrorAction SilentlyContinue)
		}
		if (-not $alive) { break }
		Set-Step $UiWait ([Math]::Min(40, $bar.Value + 1))
		Start-Sleep -Milliseconds 400
	}
	if ($ObsPid -gt 0 -and (Get-Process -Id $ObsPid -ErrorAction SilentlyContinue)) {
		throw 'OBS is still running'
	}
	Start-Sleep -Milliseconds 600
	if ($targets.Count -eq 0) {
		Set-Step $UiNone 100
		"no targets" | Add-Content -LiteralPath $logPath -Encoding utf8
	} else {
		Set-Step $UiRemoving 55
		foreach ($t in $targets) {
			if (-not (Test-LivedelayPath $t)) { throw "refused $t" }
			if (Test-Path -LiteralPath $t) {
				Remove-Item -LiteralPath $t -Recurse -Force
				"removed $t" | Add-Content -LiteralPath $logPath -Encoding utf8
			}
		}
		Set-Step $UiDone 100
	}
} catch {
	$msg = $_.Exception.Message
	Set-Step ($UiFail + $msg) 100
	"failed $msg" | Add-Content -LiteralPath $logPath -Encoding utf8
}
$closeBtn.Enabled = $true
$form.ControlBox = $true
while ($form.Visible) {
	[void][System.Windows.Forms.Application]::DoEvents()
	Start-Sleep -Milliseconds 50
}
