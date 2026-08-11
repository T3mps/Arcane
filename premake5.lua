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
    -- imgui lives inside ArcaneClient.dll (ImGuiLayer + first-party imgui_impl_nvrhi).
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
    IncludeDir["nlohmann"]         = "%{wks.location}/../ThirdParty/nlohmann"
    IncludeDir["picosha2"]         = "%{wks.location}/../ThirdParty/picosha2"
    IncludeDir["spdlog"]           = "%{wks.location}/../ThirdParty/spdlog/include"
    IncludeDir["Catch2"]           = "%{wks.location}/../ThirdParty/Catch2/src"
    IncludeDir["rapidcheck"]       = "%{wks.location}/../ThirdParty/rapidcheck/include"
    IncludeDir["rapidcheck_catch"] = "%{wks.location}/../ThirdParty/rapidcheck/extras/catch/include"
    IncludeDir["glm"]              = "%{wks.location}/../ThirdParty/glm"
    IncludeDir["stb"]              = "%{wks.location}/../ThirdParty/stb"
    IncludeDir["miniaudio"]        = "%{wks.location}/../ThirdParty/miniaudio"
    IncludeDir["Astra"]            = "%{wks.location}/../ThirdParty/Astra/include"
    IncludeDir["enkiTS"]           = "%{wks.location}/../ThirdParty/enkiTS/src"
    IncludeDir["tracy"]            = "%{wks.location}/../ThirdParty/tracy/public"
    IncludeDir["freetype"]         = "%{wks.location}/../ThirdParty/freetype/include"
    IncludeDir["msdfgen"]          = "%{wks.location}/../ThirdParty/msdfgen"
    IncludeDir["nvrhi"]            = "%{wks.location}/../ThirdParty/nvrhi/include"
    IncludeDir["VulkanHeaders"]    = "%{wks.location}/../ThirdParty/Vulkan-Headers/include"
    IncludeDir["DirectXHeaders"]   = "%{wks.location}/../ThirdParty/DirectX-Headers/include"
    IncludeDir["SDL3"]             = VCPKG_INSTALLED_MD .. "/include"
    IncludeDir["imgui"]            = "%{wks.location}/../ThirdParty/imgui"
    IncludeDir["imguinodeeditor"]  = "%{wks.location}/../ThirdParty/imgui-node-editor"
    IncludeDir["Manifold2D"]       = "%{wks.location}/../ThirdParty/Manifold2D/include"
    IncludeDir["Mosaic"]           = "%{wks.location}/../ThirdParty/Mosaic/include"

group "Dependencies"
    include "../ThirdParty/Catch2"
    include "../ThirdParty/rapidcheck"
    include "../ThirdParty/enkiTS"
    include "../ThirdParty/tracy"
    include "../ThirdParty/freetype"
    include "../ThirdParty/msdfgen"
    include "../ThirdParty/nvrhi"
    include "../ThirdParty/imgui"
    include "../ThirdParty/imgui-node-editor"
    include "../ThirdParty/Manifold2D"
group ""

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
-- NVRHI and SDL3 link INTO this DLL; consumers link only the import lib.
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
        'call "%{wks.location}/shaders/compile-shaders.bat"',
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
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

    links { "ArcaneCore", "nvrhi", "msdfgen", "freetype", "imgui", "enkiTS", "Manifold2D" }

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
        -- NDEBUG must match NVRHI's Release build (nvrhi/premake5.lua defines
        -- NDEBUG in Release). DispatchLoaderDynamic has NDEBUG-gated fields;
        -- a layout mismatch between ArcaneClient.dll and the statically-linked NVRHI
        -- causes every Vulkan function-pointer lookup to read the wrong offset.
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
-- PlaygroundGame: the M4 scene as a game plugin (the first live ABI consumer).
-- SharedLib, /MD, links Arcane (NOT ArcaneCore). Loaded by ArcaneRuntime at runtime.
-- The reserved Game/ slot stays empty for the future Aphelyon client port.
-- ============================================================================
project "PlaygroundGame"
    location "PlaygroundGame"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",   -- Runtime.hpp (plugin API) includes <Arcane/Guid.hpp>
        "%{IncludeDir.glm}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Mosaic}",
    }
    links { "ArcaneClient" }
    -- ABI v2: the plugin draws ImGui via the host's exported context. imgui is exported
    -- from ArcaneClient.dll (IMGUI_API=dllexport there); the plugin imports it -- one GImGui per
    -- process. The import lib arrives via "ArcaneClient" (imgui surface is /WHOLEARCHIVE-exported).
    defines { "GAME_BUILD_DLL", "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }                   runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" }       runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }          runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- Sandbox: the physics-sandbox game plugin (Arcane Physics Sandbox, Task 4).
