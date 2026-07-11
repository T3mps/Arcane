-- Manifold2D premake5 wrapper -- Starworks 2D physics + geometry, lifted from
-- Arcane/Core in Phase 2. Standalone StaticLib (zero Arcane deps); consumed by
-- the Arcane (/MD) workspace. Honors the shared-wrapper globals so it COULD be
-- built static-CRT later without edits (no Server consumer today).
project "Manifold2D"
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")
    floatingpoint "Strict"   -- determinism: /fp:fast is banned engine-wide

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "include/**.hpp",
        "include/**.inl",
        "src/**.cpp",
    }

    includedirs { "include" }

    defines { "_CRT_SECURE_NO_WARNINGS" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"
        defines { "ARCANE_DEBUG" }

    filter "configurations:Release"
        runtime "Release"
        optimize "speed"
        symbols "on"                 -- match Core: keep symbols in Release (the solver is the hottest, most-debugged code)
        defines { "ARCANE_RELEASE", "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "speed"
        symbols "off"
        defines { "ARCANE_DIST", "NDEBUG" }
