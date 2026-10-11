#pragma once

#include "Exit.hpp"

#include <Arcane/Plugin/Module.hpp>

#include <cstdint>
#include <string_view>

namespace arcbuild
{
    [[nodiscard]] bool ConfigWantsDebugCrt(std::string_view config);

    enum class SlotState : std::uint8_t
    {
        Absent,
        Match,
        Mismatch,
        Unreadable
    };

    [[nodiscard]] const char* SlotStateName(SlotState state);
    [[nodiscard]] SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config);

    struct Verdict
    {
        bool        rebuild;
        const char* reason;
    };

    [[nodiscard]] Verdict Decide(SlotState slot);
    [[nodiscard]] int ProbeExitCode(SlotState state);
}