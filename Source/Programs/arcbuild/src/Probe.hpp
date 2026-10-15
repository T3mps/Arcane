#pragma once

#include "ProjectLayout.hpp"
#include "Slot.hpp"

#include <Arcane/Plugin/Module.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace arcbuild
{
    struct SlotProbe
    {
        std::filesystem::path slot;
        Arcane::CrtFlavor     flavor = Arcane::CrtFlavor::Unknown;
        std::string           matched;
        SlotState             state = SlotState::Absent;
    };

    class ISlotInspector
    {
    public:
        virtual ~ISlotInspector() = default;

        [[nodiscard]]
        virtual SlotProbe Inspect(
            const ProjectLayout& project,
            std::string_view config) const = 0;

        [[nodiscard]]
        virtual std::string Describe(
            const SlotProbe& probe,
            std::string_view config,
            const Verdict& verdict) const = 0;
    };

    class SlotInspector final : public ISlotInspector
    {
    public:
        [[nodiscard]]
        SlotProbe Inspect(
            const ProjectLayout& project,
            std::string_view config) const override;

        [[nodiscard]]
        std::string Describe(
            const SlotProbe& probe,
            std::string_view config,
            const Verdict& verdict) const override;
    };
}
