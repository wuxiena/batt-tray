@echo off
rem Build dist\BattTray.exe with zig (MinGW-w64 target). No Visual Studio required.
rem Usage: build.cmd
rem
rem zig is resolved in this order:
rem   1. .\toolchain\ziglang\zig.exe   (local, optional)
rem   2. zig.exe on PATH              (https://ziglang.org/download/)
setlocal
cd /d "%~dp0"

set "ZIG=%~dp0toolchain\ziglang\zig.exe"
if not exist "%ZIG%" (
    where zig.exe >nul 2>nul
    if errorlevel 1 (
        echo [!] zig not found.
        echo     Either put zig.exe on PATH, or unpack it to toolchain\ziglang\zig.exe
        exit /b 1
    )
    set "ZIG=zig.exe"
)
echo [i] using %ZIG%

set "ZIG_GLOBAL_CACHE_DIR=%~dp0.zigcache"
set "ZIG_LOCAL_CACHE_DIR=%~dp0.zigcache"
if not exist "dist" mkdir "dist"

"%ZIG%" cc -target x86_64-windows-gnu -O2 -s -Wl,--subsystem,windows ^
    -o dist\BattTray.exe src\BattTray.c src\providers.c ^
    -luser32 -lshell32 -lgdi32 -ladvapi32 -lhid -lsetupapi -lm
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

rem Embed the app icon into the exe resource section (RT_GROUP_ICON + RT_ICON).
rem Uses Win32 UpdateResource, so no rc.exe / windres / llvm-rc is needed.
if not exist "build" mkdir "build"
"%ZIG%" cc -target x86_64-windows-gnu -O2 -s -o build\embedres.exe tools\embedres.c
if errorlevel 1 (
    echo BUILD FAILED - embedres
    exit /b 1
)
build\embedres.exe dist\BattTray.exe src\res\app.ico
if errorlevel 1 (
    echo EMBED ICON FAILED
    exit /b 1
)

echo BUILD OK
for %%f in (dist\BattTray.exe) do echo   %%~ff  %%~zf bytes
