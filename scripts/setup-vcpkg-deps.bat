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

:: The triplet builds SDL3 (only) as a DLL -- one SDL3.dll per process, staged
:: beside every program (ThirdParty/SDL3/SDL3.arcmodule). vcpkg's classic mode
:: does not rebuild an installed port whose triplet changed, so an earlier
:: static install (share\sdl3 present, no bin\SDL3.dll) is removed first.
set "SDL3_MD=%VCPKG_PATH%\installed\x64-windows-static-md"
if exist "%SDL3_MD%\share\sdl3" if not exist "%SDL3_MD%\bin\SDL3.dll" (
    echo   removing the static SDL3 install ^(the triplet now builds SDL3.dll^)...
    "%VCPKG_PATH%\vcpkg.exe" remove "sdl3:x64-windows-static-md" --overlay-triplets="%~dp0..\vcpkg-triplets"
    if errorlevel 1 (
        echo ERROR: vcpkg remove failed.
        echo.
        if not defined _APH_NOPAUSE pause
        exit /b 1
    )
)

"%VCPKG_PATH%\vcpkg.exe" install "sdl3[vulkan]:x64-windows-static-md" --overlay-triplets="%~dp0..\vcpkg-triplets"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: vcpkg install failed.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

echo.
echo ============================================
echo  Success! SDL3 (with Vulkan support) installed as SDL3.dll.
echo ============================================
echo.
if not defined _APH_NOPAUSE pause
endlocal
