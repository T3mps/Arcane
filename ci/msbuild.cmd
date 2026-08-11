@echo off
:: Locate MSBuild via vswhere (ships with any VS/Build Tools install) and
:: forward all arguments. Keeps the Jenkinsfile host-agnostic.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found at "%VSWHERE%" - is Visual Studio installed?
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not defined MSBUILD (
    echo ERROR: MSBuild not found via vswhere.
    exit /b 1
)
"%MSBUILD%" %*
