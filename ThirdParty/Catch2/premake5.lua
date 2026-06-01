-- Catch2 v3 premake5 build script
-- Boost Software License 1.0 — modern C++ unit-testing framework

project "Catch2"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/catch2/**.cpp",
        "src/catch2/**.hpp",
    }

    -- catch_main.cpp provides a default main() that conflicts with
    -- test executables that define their own; exclude it here.
    -- Tests that want the default runner can link Catch2WithDefs separately
    -- (a future task); for now all test executables provide their own main.
    removefiles { "src/catch2/internal/catch_main.cpp" }

    includedirs { "src" }

    defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
