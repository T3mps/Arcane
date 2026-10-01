#pragma once

// The shared asset-reference field (spec 2026-09-30 s4.2): ONE cell for every
// Guid that names an asset -- entity components now, the sprite, mesh and
// material pages in T3. MODEL-AWARE, so it lives in Panels/; Widgets/ stays
// model-free (EditorWidgets.hpp:9-13). This header carries the services, the
// arguments, the result and the PURE halves the cell is built from.

#include "Panels/AssetPanelModel.hpp"   // AssetKind, AssetDragPayload, AssetPanelEntry, AssetPanelModel

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    // EditorApp owns ONE (m_assetRefServices) and hands a pointer to the
    // Inspector and to every document's Services. Every callable reads app
    // state at CALL time, so neither a project switch nor a document created
    // during a boot stage can leave a stale copy. A default-constructed value
    // is the NULL services (headless tests): the kind glyph, the raw guid,
    // "No project open", no browse-to / open / mint. Set drops still work.
    struct AssetRefServices
    {
        const AssetPanelModel* model = nullptr;                                          // EditorApp::m_assetModel
        std::function<const Arcane::Project*()> project;                                 // read at call time (project switch)
        std::function<std::uint64_t(const Arcane::Guid&)> resolveThumb;                  // AssetServices::resolveAssetThumb
        std::function<bool()> canReveal;                                                 // Asset Browser visible
        std::function<void(const Arcane::Guid&)> reveal;                                 // QUEUES a reveal (next frame)
        std::function<void(const Arcane::Guid&)> open;                                   // QUEUES an open (next frame)
        std::function<Arcane::Guid(const Arcane::Guid&)> mintSpriteForTexture;
        std::function<std::optional<std::string>(const Arcane::Guid&)> tombstoneName;   // filled in T5 (s7.12)
    };

    struct AssetRefArgs
    {
        Arcane::Guid guid;
        int  kindFilter = -1;           // AssetKind, -1 = any
        int  surfaceFilter = -1;        // MaterialSurface, -1 = any
        bool readOnly = false;
        bool mixed = false;             // multi-selection disagrees: "--"
        bool allowTextureMint = false;  // a Texture DROPPED on a Sprite field mints the wrapping .arcsprite
        bool identityGuid = false;      // an Identity guid, not a reference
        bool ownTooltip = true;         // false: the caller composes the tooltip from the result
    };

    struct AssetRefEdit
    {
        enum class Op : std::uint8_t { None, Set, Clear } op = Op::None;
        Arcane::Guid guid;              // Op::Set: the new target
        bool hovered = false;           // the name part, ForTooltip
        bool truncated = false;
        std::string fullText;           // untruncated display / its mount path
    };

    // What the cell shows for `args` (spec s4.2's table). Pure.
    struct AssetRefDisplay { std::string text, tooltip; bool dangling = false; bool browsable = false; AssetKind kind = AssetKind::Other; };
    [[nodiscard]] AssetRefDisplay DescribeAssetRef(const AssetRefArgs& args, const AssetRefServices& services);

    // Whether a browser drag may land: kind-only (no surface check on drop). Pure.
    enum class AssetRefDropVerdict : std::uint8_t { Refuse, Set, MintSprite };
    [[nodiscard]] AssetRefDropVerdict DecideAssetRefDrop(const AssetDragPayload& payload, const AssetRefArgs& args);

    // The picker's rows: kind-filtered; a material is excluded ONLY on a
    // CONFIRMED differing surface (an unknown surface stays, pill-less);
    // search on name and mount path, case-insensitive (MatchesFilter); sorted
    // by name, then mount path. Reads model.Entries() -- never a
    // BuildAssetEntries per popup frame. Pure.
    [[nodiscard]] std::vector<const AssetPanelEntry*> AssetRefCandidates(const AssetPanelModel& model, int kindFilter,
                                                                         int surfaceFilter, std::string_view search);

    class PropertyGrid;

    // THE CELL, left to right: a 20 px thumb (resolveThumb, else the kind
    // glyph); the ellipsized name (an AllowDoubleClick Selectable: a
    // double-click QUEUES open() when browsable, a single click does
    // nothing); the picker chevron (hidden when read-only); browse-to
    // (browsable only; disabled while the Asset Browser is closed); clear
    // (!readOnly && (guid valid || mixed)). The whole cell is a drop target
    // for kAssetDragType unless read-only. Returns Set/Clear for the CALLER
    // to route through its undo. ownTooltip draws the tooltip; otherwise
    // hovered / truncated / fullText are the caller's to compose.
    [[nodiscard]] AssetRefEdit AssetReferenceValue(const char* id, const AssetRefArgs& args,
                                                   const AssetRefServices& services);
    // FieldLabelCell + PushID(label) + AssetReferenceValue("##value") + grid.ProbeItem(label).
    [[nodiscard]] AssetRefEdit AssetRefRow(PropertyGrid& grid, const char* label, const AssetRefArgs& args,
                                           const AssetRefServices& services);
}
