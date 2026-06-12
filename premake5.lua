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
    IncludeDir["nvrhi"]            = "%{wks.location}/../ThirdParty/nvrhi/include"
    IncludeDir["VulkanHeaders"]    = "%{wks.location}/../ThirdParty/Vulkan-Headers/include"
    IncludeDir["DirectXHeaders"]   = "%{wks.location}/../ThirdParty/DirectX-Headers/include"
    IncludeDir["SDL3"]             = VCPKG_INSTALLED_MD .. "/include"

group "Dependencies"
    include "../ThirdParty/Catch2"
    include "../ThirdParty/rapidcheck"
    include "../ThirdParty/enkiTS"
    include "../ThirdParty/tracy"
    include "../ThirdParty/freetype"
    include "../ThirdParty/nvrhi"
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
    }

    links { "Core", "nvrhi" }

    defines {
        "ARCANE_BUILD_DLL",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
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
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.nvrhi}",
    }

    links { "Arcane" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
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
        "%{IncludeDir.nvrhi}",
    }

    links { "Core", "Arcane", "Catch2", "rapidcheck", "enkiTS", "freetype" }

    -- The test exe loads Arcane.dll from its own directory.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/Arcane/Arcane.dll" "%{cfg.buildtarget.directory}/Arcane.dll"',
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
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
