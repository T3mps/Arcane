#pragma once

// Console model: the pure half of the Console panel. One log record as the
// panel needs it, the category derivation, and identical-row collapsing.
// No ImGui -- EditorPanels.cpp draws these.

#include <Arcane/Base/Diagnostics.hpp>   // DiagSeverity, shared with Problems

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct ConsoleEntry
    {
        Arcane::DiagSeverity level       = Arcane::DiagSeverity::Info;
        std::uint64_t        timestampMs = 0;
        std::string          category;    // see CategoryForMessage
        std::string          message;
        std::string          file;        // spdlog source_loc, may be empty
        int                  line = 0;
    };

    // Derive a category from the engine's already-consistent "Subsystem: "
    // message prefixes. Deliberately stringly-typed: it is zero engine churn and
    // the prefixes are de facto stable. Anything that genuinely matters gets a
    // real Arcane::DiagScope through the diagnostic seam instead of this table.
    // Returns "General" for anything unrecognized.
    [[nodiscard]] std::string_view CategoryForMessage(std::string_view message) noexcept;

    // One rendered row: the first entry of a run of identical entries, plus how
    // many there were. `first` points INTO the span passed to CollapseConsole and
    // is valid only as long as that span is.
    struct CollapsedRow
    {
        const ConsoleEntry* first = nullptr;
        std::size_t         count = 0;
    };

    // Fold entries with an identical (level, category, message) into one row,
    // preserving first-seen order. Nothing is hidden -- the count carries the
    // rest. Computed at draw time over the current buffer, so storage is untouched.
    [[nodiscard]] std::vector<CollapsedRow> CollapseConsole(std::span<const ConsoleEntry> entries);
}
