-- ============================================================================
-- arcane.lua -- the Arcane SDK premake module (engine-as-SDK, Slice 5).
--
-- An EXTERNAL project (one built OUTSIDE the Arcane.slnx workspace) consumes the
-- engine through this file. The "SDK" is the engine's own build output consumed
-- IN PLACE via the ARCANE_SDK env var (a packaged/installed SDK with a
-- multi-version registry is a later nicety -- spec 2026-07-22 S10/S12):
--
--   ARCANE_SDK  ->  the Arcane engine repo root (e.g. D:\dev\starworks\Arcane)
--                   include surface = $ARCANE_SDK/ArcaneClient/src (+ ThirdParty header-only)
--                   import lib       = $ARCANE_SDK/bin/<cfg>-<sys>-x86_64-md/ArcaneClient/ArcaneClient.lib
--                   ArcaneClient.dll        ships beside the host exe (host copies it)
--
-- Usage from a project's premake5.lua:
--   workspace "MyGame"
--       architecture "x64"
--       configurations { "Debug", "Release", "Dist" }
--   include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
--   arcane_game_module("MyGame")   -- declares the SharedLib game module (-> Binaries/MyGame.dll)
--
-- The module implements the extern-C plugin ABI (Arcane/Plugin/PluginABI.hpp --
-- see kGamePluginABIVersion there); the host's ABI gate refuses a cross-build
-- mismatch. The include + link + define set below is the proven minimal
-- game-module recipe (ReferenceProject is the in-repo consumer; Aphelyon the
-- external one) so an external build matches an in-tree one.
-- ============================================================================

-- A consumer may pre-set the ARCANE_SDK global before including this file
-- (the in-repo ReferenceProject self-locates the SDK relatively so a fresh clone
-- builds without any env setup); the env var remains the external-project
-- contract (Aphelyon).
ARCANE_SDK = ARCANE_SDK or os.getenv("ARCANE_SDK")
if not ARCANE_SDK then
    error("ARCANE_SDK environment variable is not set.\n" ..
          "Point it at the Arcane engine repo root, e.g.:\n" ..
          "  setx ARCANE_SDK D:\\dev\\starworks\\Arcane\n" ..
          "then restart the terminal and re-generate.")
end
ARCANE_SDK = ARCANE_SDK:gsub("\\", "/")             -- normalize separators for premake tokens
local ARCANE_TP = ARCANE_SDK .. "/ThirdParty"       -- vendored header-only deps live inside the SDK repo

-- The engine's per-config bin flavor. Must byte-match the engine's own outputdir
-- literal in the SDK root premake5.lua ("-md" = the dynamic-CRT flavor; /MD everywhere so
-- one heap crosses the ArcaneClient.dll/Game.dll boundary).
local ARCANE_BIN = ARCANE_SDK .. "/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

