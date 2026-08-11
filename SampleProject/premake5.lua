-- ============================================================================
-- SampleProject -- the in-repo sample game, built EXACTLY like an external
-- project (Aphelyon): a workspace that consumes the engine SDK's arcane.lua
-- to declare its game module -> Binaries/SampleGame.dll (what the manifest's
-- gameModule names). The editor's Build -> Rebuild Game Module drives this
-- same file; manually:
--
--   ..\..\ThirdParty\premake5\premake5.exe vs2026     (from this dir)
--   msbuild SampleProject.slnx /p:Configuration=Debug
--
-- ARCANE_SDK is self-located relatively (this project lives INSIDE the engine
-- workspace), so a fresh clone builds with no env setup; setting the env var
-- still wins, matching the external-project contract.
--
-- Generated IDE files (SampleProject.slnx / *.vcxproj), Binaries/ and
-- Intermediate/ are throwaway/derived -- see the repo .gitignore.
-- ============================================================================

workspace "SampleProject"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
    startproject "SampleGame"

ARCANE_SDK = os.getenv("ARCANE_SDK") or path.getabsolute("..")
include(ARCANE_SDK .. "/build/arcane.lua")

-- Declares the game module -> Binaries/SampleGame.dll (matches
-- SampleProject.arcproj's gameModule).
arcane_game_module("SampleGame")
