#pragma once

// The GAME-PROJECT incremental rule (spec s4.3): classify the single-slot
// Binaries/<gameModule> CRT flavor and decide plain build vs /t:Rebuild.
//
// This is the reason arcbuild exists for game modules. `--engine` (spec §6)
// does not use it -- the engine workspace has no gameModule slot; its hazard
// is ReferenceProject-before-Arcane ordering and staging, which lands in its
// own unit when that target kind is built. Do not generalise ClassifySlot to
// cover Arcane.slnx.

#include "Exit.hpp"
#include "Request.hpp"

#include <Arcane/Plugin/Module.hpp>   // Arcane::CrtFlavor (enum only; no link)

#include <cstdint>
#include <string_view>

namespace arcbuild
{
    // Which CRT family a configuration's DLL links: Debug <=> the debug CRT
    // (ucrtbased & co); Release AND Dist are both release-CRT, so Dist maps
    // onto Release for the probe -- the same caveat the editor's
    // ModuleBuild::Configuration() documents.
    [[nodiscard]] bool ConfigWantsDebugCrt(std::string_view config);

    // The row of the s4.3 table the slot lands on.
    enum class SlotState : std::uint8_t { Absent, Match, Mismatch, Unreadable };
    [[nodiscard]] const char* SlotStateName(SlotState s);
    [[nodiscard]] SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config);

    // The decision: plain build, or msbuild /t:Rebuild. `reason` is a static
    // string naming the row (printed as the driver's own log line).
    struct Verdict
    {
        bool        rebuild;
        const char* reason;
    };
    [[nodiscard]] Verdict Decide(Command command, bool forceRebuild, SlotState slot);

    // Slot-based (R4), not Verdict-based: --force-rebuild does not change
    // this. Probe is CI's nothing-stale check, not a dry-run of `build`.
    [[nodiscard]] int ProbeExitCode(SlotState s);
}
