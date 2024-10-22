workspace "Arcane"
    architecture "x86_64"
    platforms { "x86_64" }
    startproject "ArcaneTest"
    configurations { "Debug", "Release" }
    flags { "MultiProcessorCompile" }

    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

    IncludeDir = {
        Arcane       = "Arcane/src",
        entt         = "Vendor/entt/include",
        glm          = "Vendor/glm/include",
        jemalloc     = "Vendor/jemalloc/include",
        GLFW         = "Vendor/GLFW/include",
        nlogmannjson = "Vendor/nlohmannjson/include",
        tomlplusplus = "Vendor/tomlplusplus/include",
        sol2         = "Vendor/sol2/include"
    }

    language "C++"
    cppdialect "C++latest"
    staticruntime "on"

    filter "system:windows"
        toolset "v143"
        systemversion "latest"
        defines { "ARC_PLATFORM_WINDOWS" }

    filter "system:linux"
        pic "On"
        systemversion "latest"
        defines { "ARC_PLATFORM_LINUX" }

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
            "%{prj.location}/src/**.c",
            "%{prj.location}/src/**.h",
            "%{prj.location}/src/**.cpp",
            "%{prj.location}/src/**.hpp",
        }

        includedirs {
            "%{IncludeDir.Arcane}",
            "%{IncludeDir.entt}",
            "%{IncludeDir.glm}",
            "%{IncludeDir.jemalloc}",
            "%{IncludeDir.GLFW}",
            "%{IncludeDir.nlogmannjson}",
            "%{IncludeDir.tomlplusplus}",
            "%{IncludeDir.sol2}"
        }

        defines { "_CRT_SECURE_NO_WARNINGS" }

        filter "system:linux"
            links { "pthread", "dl" }
    end

-- Projects
    project "Arcane"
        setupProject("Arcane", "StaticLib")

        filter "system:windows"
            links { "GLFW", "d3d12" }

        filter "system:linux"
            links { "GLFW" }

        pchheader "arcpch.h"
        pchsource "Arcane/src/arcpch.cpp"

    project "ArcaneTest"
        setupProject("ArcaneTest", "SharedLib")

        pchheader "arcpch.h"
        pchsource "../Arcane/src/arcpch.cpp"

        filter "system:windows"
            links { "Arcane" }

            postbuildcommands {
                "if not exist \"..\\bin\\" .. outputdir .. "\\Loom\\\" mkdir \"..\\bin\\" .. outputdir .. "\\Loom\\\"",
                "xcopy /Q /Y \"..\\bin\\" .. outputdir .. "\\ArcaneTest\\*.*\" \"..\\bin\\" .. outputdir .. "\\Loom\\\""
            }

        filter "system:linux"
            links { "Arcane" }

            postbuildcommands {
                "mkdir -p \"../bin/" .. outputdir .. "/Loom/\"",
                
                "cp -r \"../bin/" .. outputdir .. "/ArcaneTest/*\" \"../bin/" .. outputdir .. "/Loom/\""
            }

    project "Loom"
        setupProject("Loom", "ConsoleApp")
        
        pchheader "arcpch.h"
        pchsource "../Arcane/src/arcpch.cpp"

        filter "system:windows"
            links { "Arcane" }

        filter "system:linux"
            links { "Arcane", "pthread", "dl" }

    include "Vendor/GLFW"
    