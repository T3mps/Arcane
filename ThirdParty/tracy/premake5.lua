-- Tracy client premake5 build script
-- BSD-3-Clause -- frame profiler client. Compiled in only where
-- TRACY_ENABLE is defined: Debug + Release here; Dist builds the TU
-- with the define absent, which compiles to nothing (zero cost).
-- Windows consumers that link this lib also need ws2_32 + dbghelp.

project "TracyClient"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files { "public/TracyClient.cpp" }

    includedirs { "public" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        defines { "TRACY_ENABLE" }

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG", "TRACY_ENABLE" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
