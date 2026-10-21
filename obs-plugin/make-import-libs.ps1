# Tworzy obs.lib i obs-frontend-api.lib z zainstalowanego OBS (do linkowania wtyczki).
param([string]$Obs = 'C:\Program Files\obs-studio\bin\64bit', [string]$Out = "$PSScriptRoot\..\..\deps\obslibs")
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vc = Get-ChildItem "$vs\VC\Tools\MSVC" | Sort-Object Name | Select-Object -Last 1
$bin = "$($vc.FullName)\bin\Hostx64\x64"
New-Item -ItemType Directory -Force "$Out\include" | Out-Null
"#pragma once`n#define OBS_RELEASE_CANDIDATE 0`n#define OBS_BETA 0" | Set-Content "$Out\include\obsconfig.h"
foreach ($n in 'obs', 'obs-frontend-api') {
    $names = & "$bin\dumpbin.exe" /nologo /exports "$Obs\$n.dll" |
        Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
    @("LIBRARY $n", 'EXPORTS') + $names | Set-Content "$Out\$n.def"
    & "$bin\lib.exe" /nologo "/def:$Out\$n.def" /machine:x64 "/out:$Out\$n.lib"
}
