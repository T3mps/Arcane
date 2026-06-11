-- Arcane Engine Workspace
-- premake5 for Visual Studio 2026 (generates Arcane.slnx)
--
-- CRT rule (architecture spec 2026-06-11, decision 3): the entire Arcane
-- workspace builds /MD (dynamic CRT) -- memory crosses the Arcane.dll /
-- Game.dll boundary, so all modules must share one heap. Server/ and
-- Tools/ keep their static-runtime conventions, unaffected.

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

group "Dependencies"
    include "../ThirdParty/Catch2"
    include "../ThirdParty/rapidcheck"
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
        defines { "ARCANE_RELEASE" }
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
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Catch2}",
        "%{IncludeDir.rapidcheck}",
        "%{IncludeDir.rapidcheck_catch}",
    }

    links { "Core", "Catch2", "rapidcheck" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        links { "ws2_32" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"
