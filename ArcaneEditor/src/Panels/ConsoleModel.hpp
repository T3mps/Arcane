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
        // Monotonic per-buffer identity, stamped by ConsoleBuffer::Push (0 =
        // never pushed). The Console panel keys row SELECTION on it: deque
        // positions shift on every ring eviction, but a seq follows its line
        // for the line's whole life -- and it is never reused, not even
        // across Clear, so a stale selected id can never alias a new line.
        std::uint64_t        seq = 0;
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

    // Wall-clock hh:mm:ss from epoch millis. LOCAL time: this is a human
    // reading their own session, not a log correlated across machines. Shared
    // by the panel's row prefix and FormatConsoleRow below, so what is drawn
    // and what lands on the clipboard cannot drift.
    [[nodiscard]] std::string ClockText(std::uint64_t timestampMs);

    // The plain-text form of one rendered row -- what Copy and the selection's
    // Ctrl+C put on the clipboard, mirroring the drawn row:
    // "HH:MM:SS  Category   message" plus "  (xN)" when a collapsed row's
    // count > 1. Category is padded to the panel's own 8-column minimum. No
    // trailing newline; the caller joins rows.
    [[nodiscard]] std::string FormatConsoleRow(const ConsoleEntry& e, std::size_t count = 1);
}
