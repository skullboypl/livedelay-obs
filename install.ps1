# Installs the LiveDelay OBS plugin for this computer (no administrator rights needed).
# Build first: obs-plugin\build.bat. Close OBS before installing.
$ErrorActionPreference = 'Stop'
$root = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs'
$bin = Join-Path $root 'bin\64bit'
$data = Join-Path $root 'data'
New-Item -ItemType Directory -Force $bin, $data | Out-Null
Copy-Item "$PSScriptRoot\obs-plugin\build\Release\livedelay-obs.dll" $bin -Force
Copy-Item "$PSScriptRoot\obs-plugin\data\*" $data -Recurse -Force
# Older builds shipped a separate program; everything lives in the DLL now.
$old = Join-Path $bin 'livedelay.exe'
if (Test-Path $old) { Remove-Item $old }
Write-Host "Installed to $root"
Write-Host "In OBS: Docks -> LiveDelay, and Tools -> LiveDelay for settings."
# OBS must start from its bin folder or it shows "Failed to find locale/en-US.ini".
$obsBin = 'C:\Program Files\obs-studio\bin\64bit'
if ($env:LD_LAUNCH_OBS -eq '1' -and (Test-Path "$obsBin\obs64.exe")) {
	Start-Process -FilePath "$obsBin\obs64.exe" -WorkingDirectory $obsBin
}
