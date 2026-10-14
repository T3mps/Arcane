#pragma once

// Inspector filters (spec 2026-09-29 s2/s3): the fixed CATALOG of source
// kinds an Inspector instance can filter on, and the per-instance filter
// itself. The filter stores the UNTICKED kinds, so a kind added to the
// catalog later is admitted by every existing filter (decision 8.3: the tool
// stays honest -- a filter never silently narrows when the catalog grows).
// Pure: no ImGui (InspectorHost and the tests use it). IconsLucide.h is
// glyph macros only.

#include "Widgets/IconsLucide.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    struct InspectorKind
    {
        std::string_view id;            // InspectorSource::Kind(), persisted in imgui.ini
        std::string_view displayName;   // the dropdown row and the label
        std::string_view singular;      // one of them: the empty state's "No <singular> document open"
        const char* icon;               // the dropdown face + row glyph: the Asset Browser's KindIcon for the same thing
    };

    // Catalog order = dropdown order = Sanitized() order = the Filters= write order.
    inline constexpr std::array<InspectorKind, 6> kInspectorKinds{ {
        { "scene",         "Scene",         "Scene",         ICON_LC_CLAPPERBOARD },
        { "assets",        "Assets",        "Asset",         ICON_LC_PACKAGE },
        { "input-actions", "Input Actions", "Input Actions", ICON_LC_GAMEPAD_2 },
        { "material",      "Materials",     "Material",      ICON_LC_PALETTE },     // ShaderEditorDocument
        { "sprite",        "Sprites",       "Sprite",        ICON_LC_STICKER },
        { "mesh",          "Meshes",        "Mesh",          ICON_LC_BOX },
    } };

    [[nodiscard]] const InspectorKind* FindInspectorKind(std::string_view id);

    struct InspectorFilter
    {
        std::vector<std::string> excluded;   // catalog ids; empty = All

        [[nodiscard]] bool IsAll() const noexcept { return excluded.empty(); }
        // An empty kind (a document that never selects) is admitted only by All.
        [[nodiscard]] bool Admits(std::string_view kind) const;
        // Catalog kinds only, catalog order, no duplicates; an all-excluded
        // set becomes All (an instance that can show nothing reads as broken).
        [[nodiscard]] InspectorFilter Sanitized() const;
        [[nodiscard]] bool ExcludesEveryKind() const;   // true = would be refused

        static InspectorFilter AllBut(std::string_view kind);
        static InspectorFilter Only(std::string_view kind);

        friend bool operator==(const InspectorFilter&, const InspectorFilter&) = default;
    };

    // "All" / "<name>" (one ticked) / "All but <name>" (one unticked) /
    // "<name>, <name>" (the ticked names, catalog order).
    [[nodiscard]] std::string InspectorFilterLabel(const InspectorFilter& filter);

    // The dropdown FACE (user request 2026-09-30): All is ONE glyph (never
    // every kind's icon); otherwise the ticked kinds' icons in catalog order,
    // at most maxIcons of them (floor 1: never an empty face), the rest
    // counted in `overflow` (drawn as "+N"). The label is the face's tooltip.
    inline constexpr const char* kInspectorAllIcon = ICON_LC_LAYERS;

    struct FilterFace
    {
        std::vector<const char*> icons;
        int overflow = 0;
    };
    [[nodiscard]] FilterFace InspectorFilterFace(const InspectorFilter& filter, int maxIcons);
}
