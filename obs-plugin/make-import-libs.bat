@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
lib /nologo /def:obs.def /machine:x64 /out:obs.lib
lib /nologo /def:obs-frontend-api.def /machine:x64 /out:obs-frontend-api.lib
