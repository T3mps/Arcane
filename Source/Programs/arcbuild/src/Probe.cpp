#include "Probe.hpp"

namespace arcbuild
{
    namespace
    {
        const char* FlavorName(
            Arcane::CrtFlavor flavor)
        {
            switch (flavor)
            {
            case Arcane::CrtFlavor::Debug:
                return "debug";

            case Arcane::CrtFlavor::Release:
                return "release";

            case Arcane::CrtFlavor::Unknown:
                return "unknown";
            }

            return "?";
        }
    }

    SlotProbe SlotInspector::Inspect(
        const ProjectLayout& project,
        std::string_view config) const
    {
        SlotProbe probe;

        probe.slot =
            SlotPath(project);

        if (probe.slot.empty())
        {
            probe.state =
                SlotState::Absent;

            return probe;
        }

        std::error_code ec;

        const auto status =
            std::filesystem::status(
                probe.slot,
                ec);

        // status(path, ec) may set ENOENT as well as returning not_found.
        // A genuinely missing slot is safe for an incremental first build.
        if (status.type() == std::filesystem::file_type::not_found)
        {
            probe.state = SlotState::Absent;
            return probe;
        }

        if (ec)
        {
            probe.state =
                SlotState::Unreadable;

            return probe;
        }

        if (!std::filesystem::exists(status))
        {
            probe.state =
                SlotState::Absent;

            return probe;
        }

        if (!std::filesystem::is_regular_file(status))
        {
            probe.state =
                SlotState::Unreadable;

            return probe;
        }

        probe.flavor =
            Arcane::Module::ScanFileCrtFlavor(
                probe.slot,
                &probe.matched);

        probe.state =
            ClassifySlot(
                true,
                probe.flavor,
                config);

        return probe;
    }

    std::string SlotInspector::Describe(
        const SlotProbe& probe,
        std::string_view config,
        const Verdict& verdict) const
    {
        std::string row =
            "probe: slot=";

        row += probe.slot.empty()
            ? "(no gameModule)"
            : probe.slot.generic_string();

        row += " state=";
        row += SlotStateName(probe.state);

        row += " flavor=";
        row += FlavorName(probe.flavor);

        if (!probe.matched.empty())
        {
            row += " (";
            row += probe.matched;
            row += ')';
        }

        row += " config=";
        row += config;

        row += " -> ";
        row += verdict.rebuild
            ? "rebuild"
            : "incremental build";

        return row;
    }
}
