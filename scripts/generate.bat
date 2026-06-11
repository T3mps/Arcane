@echo off
setlocal

echo ============================================
echo  Arcane Engine Project Generation
echo ============================================
echo.

set "PROJECT_ROOT=%~dp0.."

:: -------------------------------------------------------------------
:: Generate Visual Studio solution
:: -------------------------------------------------------------------
echo [1/1] Generating Visual Studio 2026 solution...

set "PREMAKE5=%PROJECT_ROOT%\..\ThirdParty\premake5\premake5.exe"
if not exist "%PREMAKE5%" (
    echo ERROR: premake5 not found at %PREMAKE5%
    echo Ensure ThirdParty\premake5\premake5.exe exists in the repo.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

pushd "%PROJECT_ROOT%"
call "%PREMAKE5%" vs2026
set "PREMAKE_ERR=%ERRORLEVEL%"
popd

if %PREMAKE_ERR% NEQ 0 (
    echo ERROR: premake5 failed.
    echo.
    if not defined _APH_NOPAUSE pause
    exit /b 1
)

echo.
echo ============================================
echo  Success! Open Arcane.slnx in Visual Studio 2026.
echo ============================================
echo.
if not defined _APH_NOPAUSE pause
endlocal
