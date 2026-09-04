-- bc7enc_rdo premake5 build script
-- MIT license or public domain (see LICENSE) -- Richard Geldreich, Jr.
--
-- Vendored slice: bc7enc.{h,cpp} (the plain, non-RDO BC7 block encoder) and
-- bc7decomp.{h,cpp} (the BC7 decoder, needed only for the pipeline's own
-- decode-block sanity test). Deliberately EXCLUDES the RDO post-process
-- (ert.*, rdo_bc_encoder.*), the multithreaded CLI wrapper, rgbcx.* (BC1-5),
-- lodepng, and the example CLI/test images -- F2b Task 4's determinism
-- ruling only needs a pinned-parameter, single-block-at-a-time encoder, and
-- bc7enc.cpp/bc7decomp.cpp have no thread/RNG surface at all (verified on
-- the vendoring desk: no rand()/thread/omp/clock() hits in either file),
-- so there is nothing here to prove deterministic beyond "same input,
-- same pinned params, same output" -- true by construction.
--
-- Included by BOTH the Server and Arcane workspaces is NOT expected (Server
-- has no texture pipeline), but this wrapper follows the same two-global
-- convention as every other ThirdParty wrapper so it behaves correctly if
-- that ever changes.

project "bc7enc_rdo"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "*.h",
        "*.cpp",
    }

    includedirs { "." }

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
