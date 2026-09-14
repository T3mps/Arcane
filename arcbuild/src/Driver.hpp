#pragma once

// arcbuild -- the PURE core of the game-project build driver (spec
// docs/specs/2026-09-13-arcbuild-driver-design.md). Umbrella include for the
// test exe ([build], ArcaneTests/src/BuildDriverTest.cpp) and for main.cpp.
//
// Split so `--engine` (spec §6) can land as a second target kind without
// rewriting the game-project CLI or stuffing engine ordering into the
// game-module CRT table:
//
//   Exit.hpp      shared exit codes
//   Request.hpp   Command, flags, ValidateRequest, ResolveSdk  (grows --engine)
//   Slot.hpp      s4.3 Binaries/<gameModule> CRT rule          (game only)
//   Compose.hpp   premake/msbuild lines + game Layout          (spawn lines shared)
//
// main.cpp is the thin shell: manifest, SDK, tool resolution, the slot probe,
// spawn-and-stream. NOT a second PE scanner: CrtFlavor comes from
// Arcane::Module::ScanFileCrtFlavor, the same verdict PluginHost uses.

#include "Compose.hpp"
#include "Exit.hpp"
#include "Request.hpp"
#include "Slot.hpp"
