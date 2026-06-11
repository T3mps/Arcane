@echo off
:: Thin wrapper -- delegates to scripts\generate.bat
call "%~dp0scripts\generate.bat" %*
