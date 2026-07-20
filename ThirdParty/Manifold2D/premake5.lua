-- Manifold2D -- Aphelyon consumer wrapper (project-only; included by the Arcane
-- workspace, like enkiTS/nvrhi/msdfgen). Builds the vendored include/ + src/ as a
-- StaticLib across the /MD engine boundary (staticruntime parameterized via the
-- workspace's THIRDPARTY_STATICRUNTIME). The standalone workspace + test suite
-- live in the Manifold2D repo (D:\dev\starworks\Manifold2D), NOT here.
-- NOTE: this file is Aphelyon-owned. The vendor-back sync from the standalone
-- repo must copy include/ + src/ ONLY and must NOT overwrite this premake5.lua.
project "Manifold2D"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")
    floatingpoint "Strict"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "include/**.hpp",
        "include/**.inl",
        "src/**.cpp",
    }

    includedirs { "%{IncludeDir.Manifold2D}", "%{IncludeDir.Mosaic}" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }

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

    filter {}
