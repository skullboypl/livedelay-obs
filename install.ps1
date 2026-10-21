# Dev install into ProgramData (no admin). Build first: obs-plugin\build.bat. Close OBS first.
# End users: use Install-LiveDelay.ps1 from the release zip (detects OBS / portable).
$ErrorActionPreference = 'Stop'
$dll = Join-Path $PSScriptRoot 'obs-plugin\build\Release\livedelay-obs.dll'
$dataSrc = Join-Path $PSScriptRoot 'obs-plugin\data'
if (-not (Test-Path $dll)) { throw "Missing $dll — run obs-plugin\build.bat first." }
$root = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs'
$bin = Join-Path $root 'bin\64bit'
$data = Join-Path $root 'data'
New-Item -ItemType Directory -Force $bin, $data | Out-Null
# Legacy path (OBS 31/32) + flat path (OBS 33+)
Copy-Item $dll (Join-Path $bin 'livedelay-obs.dll') -Force
Copy-Item $dll (Join-Path $root 'livedelay-obs.dll') -Force
Copy-Item "$dataSrc\*" $data -Recurse -Force
$old = Join-Path $bin 'livedelay.exe'
if (Test-Path $old) { Remove-Item $old }
Write-Host "Installed to $root"
Write-Host "In OBS: Docks -> LiveDelay, and Tools -> LiveDelay for settings."
$obsBin = 'C:\Program Files\obs-studio\bin\64bit'
if ($env:LD_LAUNCH_OBS -eq '1' -and (Test-Path "$obsBin\obs64.exe")) {
	Start-Process -FilePath "$obsBin\obs64.exe" -WorkingDirectory $obsBin
}
