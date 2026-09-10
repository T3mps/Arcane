-- meshoptimizer premake5 build script
-- MIT license (see LICENSE.md) -- Arseny Kapoulkine
--
-- Vendored at tag v1.2 (9d9890c73011d75920af614485296d1e03e95448). F2c Task 1.
--
-- Vendored slice: the WHOLE upstream src/ directory (~20 small, self-contained
-- TUs, no external dependencies, no build-time configuration) -- a deliberate
-- divergence from bc7enc_rdo's file-level curation (see that wrapper's own
-- header comment for the contrast). File-level curation buys nothing here and
-- risks a link error the day a dormant entry point gets switched on: this pin
-- carries three capabilities vendored-but-unused today --
--   - mesh simplification (simplifier.cpp)      -- spec s2's LOD trigger
--   - EXT_meshopt_compression decode (vertex/indexcodec.cpp) -- spec s2's
--     EXT_meshopt_compression trigger
--   - tangent generation (tangentspace.cpp)      -- spec s2/A3's tangent trigger
-- Excluded: the tooling that surrounds src/ -- gltf/ (the gltfpack CLI), js/,
-- demo/, tools/ (and any future tests/) -- none of it is a library TU.

project "meshoptimizer"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/*.cpp",
        "src/*.h",
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
