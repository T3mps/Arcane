@echo off
rem Convenience shim for launch.ps1 -- launch any built Arcane exe in any config.
rem   launch                -> interactive picker
rem   launch -List          -> inventory of built exes
rem   launch ArcaneRuntime Dist --backend vulkan
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" %*
