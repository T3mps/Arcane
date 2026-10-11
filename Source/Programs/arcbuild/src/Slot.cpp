#include "Slot.hpp"

namespace arcbuild
{
    bool ConfigWantsDebugCrt(std::string_view config)
    {
        return config == "Debug";
    }

    const char* SlotStateName(SlotState state)
    {
        switch (state)
        {
            case SlotState::Absent:     return "absent";
            case SlotState::Match:      return "match";
            case SlotState::Mismatch:   return "mismatch";
            case SlotState::Unreadable: return "unreadable";
        }

        return "?";
    }

    SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config)
    {
        if (!exists)
            return SlotState::Absent;

        if (flavor == Arcane::CrtFlavor::Unknown)
            return SlotState::Unreadable;

        const bool slotIsDebug = flavor == Arcane::CrtFlavor::Debug;

        return slotIsDebug == ConfigWantsDebugCrt(config) ? SlotState::Match : SlotState::Mismatch;
    }

    Verdict Decide(SlotState slot)
    {
        switch (slot)
        {
            case SlotState::Absent: return { false, "slot absent: incremental build" };
            case SlotState::Match: return { false, "slot CRT flavor matches --config: incremental build" };
            case SlotState::Mismatch: return { true, "slot CRT flavor mismatches --config: full rebuild required (single-slot Binaries/ hazard)" };
            case SlotState::Unreadable: return { true, "slot CRT flavor unreadable: full rebuild required" };
        }

        return { true, "slot state unknown: full rebuild required" };
    }

    int ProbeExitCode(SlotState state)
    {
        return (state == SlotState::Absent || state == SlotState::Match) ? kExitOk : kExitProbeRebuild;
    }
}