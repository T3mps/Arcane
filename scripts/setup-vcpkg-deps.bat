@echo off
setlocal

echo ============================================
echo  Arcane vcpkg Dependency Setup
echo ============================================
echo.

:: -------------------------------------------------------------------
:: Resolve vcpkg (required for SDL3 -- platform layer)
:: -------------------------------------------------------------------
echo [1/2] Checking vcpkg...

if defined VCPKG_ROOT (
    set "VCPKG_PATH=%VCPKG_ROOT%"
) else if exist "C:\vcpkg\vcpkg.exe" (
    set "VCPKG_PATH=C:\vcpkg"
) else (
    echo ERROR: vcpkg not found.
    echo Set the VCPKG_ROOT environment variable or install vcpkg to C:\vcpkg.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

if not exist "%VCPKG_PATH%\vcpkg.exe" (
    echo ERROR: vcpkg.exe not found at %VCPKG_PATH%
    echo Verify your VCPKG_ROOT is correct.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)
echo   vcpkg: %VCPKG_PATH%

:: -------------------------------------------------------------------
:: Install SDL3 with the dynamic-CRT overlay triplet. The Arcane
:: workspace is /MD everywhere (see Arcane/premake5.lua); the triplet
:: pins the v143 toolset like the Server's x64-windows-static one.
:: -------------------------------------------------------------------
echo [2/2] Installing sdl3[vulkan]:x64-windows-static-md...

"%VCPKG_PATH%\vcpkg.exe" install "sdl3[vulkan]:x64-windows-static-md" --overlay-triplets="%~dp0..\..\vcpkg-triplets"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: vcpkg install failed.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

echo.
echo ============================================
echo  Success! SDL3 (with Vulkan support) installed.
echo ============================================
echo.
if not defined _APH_NOPAUSE pause
endlocal
