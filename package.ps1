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
@"
LiveDelay $version for OBS Studio 31+ (Windows x64)

Install
  1. Close OBS.
  2. Copy the "obs-plugins" and "data" folders into your OBS folder
     (usually C:\Program Files\obs-studio) and confirm merging.
  3. Start OBS: Docks -> LiveDelay, Tools -> LiveDelay for settings.

Uninstall
  Delete obs-plugins\64bit\livedelay-obs.dll and data\obs-plugins\livedelay-obs.

Source and help: $repo
"@ | Set-Content -Encoding utf8 "$stage\INSTALL.txt"
$zip = Join-Path $PSScriptRoot "dist\$name.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
# tar (bsdtar, built into Windows 10+) writes forward-slash paths; Compress-Archive on
# Windows PowerShell 5 writes backslashes that some unzip tools mangle.
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage obs-plugins data INSTALL.txt LICENSE.txt
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $name.zip" | Set-Content -Encoding ascii "$zip.sha256"
Write-Host "$zip"
Write-Host "SHA256 $hash"
