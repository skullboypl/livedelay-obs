@echo off
rem Builds the Rust proxy as a static library, then the OBS plugin DLL that embeds it.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set PATH=%USERPROFILE%\.cargo\bin;%PATH%
cd /d %~dp0..
cargo build --release || exit /b 1
cd /d %~dp0
set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
%CMAKE% -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
%CMAKE% --build build --config Release || exit /b 1