-- Declare + fully configure a game module (the project's primary plugin).
-- Call AFTER declaring the workspace + configurations. Builds -> Binaries/<name>.dll.
function arcane_game_module(name)
    project(name)
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"                         -- /MD: share one CRT heap with ArcaneClient.dll
        targetname(name)
        -- Flat Binaries/ (config-agnostic, matching the manifest's gameModule name).
        -- Dev + the host run Debug; Binaries/ holds the config the host loads.
        targetdir "%{wks.location}/Binaries"
        objdir "%{wks.location}/Intermediate/%{cfg.buildcfg}"

        files { "%{wks.location}/Source/**.cpp", "%{wks.location}/Source/**.hpp" }

        -- Public engine header surface (in-place) + the header-only ThirdParty deps a
        -- game module pulls in transitively (glm/Astra scene types, imgui handoff,
        -- spdlog via Log.hpp, the Mosaic threading seam).
        includedirs {
            "%{wks.location}/Source",
            ARCANE_SDK .. "/ArcaneClient/src",
            -- Core's namespaced include root (<Arcane/Guid.hpp>, and since the
            -- Core-DLL split the whole headless engine layer: Base/Scene/
            -- Plugin/Project/Serialization/...) -- and its import lib: a game
            -- module links BOTH engine DLLs (spec docs/specs/
            -- 2026-09-15-core-dll-split-design.md s1.2); the host's own copies
            -- of both DLLs are what the loader binds.
            ARCANE_SDK .. "/ArcaneCore/src",
            ARCANE_TP .. "/glm",
            ARCANE_TP .. "/Astra/include",
            ARCANE_TP .. "/enkiTS/src",
            ARCANE_TP .. "/imgui",
            ARCANE_TP .. "/spdlog/include",
            ARCANE_TP .. "/Mosaic/include",
        }

        -- Link the engine import libs by name out of the per-config SDK bin dirs.
        -- Neither is a project in this workspace, so premake treats them as
        -- library links resolved against libdirs (-> ArcaneCore.lib /
        -- ArcaneClient.lib). imgui's exported surface arrives through
        -- ArcaneClient's import lib (/WHOLEARCHIVE in the engine).
        libdirs { ARCANE_BIN .. "/ArcaneCore", ARCANE_BIN .. "/ArcaneClient" }
        links   { "ArcaneCore", "ArcaneClient" }

        defines {
            "GAME_BUILD_DLL",                         -- kept for an external module's own GAME_API; ARCANE_GAME_MODULE needs no define
            "IMGUI_API=__declspec(dllimport)",        -- adopt ArcaneClient.dll's single GImGui
            "_CRT_SECURE_NO_WARNINGS",
            "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        }

        filter "system:windows"
            systemversion "latest"
            -- /arch:AVX2 matches the engine's x86 min-spec (Arcane::Simd) so inline
            -- header codegen shared across the DLL boundary agrees. /utf-8 for fmt/spdlog.
            buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        filter { "system:linux or system:macosx", "architecture:x86_64" }
            buildoptions { "-mavx2", "-mfma" }

        -- Per-config: runtime + NDEBUG must match ArcaneClient.dll's flavor (the vulkan.hpp
        -- dispatcher layout + inline header layouts are NDEBUG-conditional).
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
end

-- ============================================================================
-- Core-only consumers (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-
-- design.md s1.2 / s8, Plan 3). An external exe or static lib that needs the
-- headless engine -- Cli, Guid, Base/Log, Project, ... -- and NOTHING from the
-- presentation DLL links ArcaneCore.dll alone. Aphelyon's three services,
-- their Common lib and their test exes are the consumers; the recipe is lifted
-- from the engine's own ArcaneServer block in premake5.lua (the proven
-- Core-only host).
--
-- arcane_core_consumer() configures the CURRENT project: call it INSIDE a
-- `project` block, in place of `staticruntime`. It composes into whatever
-- shape the project has (ConsoleApp, StaticLib, a factory function) rather
-- than declaring one, because Core consumers do not share a shape the way
-- game modules do. It adds:
--   * staticruntime "off" -- /MD, one CRT heap across the DLL boundary (the
--     whole reason a consumer links a DLL instead of compiling Core from
--     source: objects allocated in ArcaneCore.dll are freed by the caller);
--   * the Core include root + the SDK-PRIVATE header-only deps a Core header
--     closure can reach (glm, Astra, enkiTS, Manifold2D, Mosaic). spdlog and
--     nlohmann are deliberately NOT added: a consumer vendors its own copies
--     (Aphelyon does, at the engine's versions), and two copies of a
--     header-only library on one include path is a version split waiting to
--     happen -- a consumer without them gets a clear missing-header error;
--   * the import lib + libdir;
--   * the engine's flavor contract, the same lines arcane_game_module
--     carries: /utf-8, /arch:AVX2 (ArcaneCore.dll is built AVX2 workspace-wide,
--     so the process already requires it -- matching keeps inline header
--     codegen identical across the boundary), and per-config runtime +
--     ARCANE_DEBUG / ARCANE_RELEASE+NDEBUG / ARCANE_DIST+NDEBUG so inline
--     header layouts under #ifndef NDEBUG agree with the DLL's.
-- It ends with `filter {}` so the caller's following lines are unfiltered.
--
-- arcane_core_stage_dll() adds the postbuild copy of ArcaneCore.dll beside an
-- exe's output (the dev bin layout; ArcaneRuntime's own postbuild is the
-- template). StaticLib consumers do not call it.
-- ============================================================================
function arcane_core_consumer()
    staticruntime "off"

    includedirs {
        ARCANE_SDK .. "/ArcaneCore/src",
        ARCANE_TP .. "/glm",
        ARCANE_TP .. "/Astra/include",
        ARCANE_TP .. "/enkiTS/src",
        ARCANE_TP .. "/Manifold2D/include",
        ARCANE_TP .. "/Mosaic/include",
    }

    libdirs { ARCANE_BIN .. "/ArcaneCore" }
    links   { "ArcaneCore" }

    filter "system:windows"
        buildoptions { "/utf-8", "/arch:AVX2" }
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
    filter "configurations:Release"
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
    filter {}
end

function arcane_core_stage_dll()
    postbuildcommands {
        '{COPYFILE} "' .. ARCANE_BIN .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"',
    }
end
