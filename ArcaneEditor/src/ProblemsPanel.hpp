#pragma once

// The Problems panel: current diagnostic STATE, grouped by scope. Distinct from
// the Console, which is the append-only log stream -- see
// docs/superpowers/specs/2026-07-29-diagnostics-problems-console-design.md.
// Draw returns the locator of a clicked row so the HOST performs the navigation
// (opening documents / changing selection mid-draw is what the modal deferral
// rules elsewhere in this editor exist to prevent).

#include <DiagnosticStore.hpp>

#include <optional>

namespace Arcane::Editor
{
    struct ProblemsUiState
    {
        bool showWarnings = true;
        bool showInfo     = true;
        char search[128]  = {};
    };

    [[nodiscard]] const char* ScopeLabel(Arcane::DiagScope scope) noexcept;

    // `open` is forwarded to ImGui::Begin (the tab's X button; null = no X).
    [[nodiscard]] std::optional<Arcane::DiagLocator>
    DrawProblemsPanel(const DiagnosticStore& store, ProblemsUiState& ui, bool* open = nullptr);
}
