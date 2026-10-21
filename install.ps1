# Instaluje LiveDelay jako wtyczkę OBS dla tego komputera (bez uprawnień administratora).
# Uruchom po zbudowaniu: cargo build --release oraz obs-plugin\build.bat
$ErrorActionPreference = 'Stop'
$dst = Join-Path $env:ProgramData 'obs-studio\plugins\livedelay-obs\bin\64bit'
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item "$PSScriptRoot\obs-plugin\build\Release\livedelay-obs.dll", "$PSScriptRoot\target\release\livedelay.exe" $dst -Force
Write-Host "Zainstalowano do $dst"
Write-Host "Uruchom OBS i włącz panel: Panele -> LiveDelay. Ustawienia proxy: $env:APPDATA\LiveDelay\livedelay.ini"
