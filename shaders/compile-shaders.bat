@echo off
:: Compiles every engine shader entry point to DXIL + SPIR-V loose artifacts.
:: Invoked by the premake prebuild step on the Arcane project; also runnable
:: by hand for the hot-reload dev loop (the running app picks changes up via
:: ShaderLibrary::Poll when ARCANE_SHADER_DIR points at generated\).
::
:: SPIR-V register shifts MUST match nvrhi::VulkanBindingOffsets defaults
:: (t=0, s=128, b=256, u=384). ShaderMake (vendored) replaces this script
:: when the shader count outgrows explicit lines.
::
:: INVARIANT: the output stem's _vs/_ps/_cs suffix (4th arg) must agree with
:: the entry point's <type>_main prefix (2nd arg) -- ShaderLibrary derives the
:: SPIR-V entry name from the stem suffix. Mismatch = late Vulkan failure.
setlocal
set DXC=%~dp0..\..\ThirdParty\tools\dxc\dxc.exe
set SRC=%~dp0
set OUT=%~dp0generated
if not exist "%OUT%\dxil"  mkdir "%OUT%\dxil"
if not exist "%OUT%\spirv" mkdir "%OUT%\spirv"

set SPIRV_FLAGS=-spirv -D SPIRV=1 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0

call :compile sprite  vs_main vs_6_5 sprite_vs  || exit /b 1
call :compile sprite  ps_main ps_6_5 sprite_ps  || exit /b 1
call :compile circle  vs_main vs_6_5 circle_vs  || exit /b 1
call :compile circle  ps_main ps_6_5 circle_ps  || exit /b 1
call :compile msdf    vs_main vs_6_5 msdf_vs    || exit /b 1
call :compile msdf    ps_main ps_6_5 msdf_ps    || exit /b 1
call :compile imgui   vs_main vs_6_5 imgui_vs   || exit /b 1
call :compile imgui   ps_main ps_6_5 imgui_ps   || exit /b 1
call :compile tonemap vs_main vs_6_5 tonemap_vs || exit /b 1
call :compile tonemap ps_main ps_6_5 tonemap_ps || exit /b 1
echo Shaders compiled to %OUT%
exit /b 0

:compile
"%DXC%" -T %3 -E %2 -Fo "%OUT%\dxil\%4.bin" "%SRC%%1.hlsl" || exit /b 1
"%DXC%" -T %3 -E %2 %SPIRV_FLAGS% -Fo "%OUT%\spirv\%4.bin" "%SRC%%1.hlsl" || exit /b 1
exit /b 0
