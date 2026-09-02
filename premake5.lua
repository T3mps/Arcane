-- Arcane Engine Workspace
-- premake5 for Visual Studio 2026 (generates Arcane.slnx)
--
-- CRT rule (architecture spec 2026-06-11, decision 3): the entire Arcane
-- workspace builds /MD (dynamic CRT) -- memory crosses the ArcaneClient.dll /
-- Game.dll boundary, so all modules must share one heap. Server/ and
-- Tools/ keep their static-runtime conventions, unaffected.

-- vcpkg required for SDL3 (platform layer; deep build system -> vcpkg per
-- the repo rule). Overlay triplet x64-windows-static-md pins v143 + /MD.
VCPKG_ROOT = os.getenv("VCPKG_ROOT")
if not VCPKG_ROOT then
    error("VCPKG_ROOT environment variable is not set.\nSet it to your vcpkg installation directory, e.g.:\n  setx VCPKG_ROOT C:\\vcpkg\nThen restart your terminal and re-run GenerateProjects.bat.")
end
VCPKG_INSTALLED_MD = VCPKG_ROOT .. "/installed/x64-windows-static-md"

workspace "Arcane"
    architecture "x64"
    startproject "ArcaneTests"
    configurations { "Debug", "Release", "Dist" }
    multiprocessorcompile "On"

    -- MSVC: /utf-8 ensures source + execution charsets are UTF-8.
    -- Required by fmt 11+ (bundled in spdlog 1.17).
    filter "system:windows"
        buildoptions { "/utf-8", "/arch:AVX2" }   -- AVX2 is the x86 min-spec for the engine (Arcane::Simd)
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }         -- gcc/clang x64 parity; ARM port supplies NEON flags later
    filter {}

    -- C4251 ("needs to have dll-interface"): disabled workspace-wide, deliberately.
    -- Every hit is an ARCANE_API class holding STL members. The warning exists for
    -- DLL/client CRT-layout mismatches; this workspace's foundational rule is /MD
    -- everywhere + one toolset + one shared heap (memory crosses the
    -- ArcaneClient.dll/Game.dll boundary by design), so the mismatch it warns about is
    -- structurally impossible here. The "real" fixes (pimpl-everything, function-
    -- only exports) buy nothing under that contract; Unreal ships with 4251
    -- disabled engine-wide for the same reason.
    disablewarnings { "4251" }

    -- "-md" suffix keeps ThirdParty wrapper outputs (each dep builds into
    -- bin/ under its own dir) separate from the static-CRT flavors the
    -- Server workspace builds from the same wrapper scripts.
    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

    -- Shared ThirdParty wrappers read these; their defaults preserve the
    -- Server/Tools behavior (static CRT, vcxproj in the dep root).
    THIRDPARTY_STATICRUNTIME    = "off"
    THIRDPARTY_PROJECT_LOCATION = "ide-md"
    -- imgui wrapper: SDL3 backend includes come from the same vcpkg -md install
    -- used by the rest of the workspace (identical path to IncludeDir["SDL3"]).
    THIRDPARTY_SDL3_INCLUDE     = VCPKG_INSTALLED_MD .. "/include"
    -- imgui lives inside ArcaneClient.dll (ImGuiLayer + the frame graph's ImGuiNriNode).
    -- Export it so GImGui and the sdl3 backend symbols live in ONE module:
    -- the imgui static lib builds with dllexport, the DLL's own TUs match,
    -- and consumers (ArcaneTests/ArcaneRuntime) import. Without this each module
    -- keeps its own null GImGui and ShowDemoWindow() from the test exe asserts.
    THIRDPARTY_IMGUI_API        = "__declspec(dllexport)"
    -- imgui-node-editor links into ArcaneEditor.exe, which IMPORTS imgui from
    -- ArcaneClient.dll -- its ImGui calls must be dllimport (see the wrapper).
    THIRDPARTY_NODE_EDITOR_IMGUI_API = "__declspec(dllimport)"

    IncludeDir = {}
    IncludeDir["ArcaneCore"]       = "%{wks.location}/ArcaneCore/src"
    IncludeDir["nlohmann"]         = "%{wks.location}/ThirdParty/nlohmann"
    IncludeDir["picosha2"]         = "%{wks.location}/ThirdParty/picosha2"
    IncludeDir["spdlog"]           = "%{wks.location}/ThirdParty/spdlog/include"
    IncludeDir["Catch2"]           = "%{wks.location}/ThirdParty/Catch2/src"
    IncludeDir["rapidcheck"]       = "%{wks.location}/ThirdParty/rapidcheck/include"
    IncludeDir["rapidcheck_catch"] = "%{wks.location}/ThirdParty/rapidcheck/extras/catch/include"
    IncludeDir["glm"]              = "%{wks.location}/ThirdParty/glm"
    IncludeDir["stb"]              = "%{wks.location}/ThirdParty/stb"
    IncludeDir["miniaudio"]        = "%{wks.location}/ThirdParty/miniaudio"
    IncludeDir["Astra"]            = "%{wks.location}/ThirdParty/Astra/include"
    IncludeDir["enkiTS"]           = "%{wks.location}/ThirdParty/enkiTS/src"
    IncludeDir["tracy"]            = "%{wks.location}/ThirdParty/tracy/public"
    IncludeDir["freetype"]         = "%{wks.location}/ThirdParty/freetype/include"
    IncludeDir["msdfgen"]          = "%{wks.location}/ThirdParty/msdfgen"
    IncludeDir["NRI"]              = "%{wks.location}/ThirdParty/NRI/Include"
    IncludeDir["VulkanHeaders"]    = "%{wks.location}/ThirdParty/Vulkan-Headers/include"
    IncludeDir["DirectXHeaders"]   = "%{wks.location}/ThirdParty/DirectX-Headers/include"
    IncludeDir["VMA"]              = "%{wks.location}/ThirdParty/VMA/include"
    IncludeDir["D3D12MA"]          = "%{wks.location}/ThirdParty/D3D12MA/include"
    IncludeDir["SDL3"]             = VCPKG_INSTALLED_MD .. "/include"
    IncludeDir["imgui"]            = "%{wks.location}/ThirdParty/imgui"
    IncludeDir["imguinodeeditor"]  = "%{wks.location}/ThirdParty/imgui-node-editor"
    IncludeDir["Manifold2D"]       = "%{wks.location}/ThirdParty/Manifold2D/include"
    IncludeDir["Mosaic"]           = "%{wks.location}/ThirdParty/Mosaic/include"

group "Dependencies"
    include "ThirdParty/Catch2"
    include "ThirdParty/rapidcheck"
    include "ThirdParty/enkiTS"
    include "ThirdParty/tracy"
    include "ThirdParty/freetype"
    include "ThirdParty/msdfgen"
    include "ThirdParty/NRI"
    include "ThirdParty/imgui"
    include "ThirdParty/imgui-node-editor"
    include "ThirdParty/Manifold2D"
group ""


group "Engine"
-- ============================================================================
-- ArcaneCore: Arcane.Core static lib (presentation-free; also compiled by the
-- Server workspace as project "ArcaneCore" with static CRT -- same project
-- name in both workspaces)
-- ============================================================================
project "ArcaneCore"
    location "ArcaneCore"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    floatingpoint "Strict"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        -- M6 physics module (Arcane/Physics/) uses glm for vec2/mat. glm is
        -- header-only; adding it here keeps ArcaneCore presentation-free.
        "%{IncludeDir.glm}",
        -- Phase 2 lift: ArcaneCore's still-resident Physics/Geometry will
        -- include Manifold2D/Core primitives (FunctionRef/BitSet/Simd/
        -- WorkScheduler) ahead of the Task 2 move.
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"

-- ============================================================================
-- Arcane: the engine DLL. One DLL, modular inside by folder/namespace
-- (Base, Platform, Render for M1; Audio/Text/Assets/UI/Jobs/Plugin later).
-- SDL3 links INTO this DLL; consumers link only the import lib.
-- Second namespaced include root: src/Arcane/{Base,Platform,Render} --
-- relative paths are disjoint from ArcaneCore's root (Net/Crypto/Types/Util),
-- so <Arcane/...> resolves unambiguously across both.
-- ============================================================================
project "ArcaneClient"
    location "ArcaneClient"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    -- Shaders are data: compiled at build time, loaded by name at runtime,
    -- hot-reloadable. The script is the single swap point for ShaderMake.
    prebuildcommands {
        'call "%{wks.location}/data/shaders/compile-shaders.bat"',
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.NRI}",
        "%{IncludeDir.VulkanHeaders}",
        "%{IncludeDir.DirectXHeaders}",
        "%{IncludeDir.DirectXHeaders}/directx",
        "%{IncludeDir.SDL3}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.stb}",
        "%{IncludeDir.miniaudio}",
        "%{IncludeDir.msdfgen}",
        "%{IncludeDir.freetype}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }

    links { "ArcaneCore", "NRI", "msdfgen", "freetype", "imgui", "enkiTS", "Manifold2D" }

    -- Force EVERY imgui object (incl. imgui_demo's ShowDemoWindow) into the
    -- DLL so their dllexport symbols are emitted: a dllexport in a static-lib
    -- object only produces an export when the linker actually pulls that
    -- object in, and the DLL itself references only a subset of the imgui API.
    -- /WHOLEARCHIVE exports the full surface for consumers (tests/ArcaneRuntime).
    -- NOTE: Linux port needs --whole-archive/-l imgui --no-whole-archive instead.
    filter "system:windows"
        linkoptions { "/WHOLEARCHIVE:imgui" }
    filter {}

    defines {
        "ARCANE_BUILD_DLL",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        -- This DLL's own TUs include imgui.h; they must export the same
        -- IMGUI_API as the imgui static lib's object files (set via the
        -- THIRDPARTY_IMGUI_API workspace global). IMGUI_IMPL_API follows.
        "IMGUI_API=__declspec(dllexport)",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
        -- d3d12/dxgi/dxguid: D3D12 backend. SDL3-static + system libs:
        -- the platform layer (list mirrors SDL3's pkgconfig Libs line).
        links {
            "d3d12", "dxgi", "dxguid",
            "SDL3-static",
            "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
            "version", "uuid", "advapi32", "setupapi", "shell32", "dinput8",
            -- dbghelp: MiniDumpWriteDump + StackWalk64/Sym* behind
            -- Arcane/Base/Diagnostics.cpp (crash + hang post-mortem capture).
            "dbghelp",
        }

    filter { "system:windows", "configurations:Debug" }
        libdirs { VCPKG_INSTALLED_MD .. "/debug/lib" }
    filter { "system:windows", "configurations:Release or configurations:Dist" }
        libdirs { VCPKG_INSTALLED_MD .. "/lib" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        -- NDEBUG must match every other statically-linked dependency in this
        -- workspace, in particular NRI's Release build (ThirdParty/NRI/premake5.lua
        -- defines NDEBUG in Release): NRI's D3D12 Agility SDK path and Vulkan
        -- dispatch tables carry NDEBUG-gated layout risk, so a mismatch between
        -- ArcaneClient.dll and the statically-linked NRI causes a function-pointer
        -- lookup to read the wrong offset.
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"

-- ArcaneServer: empty skeleton for the engine-server host (servers consume
-- Arcane tooling -- capability lands here as it gets built). Stub main only.
project "ArcaneServer"
    location "ArcaneServer"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.hpp", "%{prj.location}/src/**.cpp" }

-- ============================================================================
-- ArcaneRuntime: the thin standalone host (ArcaneRuntime.exe). Engine boot +
-- RunLoop + PluginHost. Hosts a project's gameModule (--project; --plugin
-- overrides); nothing to host refuses boot. The ReferenceProject tree is copied
-- beside the exe, so `ArcaneRuntime --project ReferenceProject --frames N` is
-- the scripted GPU-verify. Folded from "Loom" 2026-07-29.
-- ============================================================================
project "ArcaneRuntime"
    location "ArcaneRuntime"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{prj.location}/src",           -- RuntimeApp.hpp (self-resolving quoted include; kept for any future local header)
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Mosaic}",
        -- NRI Phase 2, Task 7: RuntimeApp holds an Arcane::NriGraphContext (the
        -- --nri-graph vehicle), and that header is NRI-typed because the node
        -- authors of Tasks 8-12 are its other consumers. HEADERS ONLY -- this
        -- exe does not link NRI, and every nri:: object it can reach is created
        -- and destroyed inside ArcaneClient.dll. (Linking a second static copy
        -- of NRI into the exe would be the bug -- see NriDevice::
        -- CreateNoneForTests's comment about ArcaneTests' own copy.)
        "%{IncludeDir.NRI}",
    }
    -- ArcaneCore is linked directly (alongside the Arcane DLL) even though the
    -- host-boot layer (HostConfig/GpuContext/FramePerf/ProjectBoot) moved INTO
    -- ArcaneClient.dll as Arcane/Host -- ArcaneRuntime.exe no longer source-compiles
    -- any of it. ArcaneCore stays: it's a cheap, established two-static-copies
    -- pattern (see the ArcaneTests links comment), and other exe TUs may still
    -- want un-exported ArcaneCore APIs directly. ArcaneCore links into exactly
    -- ONE module per PROCESS holds because ArcaneRuntime.exe and ArcaneClient.dll are
    -- distinct modules.
    links { "ArcaneCore", "ArcaneClient" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYDIR} "%{wks.location}/data/shaders/generated" "%{cfg.buildtarget.directory}/data/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{COPYDIR} "%{wks.location}/data/shaders/materials" "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{COPYDIR} "%{wks.location}/data/EngineConfig" "%{cfg.buildtarget.directory}/data/EngineConfig"',
        '{COPYDIR} "%{wks.location}/ReferenceProject" "%{cfg.buildtarget.directory}/ReferenceProject"',
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
        -- Agility SDK redistributable (NRI Phase 1 Task 3): the D3D12 loader
        -- reads this exe's exported D3D12SDKPath (".\D3D12\", see main.cpp)
        -- and looks there for D3D12Core.dll to unlock enhanced barriers /
        -- ID3D12Device10+ in NRI's D3D12 backend.
        '{MKDIR} "%{cfg.buildtarget.directory}/D3D12"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/D3D12Core.dll" "%{cfg.buildtarget.directory}/D3D12/D3D12Core.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/d3d12SDKLayers.dll" "%{cfg.buildtarget.directory}/D3D12/d3d12SDKLayers.dll"',
    }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }                   runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" }       runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }          runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- Arcane Editor: the editor shell (ArcaneEditor.exe). Engine boot + RunLoop + PluginHost
-- + ImGui docking shell. Hosts the open project's gameModule. Consumes the engine's
-- host-boot helpers (Arcane::GpuContext/FramePerf/HostConfig, Arcane/Host/ --
-- exported ARCANE_API from ArcaneClient.dll, same as ArcaneRuntime) rather than source-
-- compiling its own copy. Consumes only ARCANE_API otherwise.
-- ============================================================================
project "ArcaneEditor"
    location "ArcaneEditor"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
    }
    includedirs {
        "%{prj.location}/src",
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
        -- NRI Phase 3, Task 8: EditorApp holds TWO Arcane::NriGraphContexts on
        -- the `--nri-graph` flavor (the host-window "chrome" one and the
        -- offscreen one the Viewport panel samples), and that header is
        -- NRI-typed. HEADERS ONLY, exactly as ArcaneRuntime takes it (see that
        -- project's matching comment): this exe does not link NRI, and every
        -- nri:: object it can reach is created and destroyed inside
        -- ArcaneClient.dll. Linking a second static copy of NRI into the exe
        -- would be the bug.
        "%{IncludeDir.NRI}",
    }
    links { "ArcaneCore", "ArcaneClient", "imgui-node-editor" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYDIR} "%{wks.location}/data/shaders/generated" "%{cfg.buildtarget.directory}/data/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{COPYDIR} "%{wks.location}/data/shaders/materials" "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data"',
        '{COPYDIR} "%{wks.location}/data/EngineConfig" "%{cfg.buildtarget.directory}/data/EngineConfig"',
        '{COPYDIR} "%{wks.location}/ReferenceProject" "%{cfg.buildtarget.directory}/ReferenceProject"',
        -- Editor fonts: Inter (default) + Roboto faces + lucide icon font, merged into
        -- the ImGui atlas by EditorFonts.cpp (exe-relative paths -- must align w/ dests).
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/lucide"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/inter/static"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/roboto/static"',
        '{COPYFILE} "%{wks.location}/data/font/inter/static/Inter_18pt-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/inter/static/Inter_18pt-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/roboto/static/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/roboto/static/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/lucide/lucide.ttf" "%{cfg.buildtarget.directory}/data/font/lucide/lucide.ttf"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/aldotheapache"',
        '{COPYFILE} "%{wks.location}/data/font/aldotheapache/AldotheApache.ttf" "%{cfg.buildtarget.directory}/data/font/aldotheapache/AldotheApache.ttf"',
        -- Arcane logo: window/taskbar icon (Window::SetIcon) + transport-toolbar mark
        -- (LoadDisplayTexture). Same PNG, exe-relative at "data/images/arcane_logo.png".
        '{MKDIR} "%{cfg.buildtarget.directory}/data/images"',
        '{COPYFILE} "%{wks.location}/data/images/arcane_logo.png" "%{cfg.buildtarget.directory}/data/images/arcane_logo.png"',
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
        -- Agility SDK redistributable (NRI Phase 1 Task 3): the D3D12 loader
        -- reads this exe's exported D3D12SDKPath (".\D3D12\", see main.cpp)
        -- and looks there for D3D12Core.dll to unlock enhanced barriers /
        -- ID3D12Device10+ in NRI's D3D12 backend.
        '{MKDIR} "%{cfg.buildtarget.directory}/D3D12"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/D3D12Core.dll" "%{cfg.buildtarget.directory}/D3D12/D3D12Core.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/d3d12SDKLayers.dll" "%{cfg.buildtarget.directory}/D3D12/d3d12SDKLayers.dll"',
    }
    filter "system:windows"
        systemversion "latest"
        -- /bigobj (Task 3, F2a): EditorApp.cpp aggregates a wide include surface
        -- (Project.hpp, MaterialAsset.hpp, PanelRegistry.hpp, ...) that pulls in
        -- Components.hpp header-only, whose ASTRA_REFLECT_TYPE blocks re-expand
        -- PER TU. MeshRenderer's two fields pushed this TU's /ZI object past the
        -- COFF section limit (C1128); the other projects in this file that hit
        -- the same limit already carry this flag (see /bigobj above).
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        -- .exe file icon (Explorer/taskbar/Alt-Tab): a Win32 ICON resource. The .rc
        -- references arcane.ico by name; resincludedirs points RC at its folder.
        files { "%{prj.location}/resources/ArcaneEditor.rc" }
        resincludedirs { "%{prj.location}/resources" }
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
group ""

-- ============================================================================
-- ArcaneTests: Catch2 + rapidcheck (Server conventions). Links ArcaneCore
-- directly -- ArcaneCore links into exactly ONE module per process.
-- ============================================================================
-- Solution folder for the test exe + its three hot-reload fixture DLLs, so
-- the fixtures stop reading as top-level products. "Tests", not
-- "ArcaneTests": a folder with the same name as a sibling project collides
-- in the solution tree.
group "Tests"

project "ArcaneTests"
    location "ArcaneTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
        -- HostConfig (the typed host CLI result over Arcane::Cli), GpuContext,
        -- FramePerf, and ProjectBoot all moved into Arcane/Host (ArcaneClient.dll,
        -- ARCANE_API) alongside Module/Plugin/PluginHost (Arcane/Plugin) -- the
        -- test exe now consumes all of them via the "Arcane" link, not source-
        -- compiled. [host] round-trips HostConfig::Parse without loading ArcaneRuntime.exe.
        -- Task 3: ConsoleBuffer (Arcane Editor's log ring buffer) source-compiles into the
        -- test exe so the [editor] unit test drives it directly.
        "%{wks.location}/ArcaneEditor/src/Panels/ConsoleBuffer.cpp",
        -- Task 4: ViewportInput (pure input-gating predicates for the scene-in-a-
        -- panel viewport) source-compiles into the test exe so the [editor] unit
        -- tests drive it directly, same pattern as ConsoleBuffer above.
        "%{wks.location}/ArcaneEditor/src/Viewport/ViewportInput.cpp",
        -- Task 5: EntityList (entity enumeration for the Hierarchy panel)
        -- source-compiles into the test exe so the [editor] unit tests drive it
        -- directly, same pattern as ConsoleBuffer/ViewportInput above.
        "%{wks.location}/ArcaneEditor/src/Panels/EntityList.cpp",
        -- Task 6: InspectorFields (reflected field classification + pure write-backs
        -- for the Inspector panel) source-compiles into the test exe so the
        -- [editor] unit tests drive it directly -- no ImGui dependency, same
        -- pattern as ConsoleBuffer/ViewportInput/EntityList above.
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorFields.cpp",
        -- Task 8: PlayMode (play-in-editor snapshot/restore state machine)
        -- source-compiles into the test exe so the [editor] round-trip test
        -- drives it directly against a real Arcane::Runtime, same pattern as
        -- ConsoleBuffer/ViewportInput/EntityList/InspectorFields above.
        "%{wks.location}/ArcaneEditor/src/App/PlayMode.cpp",
        -- Shader-editor Slice 5: DocumentHost (open-document list + unsaved-
        -- close confirm state machine + asset->editor routing) source-compiles
        -- into the test exe so the [editor] units drive the PURE close flow
        -- with fake documents -- DrawAll (the only ImGui method) is not called.
        "%{wks.location}/ArcaneEditor/src/Documents/DocumentHost.cpp",
        -- Shader-editor review fixes: ShaderEditorDocument source-compiles into
        -- the test exe so the [editor] units drive its HEADLESS halves directly
        -- (save-before-bind, parent-chain resolution, compile-result routing).
        -- Draw (the ImGui half) is never called; device-less services skip the
        -- preview resources in the ctor.
        "%{wks.location}/ArcaneEditor/src/Documents/ShaderEditorDocument.cpp",
        -- Outliner slice 4: ComponentCatalog (registry enumeration + the one
        -- system-managed hide-list + selection-aware missing counts) source-
        -- compiles into the test exe so the [editor] units drive it directly --
        -- no ImGui dependency, same pattern as EntityList/InspectorFields above.
        "%{wks.location}/ArcaneEditor/src/Scene/ComponentCatalog.cpp",
        -- Scene authoring: SceneSession (scene identity + dirty state + the
        -- unsaved-changes confirm machine) source-compiles into the test exe so
        -- the [editor] units drive the PURE state machine directly -- there is
        -- no ImGui in it at all, same pattern as DocumentHost above.
        "%{wks.location}/ArcaneEditor/src/Scene/SceneSession.cpp",
        -- Inspector polish: InspectorMeta (display-name derivation, attribute
        -- extraction, filter matching) source-compiles into the test exe so the
        -- [editor] units drive it directly. It is the whole surface the user
        -- reads in the Inspector, and EditorPanels.cpp is not compiled here --
        -- so anything left in the draw loop would have no coverage at all.
        "%{wks.location}/ArcaneEditor/src/Panels/InspectorMeta.cpp",
        -- Scene authoring: EditorCamera (the editor's own viewport pan/zoom/
        -- framing math + the framing-bounds sweep) source-compiles into the
        -- test exe so the [editor] units drive the PURE math headlessly -- no
        -- ImGui and no engine calls in it, same pattern as SceneSession above.
        "%{wks.location}/ArcaneEditor/src/Viewport/EditorCamera.cpp",
        -- Widget layer: EditGesture's PURE decision core (gesture ownership +
        -- close-path verdicts) source-compiles into the test exe so the
        -- [editor] units drive the full decision table headlessly -- the ImGui
        -- skin in the same TU is never called, same pattern as
        -- ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/Scene/EditGesture.cpp",
        -- Widget layer: EditorWidgets is here as a LINK dependency, not a unit
        -- surface -- ShaderEditorDocument.cpp (compiled above) calls
        -- StableTextEdit for its four inline rename rows, and without this the
        -- test exe fails to link (LNK2019). Nothing in it is called headlessly;
        -- it is pure ImGui, like the skin half of EditGesture.cpp.
        "%{wks.location}/ArcaneEditor/src/Widgets/EditorWidgets.cpp",
        -- Colour pipeline + dense picker Task 7: ColorPickerPopup is here as a
        -- LINK dependency, not a unit surface, same reason as EditorWidgets.cpp
        -- above -- ShaderEditorDocument.cpp (compiled above) now calls
        -- ColorPopupId/ColorSwatchButton/ColorPopupBody for the material-param
        -- row and the ConstColor node, and without this the test exe fails to
        -- link (LNK2019). Nothing in it is called headlessly; it is pure ImGui.
        "%{wks.location}/ArcaneEditor/src/Widgets/ColorPickerPopup.cpp",
        -- Widget layer Task 7: SpriteDocument source-compiles into the test exe
        -- so the [editor] units drive its UNDO half directly (ApplySpriteData,
        -- the before/after step builder, and the doc-identity anchor after the
        -- document closes). Draw (the only ImGui method) is never called --
        -- same precedent as ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/Documents/SpriteDocument.cpp",
        -- F2a, Task 9: MeshDocument source-compiles into the test exe so the
        -- [editor][mesh] units drive its HEADLESS halves directly (data/undo
        -- following SpriteDocument above; the offscreen-preview lifecycle
        -- following ShaderEditorDocument's -- EnsurePreviewContext/
        -- RenderPreview/DestroyPreviewContext). Draw (the ImGui half) is
        -- never called; device-less services skip the preview resources in
        -- the ctor, same precedent as ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/Documents/MeshDocument.cpp",
        -- GPU crash diagnostics arc, Task 10: CrashReportDocument source-
        -- compiles into the test exe so the [editor][diag] units drive its
        -- PURE model half directly (construction from an already-loaded
        -- Diag::Envelope, the CPU-report-noise filters, the .gpudump
        -- sibling's parsed section inventory). Draw (the only ImGui method)
        -- is never called -- same precedent as ShaderEditorDocument/
        -- SpriteDocument above.
        "%{wks.location}/ArcaneEditor/src/Documents/CrashReportDocument.cpp",
        -- Task 5 (runtime-host-fold arc): RuntimeLaunch's PURE candidate-list/
        -- argv builder (ExeCandidates/BuildArgs) source-compiles into the test
        -- exe so the [editor] units drive them directly. SpawnDetached (the
        -- one CreateProcessW call in the file) is compiled here too but never
        -- invoked by any test -- process creation is desk-verify territory,
        -- same "no spawn test" rule the task brief states outright.
        "%{wks.location}/ArcaneEditor/src/Project/RuntimeLaunch.cpp",
        -- Diagnostics arc: DiagnosticStore (key -> current diagnostic set, the
        -- publication-group replace semantics, filter/sort/count) source-compiles
        -- into the test exe so the [diagnostics] units drive it directly -- no
        -- ImGui in it at all, same pattern as SceneSession/EditorCamera above.
        "%{wks.location}/ArcaneEditor/src/Panels/DiagnosticStore.cpp",
        -- Diagnostics arc: ConsoleModel (category derivation from the engine's
        -- "Subsystem: " log prefixes + identical-row collapsing) source-compiles
        -- into the test exe so the [editor] units drive the pure functions the
        -- Console panel draws with -- EditorPanels.cpp is not compiled here.
        "%{wks.location}/ArcaneEditor/src/Panels/ConsoleModel.cpp",
        -- File -> Open Recent: RecentProjects source-compiles into the test exe
        -- so the [editor] units drive its PURE half directly -- parsing the
        -- Hub's shared recents.archub, ABI/current/missing selection, and the
        -- move-to-front touch that must preserve fields the editor does not
        -- model. No ImGui in it at all, same pattern as SceneSession/
        -- EditorCamera above. This file writes to a file the HUB owns, so the
        -- refuse-to-clobber rules are the ones most worth pinning.
        "%{wks.location}/ArcaneEditor/src/Project/RecentProjects.cpp",
        -- File -> Open Recent Scene: SceneRecents' pure list ops (Parse/
        -- Serialize/Push, and the file I/O around them) source-compile into
        -- the test exe so the [editor] units drive them directly. Unlike
        -- RecentProjects, this file has no "never clobber" contract to pin --
        -- it is the editor's own per-project file, not the Hub's shared one.
        "%{wks.location}/ArcaneEditor/src/Project/SceneRecents.cpp",
        -- Build -> Rebuild Game Module: ModuleBuild's PURE halves (solution
        -- discovery, the SDK-root walk, the composed premake+msbuild line)
        -- source-compile into the test exe so the [editor] units drive them
        -- directly. The Runner/_wpopen half and the vswhere probe are compiled
        -- too but never invoked by any test -- process creation is desk-verify
        -- territory, the same rule as RuntimeLaunch's SpawnDetached above.
        "%{wks.location}/ArcaneEditor/src/Project/ModuleBuild.cpp",
    }

    includedirs {
        "%{IncludeDir.ArcaneCore}",
        "%{wks.location}/ArcaneClient/src",
        "%{wks.location}/ArcaneEditor/src",  -- ConsoleBuffer.hpp for the [editor] test
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Catch2}",
        "%{IncludeDir.rapidcheck}",
        "%{IncludeDir.rapidcheck_catch}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.stb}",
        "%{IncludeDir.miniaudio}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.freetype}",
        "%{IncludeDir.msdfgen}",
        "%{IncludeDir.NRI}",   -- Task 4: NriSubstrateTest.cpp drives nri::Result/Device/nriCreateDevice directly
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",   -- ShaderEditorDocument.cpp (graph canvas, Slice 9)
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }

    -- msdfgen, freetype, and NRI are static libs compiled separately; the smoke
    -- test calls them directly (not via ArcaneClient.dll), so all three appear
    -- in links here. Two static copies in different modules is the established
    -- pattern for this workspace. NRI: the [nri] NONE-backend lifecycle test
    -- (Task 4) calls nriCreateDevice/nriDestroyDevice directly, same reasoning.
    -- imgui is NOT linked here: it is exported from ArcaneClient.dll (IMGUI_API =
    -- dllimport below), so the test exe shares the DLL's single GImGui rather
    -- than carrying a second null context. The import lib comes via "ArcaneClient".
    -- imgui-node-editor IS linked (a plain static lib compiled with
    -- IMGUI_API=dllimport, same as this exe): ShaderEditorDocument.cpp's graph
    -- canvas calls it, and that TU source-compiles into the tests.
    links { "ArcaneCore", "ArcaneClient", "Catch2", "rapidcheck", "enkiTS", "freetype", "msdfgen", "NRI", "Manifold2D", "imgui-node-editor" }

    -- MOSAIC_ENSURE/MOSAIC_ENSURE_ALWAYS (most of AssertRoutingTest.cpp) are
    -- defined UNCONDITIONALLY, outside the MOSAIC_ASSERTS_ACTIVE gate this
    -- define controls (Mosaic/Assert.hpp:227-247) -- those cases were never
    -- at risk of per-config drift. MOSAIC_ASSERT/MOSAIC_VERIFY are what this
    -- define actually keeps alive: they compile out entirely under NDEBUG
    -- (Assert.hpp:193-198) unless forced on, and AssertRoutingTest.cpp's
    -- fatal-guard-class case exercises MOSAIC_ASSERT -- without this define
    -- that ONE case would silently vanish in Release/Dist while the rest of
    -- [assert] kept passing, which is exactly the kind of drift that is easy
    -- to miss. Scoped to this project: the hosts are untouched and keep
    -- their normal per-config behaviour.
    defines { "MOSAIC_ENABLE_ASSERTS" }

    dependson { "HotReloadPluginV1", "HotReloadPluginV2", "HotReloadPluginBad" }

    -- The test exe loads ArcaneClient.dll from its own directory.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYDIR} "%{wks.location}/data/shaders/generated" "%{cfg.buildtarget.directory}/data/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{COPYDIR} "%{wks.location}/data/shaders/materials" "%{cfg.buildtarget.directory}/data/shaders/materials"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/data/font/roboto/static/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV1/HotReloadPluginV1.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV1.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV2/HotReloadPluginV2.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV2.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginBad/HotReloadPluginBad.dll" "%{cfg.buildtarget.directory}/HotReloadPluginBad.dll"',
        -- Test data fixtures: copy ArcaneTests/data's CONTENTS into the test output
        -- dir's data/ so tests find their fixtures by relative path. {COPYDIR}
        -- copies the directory's contents, merging with the data/fonts dir the
        -- lines above create. (The M6 physics_oracle fixtures were retired in
        -- v2 T8; physics_feel_reference/ now holds the Phase-B Lua feel traces.)
        '{COPYDIR} "%{wks.location}/ArcaneTests/data" "%{cfg.buildtarget.directory}/data"',
        -- Playwright's own image-comparison fixture corpus (Task 6): the
        -- CONFORMANCE ORACLE for ImageCompare. ImageCompareConformanceTest.cpp
        -- walks this tree by a RELATIVE path from the exe's own directory.
        '{COPYDIR} "%{wks.location}/ThirdParty/playwright-fixtures" "%{cfg.buildtarget.directory}/playwright-fixtures"',
        -- Task 10 (plan-b comparator): ONLY the committed layout seed, not
        -- the whole ReferenceProject tree the two HOST exes stage (this
        -- project's own COPYDIR two screens up, and ArcaneEditor's/
        -- ArcaneRuntime's matching lines) -- GoldenImageTest.cpp's
        -- "[golden]" case is a CHEAP, no-GPU text check of this ONE file and
        -- nothing else under ReferenceProject/Saved/ is read from this exe.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Saved"',
        '{COPYFILE} "%{wks.location}/ReferenceProject/Saved/verify-layout.ini" "%{cfg.buildtarget.directory}/ReferenceProject/Saved/verify-layout.ini"',
        -- Task 11 (plan-b comparator): the engine trap corpus. Task 10's own
        -- comment (above, now narrowed) predicted this would need widening
        -- "once Task 12's [gpu][golden] cases want staged reference images" --
        -- it arrived one task early instead, because ImageCompareConformance
        -- Test.cpp's trap case (Step 3) reads ReferenceProject/Verify/Traps/
        -- by a relative path from this exe's own directory, same as the
        -- playwright-fixtures COPYDIR above. Staged as a NAMED subtree
        -- (Verify/, not the whole ReferenceProject) rather than mirroring the
        -- host exes' full {COPYDIR}: nothing else under ReferenceProject/ is
        -- read from this exe today. When Task 12 lands its own References/
        -- images under this same Verify/ tree, this one line already covers
        -- them -- no further widening needed.
        '{COPYDIR} "%{wks.location}/ReferenceProject/Verify" "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
        -- Agility SDK redistributable (NRI Phase 1 Task 3): the D3D12 loader
        -- reads this exe's exported D3D12SDKPath (".\D3D12\", see test_main.cpp)
        -- and looks there for D3D12Core.dll to unlock enhanced barriers /
        -- ID3D12Device10+ in NRI's D3D12 backend.
        '{MKDIR} "%{cfg.buildtarget.directory}/D3D12"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/D3D12Core.dll" "%{cfg.buildtarget.directory}/D3D12/D3D12Core.dll"',
        '{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/d3d12SDKLayers.dll" "%{cfg.buildtarget.directory}/D3D12/d3d12SDKLayers.dll"',
        -- The exclusion list is repo-level config that BOTH consumers read: this
        -- suite (ExclusionExpiryTest) and golden-gate.ps1. Staged beside the exe
        -- because tests run FROM the exe dir. An edit therefore needs a rebuild
        -- to take effect here, same as data/ and playwright-fixtures/ above.
        '{COPYFILE} "%{wks.location}/scripts/automation-exclusions.json" "%{cfg.buildtarget.directory}/automation-exclusions.json"',
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        -- imgui is exported from ArcaneClient.dll; this exe imports it (matches the
        -- DLL's dllexport so <imgui.h> here resolves to the DLL's symbols).
        "IMGUI_API=__declspec(dllimport)",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        -- ws2_32: ArcaneCore TcpSocket. d3d12/dxgi/dxguid: this exe's own
        -- statically-linked NRI.lib copy (Task 4's [nri] NONE-backend test
        -- links NRI directly, same "two static copies" reasoning as the
        -- includedirs/links comment above) pulls in NRI's D3D12 backend
        -- object code, which references D3D12CreateDevice/CreateDXGIFactory2/
        -- WKPDID_D3DDebugObjectName etc. even though the test never exercises
        -- the D3D12 backend -- mirrors ArcaneClient's own system-lib set.
        links { "ws2_32", "d3d12", "dxgi", "dxguid" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"

-- ============================================================================
-- Hot-reload TEST plugins: one source, three DLLs (V1 step=1, V2 step=10,
-- Bad ABI). SharedLib, /MD, links Arcane (NOT ArcaneCore -- one ArcaneCore per process).
-- Loaded at runtime by PluginHost in ArcaneTests; never linked by the test exe.
-- ============================================================================
local function test_plugin(name, defs)
    project(name)
        location "ArcaneTests/plugins"
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        targetname(name)
        targetdir ("bin/" .. outputdir .. "/" .. name)
        objdir ("bin-int/" .. outputdir .. "/" .. name)
        files { "%{prj.location}/HotReloadPlugin.cpp", "%{prj.location}/PluginExport.hpp", "%{prj.location}/HotReloadShared.hpp" }
        includedirs {
            "%{wks.location}/ArcaneClient/src",
            "%{IncludeDir.ArcaneCore}",   -- Runtime.hpp (plugin API) includes <Arcane/Guid.hpp>
            "%{IncludeDir.glm}",
            "%{IncludeDir.Astra}",
            "%{IncludeDir.enkiTS}",
            "%{IncludeDir.Mosaic}",   -- Astra headers now #include <Mosaic/...> (Mosaic-seam adoption)
        }
        links { "ArcaneClient" }
        defines (defs)
        filter "system:windows"
            systemversion "latest"
            buildoptions { "/Zc:__cplusplus", "/bigobj" }
            fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        filter "configurations:Debug"   defines { "ARCANE_DEBUG" }                    runtime "Debug"   symbols "on"
        filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" }        runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }           runtime "Release" optimize "speed" symbols "off"
        filter {}
end

test_plugin("HotReloadPluginV1",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=1",  "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginV2",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=10", "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginBad", { "GAME_BUILD_DLL", "HOTRELOAD_ABI_OFFSET=999", "_CRT_SECURE_NO_WARNINGS" })

group ""
