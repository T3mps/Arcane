-- enkiTS premake5 build script
-- zlib license -- permissively licensed C++11 task scheduler
-- Consumed by the Arcane workspace (and Astra's IWorkScheduler adapter, M1+).

project "enkiTS"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/TaskScheduler.cpp",
        "src/TaskScheduler.h",
        "src/LockLessMultiReadPipe.h",
    }

    includedirs { "src" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
