# Instaluje wtyczkę LiveDelay dla OBS na tym komputerze (bez uprawnień administratora).
# Najpierw zbuduj: obs-plugin\build.bat
$ErrorActionPreference = 'Stop'
$dst = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs\bin\64bit'
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$PSScriptRoot\obs-plugin\build\Release\livedelay-obs.dll" $dst -Force
# Starsza wersja miała osobny program; teraz wszystko jest w DLL.
$old = Join-Path $dst 'livedelay.exe'
if (Test-Path $old) { Remove-Item $old }
Write-Host "Zainstalowano do $dst"
Write-Host "W OBS: Panele -> LiveDelay oraz Narzędzia -> LiveDelay (ustawienia)."
