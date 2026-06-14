-- Arcane Engine Workspace
-- premake5 for Visual Studio 2026 (generates Arcane.slnx)
--
-- CRT rule (architecture spec 2026-06-11, decision 3): the entire Arcane
-- workspace builds /MD (dynamic CRT) -- memory crosses the Arcane.dll /
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
        buildoptions { "/utf-8" }
    filter {}

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
    -- imgui lives inside Arcane.dll (ImGuiLayer + first-party imgui_impl_nvrhi).
    -- Export it so GImGui and the sdl3 backend symbols live in ONE module:
    -- the imgui static lib builds with dllexport, the DLL's own TUs match,
    -- and consumers (ArcaneTests/Playground) import. Without this each module
    -- keeps its own null GImGui and ShowDemoWindow() from the test exe asserts.
    THIRDPARTY_IMGUI_API        = "__declspec(dllexport)"

    IncludeDir = {}
    IncludeDir["Core"]             = "%{wks.location}/Core/src"
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

group "Dependencies"
    include "../ThirdParty/Catch2"
    include "../ThirdParty/rapidcheck"
    include "../ThirdParty/enkiTS"
    include "../ThirdParty/tracy"
    include "../ThirdParty/freetype"
    include "../ThirdParty/msdfgen"
    include "../ThirdParty/nvrhi"
    include "../ThirdParty/imgui"
group ""

-- ============================================================================
-- Core: Arcane.Core static lib (presentation-free; also compiled by the
-- Server workspace as project "ArcaneCore" with static CRT)
-- ============================================================================
project "Core"
    location "Core"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

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
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }

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
-- relative paths are disjoint from Core's root (Net/Crypto/Types/Util),
-- so <Arcane/...> resolves unambiguously across both.
-- ============================================================================
project "Arcane"
    location "Arcane"
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
        "%{IncludeDir.Core}",
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
        "%{IncludeDir.msdfgen}",
        "%{IncludeDir.freetype}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
    }

    links { "Core", "nvrhi", "msdfgen", "freetype", "imgui", "enkiTS" }

    -- Force EVERY imgui object (incl. imgui_demo's ShowDemoWindow) into the
    -- DLL so their dllexport symbols are emitted: a dllexport in a static-lib
    -- object only produces an export when the linker actually pulls that
    -- object in, and the DLL itself references only a subset of the imgui API.
    -- /WHOLEARCHIVE exports the full surface for consumers (tests/Playground).
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
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
        -- d3d12/dxgi/dxguid: D3D12 backend. SDL3-static + system libs:
        -- the platform layer (list mirrors SDL3's pkgconfig Libs line).
        links {
            "d3d12", "dxgi", "dxguid",
            "SDL3-static",
            "user32", "gdi32", "winmm", "imm32", "ole32", "oleaut32",
            "version", "uuid", "advapi32", "setupapi", "shell32", "dinput8",
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
        -- a layout mismatch between Arcane.dll and the statically-linked NVRHI
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
-- Playground: standalone exe, the living integration test (stack spec).
-- M1 scope: window + device + clear + present on both backends. Becomes
-- PlaygroundGame.dll under Loom in M4.
-- ============================================================================
project "Playground"
    location "Playground"
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
        "%{wks.location}/Arcane/src",
        "%{IncludeDir.Core}",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
    }

    links { "Arcane" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        -- imgui is exported from Arcane.dll; the demo's stats overlay (Task 8)
        -- imports it. Matches the DLL's dllexport.
        "IMGUI_API=__declspec(dllimport)",
    }

    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/../Client/data/font/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/Playground/data/input_actions.json" "%{cfg.buildtarget.directory}/data/input_actions.json"',
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        -- NDEBUG must match NVRHI's Release build (nvrhi/premake5.lua defines
        -- NDEBUG in Release). DispatchLoaderDynamic has NDEBUG-gated fields;
        -- a layout mismatch between Arcane.dll and the statically-linked NVRHI
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
-- ArcaneTests: Catch2 + rapidcheck (Server conventions). Links Core
-- directly -- Core links into exactly ONE module per process.
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
    }

    includedirs {
        "%{IncludeDir.Core}",
        "%{wks.location}/Arcane/src",
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
    }

    -- msdfgen and freetype are static libs compiled separately; the smoke test
    -- calls them directly (not via Arcane.dll), so both appear in links here.
    -- Two static copies in different modules is the established pattern for this workspace.
    -- imgui is NOT linked here: it is exported from Arcane.dll (IMGUI_API =
    -- dllimport below), so the test exe shares the DLL's single GImGui rather
    -- than carrying a second null context. The import lib comes via "Arcane".
    links { "Core", "Arcane", "Catch2", "rapidcheck", "enkiTS", "freetype", "msdfgen" }

    dependson { "HotReloadPluginV1", "HotReloadPluginV2", "HotReloadPluginBad" }

    -- The test exe loads Arcane.dll from its own directory.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
        '{COPYDIR} "%{wks.location}/shaders/generated" "%{cfg.buildtarget.directory}/shaders"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/../Client/data/font/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/Playground/data/input_actions.json" "%{cfg.buildtarget.directory}/data/input_actions.json"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV1/HotReloadPluginV1.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV1.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginV2/HotReloadPluginV2.dll" "%{cfg.buildtarget.directory}/HotReloadPluginV2.dll"',
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/HotReloadPluginBad/HotReloadPluginBad.dll" "%{cfg.buildtarget.directory}/HotReloadPluginBad.dll"',
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        -- imgui is exported from Arcane.dll; this exe imports it (matches the
        -- DLL's dllexport so <imgui.h> here resolves to the DLL's symbols).
        "IMGUI_API=__declspec(dllimport)",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        links { "ws2_32" }  -- Core TcpSocket

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
-- Bad ABI). SharedLib, /MD, links Arcane (NOT Core -- one Core per process).
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
            "%{wks.location}/Arcane/src",
            "%{IncludeDir.glm}",
            "%{IncludeDir.nvrhi}",
            "%{IncludeDir.Astra}",
            "%{IncludeDir.enkiTS}",
        }
        links { "Arcane" }
        defines (defs)
        filter "system:windows"
            systemversion "latest"
            buildoptions { "/Zc:__cplusplus", "/bigobj" }
        filter "configurations:Debug"   defines { "ARCANE_DEBUG" }                    runtime "Debug"   symbols "on"
        filter "configurations:Release" defines { "ARCANE_RELEASE", "NDEBUG" }        runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"    defines { "ARCANE_DIST", "NDEBUG" }           runtime "Release" optimize "speed" symbols "off"
        filter {}
end

test_plugin("HotReloadPluginV1",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=1",  "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginV2",  { "GAME_BUILD_DLL", "HOTRELOAD_STEP=10", "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginBad", { "GAME_BUILD_DLL", "HOTRELOAD_ABI_OFFSET=999", "_CRT_SECURE_NO_WARNINGS" })
