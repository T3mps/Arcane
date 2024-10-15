workspace "Arcane"
    architectures { "x64" }
    platforms { "x64" }
    startproject "ArcaneTest"
    configurations { "Debug", "Release" }
    flags { "MultiProcessorCompile" }

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

    IncludeDir = {
        Arcane          = "Arcane/src",
        entt            = "Vendor/entt/include",
        glm             = "Vendor/glm/include",
        nlogmannjson    = "Vendor/nlohmannjson/include",
        jemalloc        = "Vendor/jemalloc/include",
        tomlplusplus    = "Vendor/tomlplusplus/include"
    }

    language "C++"
    cppdialect "C++20"
    staticruntime "on"

    filter "system:windows"
        toolset "v143"    
        systemversion "latest"

    filter "system:linux"
        pic "On"
        systemversion "latest"

    filter "configurations:Debug"
        defines { "ARC_BUILD_DEBUG", "ARC_PROFILE" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARC_BUILD_RELEASE" }
        runtime "Release"
        optimize "speed"

    function setupProject(projectName, projectKind)
        location(projectName)
        kind(projectKind)

        targetdir ("bin/" .. outputdir .. "/%{prj.name}")
        objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

        files {
            "%{prj.location}/src/**.h",
            "%{prj.location}/src/**.cpp",
            "%{prj.location}/src/**.hpp"
        }

        includedirs {
            "%{IncludeDir.Arcane}",
            "%{IncludeDir.entt}",
            "%{IncludeDir.glm}",
            "%{IncludeDir.nlogmannjson}",
            "%{IncludeDir.jemalloc}",
            "%{IncludeDir.tomlplusplus}"
        }

        defines { "_CRT_SECURE_NO_WARNINGS" }

        filter "system:linux"
            links { "pthread", "dl" }
    end

-- Projects
    project "Arcane"
        setupProject("Arcane", "StaticLib")
        filter "system:windows"
            links { "d3d12" }

        postbuildcommands { ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/ArcaneTest") }

    project "ArcaneTest"
        setupProject("ArcaneTest", "SharedLib")
        links { "Arcane" }

        filter "system:windows"
            linkoptions { "/SUBSYSTEM:WINDOWS" }
            defines { "ARCANE_TEST_EXPORTS" }

        filter "system:linux"
            defines { "ARCANE_TEST_EXPORTS" }

        postbuildcommands {
            ("{COPY} %{cfg.buildtarget.relpath} ../bin/" .. outputdir .. "/Loom/")
        }

    project "Loom"
        setupProject("Loom", "ConsoleApp")
        links { "Arcane", "ArcaneTest" }

        filter "system:windows"
            linkoptions { "/SUBSYSTEM:WINDOWS", "/ENTRY:wWinMainCRTStartup" }

        filter "system:linux"
            links { "pthread", "dl" }
