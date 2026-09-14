#include "Slot.hpp"

namespace arcbuild
{
    bool ConfigWantsDebugCrt(std::string_view config)
    {
        return config == "Debug";
    }

    const char* SlotStateName(SlotState s)
    {
        switch (s)
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
        const bool slotIsDebug = (flavor == Arcane::CrtFlavor::Debug);
        return slotIsDebug == ConfigWantsDebugCrt(config) ? SlotState::Match : SlotState::Mismatch;
    }

    Verdict Decide(Command command, bool forceRebuild, SlotState slot)
    {
        if (command == Command::Rebuild)
            return { true, "rebuild command: msbuild /t:Rebuild unconditionally" };
        if (forceRebuild)
            return { true, "--force-rebuild: msbuild /t:Rebuild, slot probe bypassed" };
        switch (slot)
        {
            case SlotState::Absent:
                return { false, "slot absent: plain build (nothing to be wrong about)" };
            case SlotState::Match:
                return { false, "slot CRT flavor matches --config: plain build (msbuild's incremental view is trustworthy)" };
            case SlotState::Mismatch:
                // THE SINGLE-SLOT HAZARD: Binaries\ holds one DLL for every
                // configuration while the object trees are per-config, so an
                // incremental build would compare this config's objects
                // against this config's link stamp, find both current, relink
                // nothing, and leave the OTHER config's DLL in place for the
                // host to refuse.
                return { true, "slot CRT flavor mismatches --config: msbuild /t:Rebuild (single-slot Binaries/ hazard)" };
            case SlotState::Unreadable:
                return { true, "slot CRT flavor unreadable: msbuild /t:Rebuild (unknown => the safe choice)" };
        }
        return { true, "slot state unknown: msbuild /t:Rebuild" };
    }

    int ProbeExitCode(SlotState s)
    {
        return (s == SlotState::Absent || s == SlotState::Match) ? kExitOk : kExitProbeRebuild;
    }
}
