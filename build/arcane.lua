-- ============================================================================
-- arcane.lua -- the Arcane SDK premake module (engine-as-SDK, Slice 5).
--
-- An EXTERNAL project (one built OUTSIDE the Arcane.slnx workspace) consumes the
-- engine through this file. The "SDK" is the engine's own build output consumed
-- IN PLACE via the ARCANE_SDK env var (a packaged/installed SDK with a
-- multi-version registry is a later nicety -- spec 2026-07-22 S10/S12):
--
--   ARCANE_SDK  ->  the Arcane engine workspace dir (e.g. D:\dev\starworks\Gacha\Arcane)
--                   include surface = $ARCANE_SDK/Arcane/src (+ ThirdParty header-only)
--                   import lib       = $ARCANE_SDK/bin/<cfg>-<sys>-x86_64-md/Arcane/Arcane.lib
--                   Arcane.dll        ships beside the host exe (host copies it)
--
-- Usage from a project's premake5.lua:
--   workspace "MyGame"
--       architecture "x64"
--       configurations { "Debug", "Release", "Dist" }
--   include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
--   arcane_game_module("MyGame")   -- declares the SharedLib game module (-> Binaries/MyGame.dll)
--
-- The module implements the extern-C plugin ABI (Arcane/Plugin/PluginABI.hpp,
-- currently v5); the host's ABI gate refuses a cross-build mismatch. The include
-- + link + define set below MIRRORS the in-tree PlaygroundGame module (the proven
-- minimal game-module recipe) so an external build matches an in-tree one.
-- ============================================================================

ARCANE_SDK = os.getenv("ARCANE_SDK")
if not ARCANE_SDK then
    error("ARCANE_SDK environment variable is not set.\n" ..
          "Point it at the Arcane engine workspace dir, e.g.:\n" ..
          "  setx ARCANE_SDK D:\\dev\\starworks\\Gacha\\Arcane\n" ..
          "then restart the terminal and re-generate.")
end
ARCANE_SDK = ARCANE_SDK:gsub("\\", "/")             -- normalize separators for premake tokens
local ARCANE_TP = ARCANE_SDK .. "/../ThirdParty"    -- vendored header-only deps live beside the workspace

-- The engine's per-config bin flavor. Must byte-match the engine's own outputdir
-- literal in Arcane/premake5.lua ("-md" = the dynamic-CRT flavor; /MD everywhere so
-- one heap crosses the Arcane.dll/Game.dll boundary).
local ARCANE_BIN = ARCANE_SDK .. "/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

-- Declare + fully configure a game module (the project's primary plugin).
-- Call AFTER declaring the workspace + configurations. Builds -> Binaries/<name>.dll.
function arcane_game_module(name)
    project(name)
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"                         -- /MD: share one CRT heap with Arcane.dll
        targetname(name)
        -- Flat Binaries/ (config-agnostic, matching the manifest's gameModule name).
        -- Dev + the host run Debug; Binaries/ holds the config the host loads.
        targetdir "%{wks.location}/Binaries"
        objdir "%{wks.location}/Intermediate/%{cfg.buildcfg}"

        files { "%{wks.location}/Source/**.cpp", "%{wks.location}/Source/**.hpp" }

        -- Public engine header surface (in-place) + the header-only ThirdParty deps a
        -- game module pulls in transitively (glm/Astra scene types, imgui handoff,
        -- spdlog via Log.hpp, nvrhi via the render context, the Mosaic threading seam).
        includedirs {
            "%{wks.location}/Source",
            ARCANE_SDK .. "/Arcane/src",
            ARCANE_TP .. "/glm",
            ARCANE_TP .. "/nvrhi/include",
            ARCANE_TP .. "/Astra/include",
            ARCANE_TP .. "/enkiTS/src",
            ARCANE_TP .. "/imgui",
            ARCANE_TP .. "/spdlog/include",
            ARCANE_TP .. "/Mosaic/include",
        }

        -- Link the engine import lib by name out of the per-config SDK bin dir.
        -- "Arcane" is not a project in this workspace, so premake treats it as a
        -- library link resolved against libdirs (-> Arcane.lib). imgui's exported
        -- surface arrives through this same import lib (/WHOLEARCHIVE in the engine).
        libdirs { ARCANE_BIN .. "/Arcane" }
        links   { "Arcane" }

        defines {
            "GAME_BUILD_DLL",                         -- GAME_API -> dllexport (GameApi.hpp)
            "IMGUI_API=__declspec(dllimport)",        -- adopt Arcane.dll's single GImGui
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

        -- Per-config: runtime + NDEBUG must match Arcane.dll's flavor (the vulkan.hpp
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
