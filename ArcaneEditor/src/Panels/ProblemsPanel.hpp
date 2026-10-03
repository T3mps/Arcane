#pragma once

// The Problems panel: current diagnostic STATE, grouped by scope. Distinct from
// the Console, which is the append-only log stream -- see
// docs/superpowers/specs/2026-07-29-diagnostics-problems-console-design.md.
// Draw returns the locator of a clicked row so the HOST performs the navigation
// (opening documents / changing selection mid-draw is what the modal deferral
// rules elsewhere in this editor exist to prevent).
// Rows route only when `ClassifyLocator` says so; a non-routable row is plain text.

#include <Panels/DiagnosticStore.hpp>
#include <Panels/LocatorRoute.hpp>

#include <optional>

namespace Arcane::Editor
{
    struct ProblemsUiState
    {
        bool showError = true, showWarning = true, showInfo = true;
        char search[128] = {};
    };

    [[nodiscard]] const char* ScopeLabel(Arcane::DiagScope scope) noexcept;

    // `open` is forwarded to ImGui::Begin (the tab's X button; null = no X).
    // `suppressBadges` (UnderVerifyHarness) keeps the tab title bare and untinted.
    [[nodiscard]] std::optional<Arcane::DiagLocator>
    DrawProblemsPanel(const DiagnosticStore& store, ProblemsUiState& ui, const RouteFacts& facts,
                      bool suppressBadges, bool* open = nullptr);
}