-- SharedLib, /MD, v2 ABI. Mirrors PlaygroundGame (imgui import) but ALSO links
-- ArcaneCore: it is the first plugin to drive PhysicsSystem, whose header-only
-- body calls into the ArcaneCore PhysicsWorld implementation (see the links{}
-- note below). Loaded by
-- ArcaneRuntime (--plugin Sandbox.dll) and the SandboxSmokeTest; the DLL is copied
-- beside BOTH ArcaneRuntime.exe and ArcaneTests.exe (its own postbuild) so the host
-- and the test can load it.
-- ============================================================================
project "Sandbox"
    location "Sandbox"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Mosaic}",
    }
    -- Sandbox is the first plugin to drive physics. PhysicsSystem (header-only) is
    -- instantiated in THIS module and calls Arcane::Physics::PhysicsWorld directly;
    -- PhysicsWorld's implementation lives in ArcaneCore (.cpp, not ARCANE_API-exported
    -- from ArcaneClient.dll), so the plugin must link ArcaneCore to resolve those symbols.
    -- ArcaneCore is Astra-free and carries no mutable global state, so the duplicate
    -- static copy is benign: the PhysicsWorld instance is owned entirely within this
    -- module (created here, stepped by this module's PhysicsSystem); ArcaneClient.dll only
    -- ever READS it via DrawPhysicsDebug through a const ref, and both modules compile
    -- the identical ArcaneCore headers under identical flags (/MD, matching NDEBUG,
    -- float-strict) so the layout matches -- the same identical-layout contract the
    -- scene components already rely on.
    links { "ArcaneClient", "ArcaneCore", "Manifold2D" }
    -- ABI v2: the plugin imports imgui from ArcaneClient.dll (one GImGui per process), same
    -- as PlaygroundGame. The import lib arrives via "ArcaneClient" (imgui surface is
    -- /WHOLEARCHIVE-exported there).
    defines { "GAME_BUILD_DLL", "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    floatingpoint "Strict"   -- physics determinism: match ArcaneCore's /fp:strict (no /fp:fast)
    -- Copy Sandbox.dll beside ArcaneRuntime.exe (host) AND ArcaneTests.exe (smoke test).
    -- The Arcane/ArcaneCore include dir is needed because SandboxApp.hpp includes
    -- Arcane/Jobs/TaskExecutor.hpp (ITaskExecutor); PhysicsWorld itself lives in
    -- Manifold2D, not ArcaneCore.
    postbuildcommands {
        '{MKDIR} "%{wks.location}/bin/' .. outputdir .. '/ArcaneRuntime"',
        '{MKDIR} "%{wks.location}/bin/' .. outputdir .. '/ArcaneTests"',
        '{COPYFILE} "%{cfg.buildtarget.abspath}" "%{wks.location}/bin/' .. outputdir .. '/ArcaneRuntime/Sandbox.dll"',
        '{COPYFILE} "%{cfg.buildtarget.abspath}" "%{wks.location}/bin/' .. outputdir .. '/ArcaneTests/Sandbox.dll"',
    }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }                   runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" }       runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }          runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- ArcaneRuntime: the thin standalone host (ArcaneRuntime.exe). Engine boot +
-- RunLoop + PluginHost. Hosts Sandbox.dll by default (the physics/engine
-- showcase); PlaygroundGame.dll is also copied beside ArcaneRuntime.exe as the
-- minimal hot-reload fixture / swap target. Folded from "Loom" 2026-07-29.
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
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Mosaic}",
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
    dependson { "PlaygroundGame", "Sandbox" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/PlaygroundGame/PlaygroundGame.dll" "%{cfg.buildtarget.directory}/PlaygroundGame.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/shaders/materials"',
        '{COPYDIR} "%{wks.location}/shaders/materials" "%{cfg.buildtarget.directory}/shaders/materials"',
        '{COPYDIR} "%{wks.location}/EngineConfig" "%{cfg.buildtarget.directory}/EngineConfig"',
        '{COPYDIR} "%{wks.location}/SampleProject" "%{cfg.buildtarget.directory}/SampleProject"',
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
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
-- + ImGui docking shell. Hosts Sandbox.dll by default. Consumes the engine's
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
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }
    links { "ArcaneCore", "ArcaneClient", "imgui-node-editor" }
    dependson { "Sandbox" }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=__declspec(dllimport)" }
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Sandbox/Sandbox.dll" "%{cfg.buildtarget.directory}/Sandbox.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/shaders/materials"',
        '{COPYDIR} "%{wks.location}/shaders/materials" "%{cfg.buildtarget.directory}/shaders/materials"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data"',
        '{COPYDIR} "%{wks.location}/EngineConfig" "%{cfg.buildtarget.directory}/EngineConfig"',
        '{COPYDIR} "%{wks.location}/SampleProject" "%{cfg.buildtarget.directory}/SampleProject"',
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
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
    }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        -- .exe file icon (Explorer/taskbar/Alt-Tab): a Win32 ICON resource. The .rc
        -- references arcane.ico by name; resincludedirs points RC at its folder.
        files { "%{prj.location}/resources/ArcaneEditor.rc" }
        resincludedirs { "%{prj.location}/resources" }
    filter "configurations:Debug"    defines { "ARCANE_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARCANE_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARCANE_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- ArcaneTests: Catch2 + rapidcheck (Server conventions). Links ArcaneCore
-- directly -- ArcaneCore links into exactly ONE module per process.
-- ============================================================================
project "ArcaneTests"
    location "Tests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
        -- Task 7/8: the Sandbox helper UNITS compile straight into the test exe so the
        -- CPU tests drive them directly (no plugin load): SandboxInteractionTest drives
        -- Interaction::Tick against a hand-built PhysicsWorld; SandboxHudTest drives
        -- SandboxApp + Hud::Draw under a throwaway ImGui context. Only the pure helper
        -- TUs are pulled in -- NOT Sandbox.cpp (the plugin shell with the extern "C"
        -- exports + g_app), which the SandboxSmokeTest exercises via DLL load.
        "%{wks.location}/Sandbox/src/Interaction.cpp",
        "%{wks.location}/Sandbox/src/Scenes.cpp",
        "%{wks.location}/Sandbox/src/SandboxApp.cpp",
        "%{wks.location}/Sandbox/src/Hud.cpp",
        -- HostConfig (the typed host CLI result over Arcane::Cli), GpuContext,
        -- FramePerf, and ProjectBoot all moved into Arcane/Host (ArcaneClient.dll,
        -- ARCANE_API) alongside Module/Plugin/PluginHost (Arcane/Plugin) -- the
        -- test exe now consumes all of them via the "Arcane" link, not source-
        -- compiled. [host] round-trips HostConfig::Parse without loading ArcaneRuntime.exe.
        -- Task 3: ConsoleBuffer (Arcane Editor's log ring buffer) source-compiles into the
        -- test exe so the [editor] unit test drives it directly, mirroring the
        -- Sandbox helper-unit pattern above.
        "%{wks.location}/ArcaneEditor/src/ConsoleBuffer.cpp",
        -- Task 4: ViewportInput (pure input-gating predicates for the scene-in-a-
        -- panel viewport) source-compiles into the test exe so the [editor] unit
        -- tests drive it directly, same pattern as ConsoleBuffer above.
        "%{wks.location}/ArcaneEditor/src/ViewportInput.cpp",
        -- Task 5: EntityList (entity enumeration for the Hierarchy panel)
        -- source-compiles into the test exe so the [editor] unit tests drive it
        -- directly, same pattern as ConsoleBuffer/ViewportInput above.
        "%{wks.location}/ArcaneEditor/src/EntityList.cpp",
        -- Task 6: InspectorFields (reflected field classification + pure write-backs
        -- for the Inspector panel) source-compiles into the test exe so the
        -- [editor] unit tests drive it directly -- no ImGui dependency, same
        -- pattern as ConsoleBuffer/ViewportInput/EntityList above.
        "%{wks.location}/ArcaneEditor/src/InspectorFields.cpp",
        -- Task 8: PlayMode (play-in-editor snapshot/restore state machine)
        -- source-compiles into the test exe so the [editor] round-trip test
        -- drives it directly against a real Arcane::Runtime, same pattern as
        -- ConsoleBuffer/ViewportInput/EntityList/InspectorFields above.
        "%{wks.location}/ArcaneEditor/src/PlayMode.cpp",
        -- Shader-editor Slice 5: DocumentHost (open-document list + unsaved-
        -- close confirm state machine + asset->editor routing) source-compiles
        -- into the test exe so the [editor] units drive the PURE close flow
        -- with fake documents -- DrawAll (the only ImGui method) is not called.
        "%{wks.location}/ArcaneEditor/src/DocumentHost.cpp",
        -- Shader-editor review fixes: ShaderEditorDocument source-compiles into
        -- the test exe so the [editor] units drive its HEADLESS halves directly
        -- (save-before-bind, parent-chain resolution, compile-result routing).
        -- Draw (the ImGui half) is never called; device-less services skip the
        -- preview resources in the ctor.
        "%{wks.location}/ArcaneEditor/src/ShaderEditorDocument.cpp",
        -- Outliner slice 4: ComponentCatalog (registry enumeration + the one
        -- system-managed hide-list + selection-aware missing counts) source-
        -- compiles into the test exe so the [editor] units drive it directly --
        -- no ImGui dependency, same pattern as EntityList/InspectorFields above.
        "%{wks.location}/ArcaneEditor/src/ComponentCatalog.cpp",
        -- Scene authoring: SceneSession (scene identity + dirty state + the
        -- unsaved-changes confirm machine) source-compiles into the test exe so
        -- the [editor] units drive the PURE state machine directly -- there is
        -- no ImGui in it at all, same pattern as DocumentHost above.
        "%{wks.location}/ArcaneEditor/src/SceneSession.cpp",
        -- Inspector polish: InspectorMeta (display-name derivation, attribute
        -- extraction, filter matching) source-compiles into the test exe so the
        -- [editor] units drive it directly. It is the whole surface the user
        -- reads in the Inspector, and EditorPanels.cpp is not compiled here --
        -- so anything left in the draw loop would have no coverage at all.
        "%{wks.location}/ArcaneEditor/src/InspectorMeta.cpp",
        -- Scene authoring: EditorCamera (the editor's own viewport pan/zoom/
        -- framing math + the framing-bounds sweep) source-compiles into the
        -- test exe so the [editor] units drive the PURE math headlessly -- no
        -- ImGui and no engine calls in it, same pattern as SceneSession above.
        "%{wks.location}/ArcaneEditor/src/EditorCamera.cpp",
        -- Widget layer: EditGesture's PURE decision core (gesture ownership +
        -- close-path verdicts) source-compiles into the test exe so the
        -- [editor] units drive the full decision table headlessly -- the ImGui
        -- skin in the same TU is never called, same pattern as
        -- ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/EditGesture.cpp",
        -- Widget layer: EditorWidgets is here as a LINK dependency, not a unit
        -- surface -- ShaderEditorDocument.cpp (compiled above) calls
        -- StableTextEdit for its four inline rename rows, and without this the
        -- test exe fails to link (LNK2019). Nothing in it is called headlessly;
        -- it is pure ImGui, like the skin half of EditGesture.cpp.
        "%{wks.location}/ArcaneEditor/src/EditorWidgets.cpp",
        -- Colour pipeline + dense picker Task 7: ColorPickerPopup is here as a
        -- LINK dependency, not a unit surface, same reason as EditorWidgets.cpp
        -- above -- ShaderEditorDocument.cpp (compiled above) now calls
        -- ColorPopupId/ColorSwatchButton/ColorPopupBody for the material-param
        -- row and the ConstColor node, and without this the test exe fails to
        -- link (LNK2019). Nothing in it is called headlessly; it is pure ImGui.
        "%{wks.location}/ArcaneEditor/src/ColorPickerPopup.cpp",
        -- Widget layer Task 7: SpriteDocument source-compiles into the test exe
        -- so the [editor] units drive its UNDO half directly (ApplySpriteData,
        -- the before/after step builder, and the doc-identity anchor after the
        -- document closes). Draw (the only ImGui method) is never called --
        -- same precedent as ShaderEditorDocument above.
        "%{wks.location}/ArcaneEditor/src/SpriteDocument.cpp",
        -- Task 5 (runtime-host-fold arc): RuntimeLaunch's PURE candidate-list/
        -- argv builder (ExeCandidates/BuildArgs) source-compiles into the test
        -- exe so the [editor] units drive them directly. SpawnDetached (the
        -- one CreateProcessW call in the file) is compiled here too but never
        -- invoked by any test -- process creation is desk-verify territory,
        -- same "no spawn test" rule the task brief states outright.
        "%{wks.location}/ArcaneEditor/src/RuntimeLaunch.cpp",
        -- Diagnostics arc: DiagnosticStore (key -> current diagnostic set, the
        -- publication-group replace semantics, filter/sort/count) source-compiles
        -- into the test exe so the [diagnostics] units drive it directly -- no
        -- ImGui in it at all, same pattern as SceneSession/EditorCamera above.
        "%{wks.location}/ArcaneEditor/src/DiagnosticStore.cpp",
        -- Diagnostics arc: ConsoleModel (category derivation from the engine's
        -- "Subsystem: " log prefixes + identical-row collapsing) source-compiles
        -- into the test exe so the [editor] units drive the pure functions the
        -- Console panel draws with -- EditorPanels.cpp is not compiled here.
        "%{wks.location}/ArcaneEditor/src/ConsoleModel.cpp",
        -- File -> Open Recent: RecentProjects source-compiles into the test exe
        -- so the [editor] units drive its PURE half directly -- parsing the
        -- Hub's shared recents.archub, ABI/current/missing selection, and the
        -- move-to-front touch that must preserve fields the editor does not
        -- model. No ImGui in it at all, same pattern as SceneSession/
        -- EditorCamera above. This file writes to a file the HUB owns, so the
        -- refuse-to-clobber rules are the ones most worth pinning.
        "%{wks.location}/ArcaneEditor/src/RecentProjects.cpp",
        -- File -> Open Recent Scene: SceneRecents' pure list ops (Parse/
        -- Serialize/Push, and the file I/O around them) source-compile into
        -- the test exe so the [editor] units drive them directly. Unlike
        -- RecentProjects, this file has no "never clobber" contract to pin --
        -- it is the editor's own per-project file, not the Hub's shared one.
        "%{wks.location}/ArcaneEditor/src/SceneRecents.cpp",
        -- Build -> Rebuild Game Module: ModuleBuild's PURE halves (solution
        -- discovery, the SDK-root walk, the composed premake+msbuild line)
        -- source-compile into the test exe so the [editor] units drive them
        -- directly. The Runner/_wpopen half and the vswhere probe are compiled
        -- too but never invoked by any test -- process creation is desk-verify
        -- territory, the same rule as RuntimeLaunch's SpawnDetached above.
        "%{wks.location}/ArcaneEditor/src/ModuleBuild.cpp",
    }

    includedirs {
        "%{IncludeDir.ArcaneCore}",
        "%{wks.location}/ArcaneClient/src",
        "%{wks.location}/Sandbox/src",   -- Interaction.hpp / Scenes.hpp / Camera.hpp for the [sandbox] tests
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
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",   -- ShaderEditorDocument.cpp (graph canvas, Slice 9)
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
    }

    -- msdfgen and freetype are static libs compiled separately; the smoke test
    -- calls them directly (not via ArcaneClient.dll), so both appear in links here.
    -- Two static copies in different modules is the established pattern for this workspace.
    -- imgui is NOT linked here: it is exported from ArcaneClient.dll (IMGUI_API =
    -- dllimport below), so the test exe shares the DLL's single GImGui rather
    -- than carrying a second null context. The import lib comes via "ArcaneClient".
    -- imgui-node-editor IS linked (a plain static lib compiled with
    -- IMGUI_API=dllimport, same as this exe): ShaderEditorDocument.cpp's graph
    -- canvas calls it, and that TU source-compiles into the tests.
    links { "ArcaneCore", "ArcaneClient", "Catch2", "rapidcheck", "enkiTS", "freetype", "msdfgen", "Manifold2D", "imgui-node-editor" }

    dependson { "HotReloadPluginV1", "HotReloadPluginV2", "HotReloadPluginBad", "PlaygroundGame", "Sandbox" }

    -- The test exe loads ArcaneClient.dll from its own directory.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/shaders/materials"',
        '{COPYDIR} "%{wks.location}/shaders/materials" "%{cfg.buildtarget.directory}/shaders/materials"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/../Client/data/font/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV1/HotReloadPluginV1.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV1.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV2/HotReloadPluginV2.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV2.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginBad/HotReloadPluginBad.dll" "%{cfg.buildtarget.directory}/HotReloadPluginBad.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/PlaygroundGame/PlaygroundGame.dll" "%{cfg.buildtarget.directory}/PlaygroundGame.dll"',
        -- Test data fixtures: copy Tests/data's CONTENTS into the test output
        -- dir's data/ so tests find their fixtures by relative path. {COPYDIR}
        -- copies the directory's contents, merging with the data/fonts dir the
        -- lines above create. (The M6 physics_oracle fixtures were retired in
        -- v2 T8; physics_feel_reference/ now holds the Phase-B Lua feel traces.)
        '{COPYDIR} "%{wks.location}/Tests/data" "%{cfg.buildtarget.directory}/data"',
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"',
        '{COPYFILE} "%{wks.location}/../ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"',
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
        links { "ws2_32" }  -- ArcaneCore TcpSocket

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
        location "Tests/plugins"
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
            "%{IncludeDir.nvrhi}",
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
