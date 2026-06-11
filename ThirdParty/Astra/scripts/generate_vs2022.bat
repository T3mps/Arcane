@echo off
pushd %~dp0\..\
echo Generating Visual Studio 2022 solution...
premake5 vs2022
if %ERRORLEVEL% NEQ 0 (
    echo Error: Failed to generate project files
    pause
    exit /b %ERRORLEVEL%
)
popd
pause