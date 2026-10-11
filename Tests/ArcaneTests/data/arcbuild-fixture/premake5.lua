-- Fixture: an input-only premake project for arcbuild's multibackend
-- structural tests (Task 4, arcbuild multibackend hardening plan). It is
-- never built -- the [build] structural tests, the RED-reproduction script,
-- and later acceptance tasks (5, 7) only copy this directory to a temporary
-- root, generate real project files from it (gmake / ninja / --os=macosx
-- xcode4), and inspect the RESULT (file names, ninja target names, link-edge
-- counts). ARCANE_SDK is read from the environment, matching the
-- external-project contract every real consumer (Aphelyon) uses -- the
-- scripts that drive this fixture set ARCANE_SDK to a checked-out engine
-- root before invoking premake5 against the temporary copy.

workspace "Fixture"
    architecture "x64"
    configurations { "Debug", "Release", "Dist" }
    startproject "Fixture"

include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")

-- Declares the game module -> Binaries/Fixture.dll (matches Fixture.arcproj's
-- gameModule).
arcane_game_module("Fixture")
