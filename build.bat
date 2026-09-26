@echo off
rem ============================================================
rem  Falling Images - Build Script  (MSVC / Visual Studio)
rem
rem  Usage:
rem     build.bat          compile
rem     build.bat run      compile and launch
rem     build.bat clean    remove build output
rem
rem  NOTE: this file is intentionally ASCII-only. Putting non-ASCII
rem  text plus "chcp 65001" inside a .bat makes cmd.exe lose its
rem  byte offset while parsing and corrupts the script.
rem ============================================================

setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
title Falling Images - Build

echo ============================================================
echo   Falling Images  -  Build Script  (MSVC)
echo ============================================================
echo.

rem ---------------- argument: clean ----------------
if /i "%~1"=="clean" (
    echo [clean] removing build output ...
    if exist "build"             rmdir /s /q "build"
    if exist "FallingImages.exe" del /q "FallingImages.exe"
    echo [clean] done.
    echo.
    pause
    exit /b 0
)

set "SRC=main.cpp"
set "OBJDIR=build"
set "OUTEXE=FallingImages.exe"
set "VCVARS="

rem Parentheses in these paths would break "if (...)" blocks if
rem expanded with %..% , so cache them and use delayed expansion.
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "!SRC!" (
    echo [ERROR] source file not found: !SRC!
    echo         put this script next to main.cpp
    echo.
    pause
    exit /b 1
)

rem ============================================================
rem  1. locate vcvars64.bat
rem ============================================================
if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)

rem ---- fallback: well known install roots (top level calls = safe) ----
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2026\Enterprise"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2026\Professional"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2026\Community"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2026\BuildTools"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2022\Enterprise"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2022\Professional"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2022\Community"
if not defined VCVARS call :try "!PF!\Microsoft Visual Studio\2022\BuildTools"
if not defined VCVARS call :try "!PF86!\Microsoft Visual Studio\2019\Enterprise"
if not defined VCVARS call :try "!PF86!\Microsoft Visual Studio\2019\Professional"
if not defined VCVARS call :try "!PF86!\Microsoft Visual Studio\2019\Community"
if not defined VCVARS call :try "!PF86!\Microsoft Visual Studio\2019\BuildTools"

rem ---- fallback: scan drive roots for a VS-like folder ----
if not defined VCVARS (
    for %%r in (C: D: E: F: G:) do (
        if exist "%%r\" (
            for /d %%v in ("%%r\VS*" "%%r\VisualStudio*" "%%r\Microsoft Visual Studio*" "%%r\BuildTools*") do (
                if not defined VCVARS if exist "%%~fv\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~fv\VC\Auxiliary\Build\vcvars64.bat"
            )
        )
    )
)

if not defined VCVARS (
    echo [ERROR] Visual Studio C++ build tools not found ^(vcvars64.bat^).
    echo.
    echo   Install "Visual Studio Build Tools" or the
    echo   "Desktop development with C++" workload:
    echo   https://visualstudio.microsoft.com/downloads/
    echo.
    pause
    exit /b 1
)

echo [1/4] toolchain:
echo       !VCVARS!
call "!VCVARS!" >nul
if errorlevel 1 (
    echo [ERROR] failed to initialise the MSVC environment.
    pause
    exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cl.exe still not on PATH after vcvars.
    pause
    exit /b 1
)

rem ============================================================
rem  2. check the images folder
rem ============================================================
if not exist "images" mkdir "images"

set "IMGCOUNT=0"
for /f %%c in ('dir /b /a-d "images" 2^>nul ^| findstr /i /e /r "\.png \.jpg \.jpeg \.bmp \.gif \.webp \.tif \.tiff \.ico \.jfif \.jxl" ^| find /c /v ""') do set "IMGCOUNT=%%c"

echo.
echo [2/4] images: !IMGCOUNT! file^(s^) found in images\
if !IMGCOUNT!==0 (
    echo       [WARN] no image file. The program will show a dialog and exit.
    echo              supported: png / jpg / jpeg / bmp / gif / webp / tif / ico
)

rem ============================================================
rem  3. compile
rem ============================================================
echo.
echo [3/4] compiling !SRC! ...
if not exist "!OBJDIR!" mkdir "!OBJDIR!"

rem /utf-8   REQUIRED: source is UTF-8 with Chinese literals, without
rem           this MSVC decodes it as CP936 and the build fails.
rem /MT      static CRT -> standalone exe, no VC++ redist needed.
rem /DUNICODE keeps every API call on the W (wide) variant.
rem windowscodecs = WIC, used to decode formats GDI+ cannot read (WebP etc).
rem user32/gdi32/gdiplus/windowscodecs are NOT linked automatically by a
rem bare cl command line - they must be named explicitly.
cl /nologo /utf-8 /std:c++17 /EHsc /O2 /W3 /MT ^
   /DUNICODE /D_UNICODE ^
   "!SRC!" ^
   /Fo:"!OBJDIR!\main.obj" ^
   /Fe:"!OUTEXE!" ^
   /link /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib gdiplus.lib ole32.lib windowscodecs.lib

if errorlevel 1 (
    echo.
    echo ============================================================
    echo   [FAILED] build failed - see the errors above.
    echo ============================================================
    echo.
    pause
    exit /b 1
)

if not exist "!OUTEXE!" (
    echo [ERROR] cl reported success but !OUTEXE! was not produced.
    pause
    exit /b 1
)

echo.
echo ============================================================
echo   [OK] build succeeded
echo ============================================================
echo   exe    : %CD%\!OUTEXE!
echo   images : %CD%\images\
echo   quit   : Ctrl + Alt + Q
echo ============================================================
echo.

rem ============================================================
rem  4. optionally launch
rem ============================================================
if /i "%~1"=="run" (
    echo [4/4] launching ...
    start "" "!OUTEXE!"
    exit /b 0
)

echo [4/4] tip: "build.bat run" compiles and launches in one step.
echo.
pause
exit /b 0

rem ============================================================
rem  subroutine: try a candidate Visual Studio root
rem ============================================================
:try
if not defined VCVARS if exist "%~1\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvars64.bat"
goto :eof
