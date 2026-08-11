#pragma once

// The editor's dockable-panel registry: ONE table drives the Window menu, the
// tab close buttons, ini persistence, and Reset Layout (spec: 2026-08-10
// editor-menu-wiring, Part I). Adding a panel = one enum value + one table
// row + gating its draw site; nothing else changes.
//
// Deliberately ImGui-free so ArcaneTests exercises it headless.

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace Arcane::Editor
{
    enum class PanelId : std::uint8_t
    {
        Viewport, Outliner, Inspector, Assets, Console, Problems,
        Count
    };

    struct PanelInfo
    {
        PanelId     id;
        const char* name;       // ImGui::Begin title AND menu label AND ini key
        bool        permanent;  // true = no X, cannot hide (Viewport only today)
    };

    // Window-menu order. Names must match each panel's ImGui::Begin title
    // exactly (PanelRegistryTest pins the invariants).
    inline constexpr PanelInfo kPanels[] = {
        { PanelId::Viewport,  "Viewport",  true  },
        { PanelId::Outliner,  "Outliner",  false },
        { PanelId::Inspector, "Inspector", false },
        { PanelId::Assets,    "Assets",    false },
        { PanelId::Console,   "Console",   false },
        { PanelId::Problems,  "Problems",  false },
    };
    static_assert(std::size(kPanels) == static_cast<std::size_t>(PanelId::Count));

    struct PanelVisibility
    {
        std::array<bool, static_cast<std::size_t>(PanelId::Count)> visible;

        PanelVisibility() { visible.fill(true); }

        // Permanent panels always report visible, whatever the array says --
        // one lie-proof read for the draw-site gates.
        [[nodiscard]] bool IsVisible(PanelId id) const
        {
            const auto i = static_cast<std::size_t>(id);
            return kPanels[i].permanent || visible[i];
        }

        // The p_open to hand ImGui::Begin: null for permanent panels (no X).
        [[nodiscard]] bool* OpenFlag(PanelId id)
        {
            const auto i = static_cast<std::size_t>(id);
            return kPanels[i].permanent ? nullptr : &visible[i];
        }
    };

    // One "[EditorPanels][Visibility]" ini line, e.g. "Console=0". Name-keyed
    // so reordering or adding panels never breaks an old ini. nullopt for a
    // malformed line, an unknown name, or a permanent panel (never persisted;
    // a hand-edited "Viewport=0" must not smuggle the viewport away).
    [[nodiscard]] inline std::optional<std::pair<PanelId, bool>>
    ParsePanelVisibilityLine(const char* line)
    {
        if (!line)
            return std::nullopt;
        const char* eq = std::strchr(line, '=');
        if (!eq || eq == line)
            return std::nullopt;
        if (eq[1] != '0' && eq[1] != '1')
            return std::nullopt;
        if (eq[2] != '\0')
            return std::nullopt;
        const std::size_t nameLen = static_cast<std::size_t>(eq - line);
        for (const PanelInfo& p : kPanels)
        {
            if (p.permanent)
                continue;
            if (std::strlen(p.name) == nameLen &&
                std::strncmp(p.name, line, nameLen) == 0)
                return std::make_pair(p.id, eq[1] == '1');
        }
        return std::nullopt;
    }
}
