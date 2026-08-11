#pragma once

// The editor's diagnostic state: key -> the CURRENT set published under it.
// See Arcane/Base/Diagnostics.hpp for the publication-group contract; this is
// the consumer side of it. Pure data with no ImGui dependency so the [diagnostics]
// units drive it headlessly -- ProblemsPanel.cpp owns all presentation.

#include <Arcane/Base/Diagnostics.hpp>

#include <cstddef>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // True when `d` passes BOTH the severity floor and the (case-insensitive)
    // search text. Empty search matches everything. Free function so the panel
    // and the store share one definition of "matches".
    [[nodiscard]] bool MatchesDiagnosticFilter(const Arcane::Diagnostic& d,
                                               Arcane::DiagSeverity minSeverity,
                                               std::string_view search) noexcept;

    class DiagnosticStore
    {
    public:
        DiagnosticStore() = default;
        ~DiagnosticStore();

        DiagnosticStore(const DiagnosticStore&)            = delete;
        DiagnosticStore& operator=(const DiagnosticStore&) = delete;

        // Replace `key`'s set. An empty span erases the key entirely.
        void Publish(std::string_view key, std::span<const Arcane::Diagnostic> diags);
        void Clear(std::string_view key);
        void ClearAll();

        [[nodiscard]] std::size_t Count(Arcane::DiagSeverity severity) const;
        // Flattened across keys, errors first, then warnings, then info. Stable
        // within a severity (key order, then publication order).
        [[nodiscard]] std::vector<Arcane::Diagnostic> Snapshot() const;
        [[nodiscard]] std::vector<Arcane::Diagnostic> Filtered(Arcane::DiagSeverity minSeverity,
                                                               std::string_view search) const;

        // Route Arcane::Diagnostics::Publish into THIS store. Uninstall is called
        // by the destructor too -- the sink slot outlives nothing, and a torn-down
        // store must never be dispatched into (mirrors EditorApp's grouped
        // ConsoleDiagnostics::sink erase-at-Shutdown rule). Uninstall clears the
        // seam's slot via ClearSinkIfCurrent (identity-checked), not SetSink(nullptr, nullptr):
        // the slot is process-wide last-writer-wins, so an unconditional clear
        // from a stale owner's teardown would silently disconnect whichever
        // OTHER store installed after this one.
        void InstallAsEngineSink();
        void UninstallEngineSink();

    private:
        static void SinkTrampoline(std::string_view key,
                                   std::span<const Arcane::Diagnostic> diags, void* user);

        mutable std::mutex                                        m_mutex;
        std::map<std::string, std::vector<Arcane::Diagnostic>>    m_byKey;
        bool                                                      m_installed = false;
    };
}
