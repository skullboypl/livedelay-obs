# Builds the release zip for the OBS Resources page / GitHub releases.
# Layout matches the OBS install folder, so users can unzip it straight into
# C:\Program Files\obs-studio. Build first: obs-plugin\build.bat
$ErrorActionPreference = 'Stop'
$repo = 'https://github.com/skullboypl/livedelay-obs'  # planned public repository
$version = (Select-String -Path "$PSScriptRoot\Cargo.toml" -Pattern '^version = "(.+)"').Matches[0].Groups[1].Value
$name = "livedelay-obs-$version-windows-x64"
$stage = Join-Path $PSScriptRoot "dist\$name"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\obs-plugins\64bit", "$stage\data\obs-plugins\livedelay-obs" | Out-Null
Copy-Item "$PSScriptRoot\obs-plugin\build\Release\livedelay-obs.dll" "$stage\obs-plugins\64bit"
Copy-Item "$PSScriptRoot\obs-plugin\data\*" "$stage\data\obs-plugins\livedelay-obs" -Recurse
Copy-Item "$PSScriptRoot\LICENSE" "$stage\LICENSE.txt"
Copy-Item "$PSScriptRoot\Install-LiveDelay.ps1" "$stage\Install-LiveDelay.ps1"
# BOM for Windows PowerShell 5.1 -File. Do NOT put BOM on web/public/install.ps1 (breaks irm|iex).
$utf8bom = New-Object System.Text.UTF8Encoding $true
[System.IO.File]::WriteAllText("$stage\Install-LiveDelay.ps1", [System.IO.File]::ReadAllText("$stage\Install-LiveDelay.ps1"), $utf8bom)
@"
LiveDelay $version for OBS Studio 31+ (Windows x64)

Recommended install (no admin)
  1. Close OBS.
  2. Unzip this archive anywhere.
  3. Right-click Install-LiveDelay.ps1 -> Run with PowerShell
     (or: powershell -ExecutionPolicy Bypass -File .\Install-LiveDelay.ps1)
  4. It installs into %ProgramData%\obs-studio\plugins\livedelay-obs
     and can detect / ask for your OBS folder (Steam, portable, custom).

Portable OBS
  powershell -ExecutionPolicy Bypass -File .\Install-LiveDelay.ps1 -IntoObsFolder -ObsRoot "D:\path\to\obs-studio"

Manual install (legacy)
  Copy obs-plugins and data into your OBS folder and merge.
  Prefer the script — wrong folder is the #1 cause of
  "The following OBS plugins failed to load".

Uninstall
  Delete %ProgramData%\obs-studio\plugins\livedelay-obs
  (and obs-plugins\64bit\livedelay-obs.dll if you used the manual path).

If the plugin still fails to load
  - OBS Studio 31+ x64 required
  - Install "Microsoft Visual C++ Redistributable 2015-2022 x64"
  - Check %AppData%\obs-studio\logs\ for LoadLibrary / [livedelay]

Source and help: $repo
"@ | Set-Content -Encoding utf8 "$stage\INSTALL.txt"
$zip = Join-Path $PSScriptRoot "dist\$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# tar (bsdtar, built into Windows 10+) writes forward-slash paths; Compress-Archive on
# Windows PowerShell 5 writes backslashes that some unzip tools mangle.
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage obs-plugins data INSTALL.txt LICENSE.txt Install-LiveDelay.ps1
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"
Write-Host "$zip"
Write-Host "SHA256 $hash"
