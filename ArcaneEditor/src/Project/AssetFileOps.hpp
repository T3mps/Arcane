#pragma once

// AssetFileOps (spec 2026-09-30 s7.3): every asset file operation -- Rename,
// Duplicate, Delete, Move, New Folder -- is a filesystem TRANSACTION, planned
// purely, executed all-or-nothing, pushed as ONE undoable step. PlanAssetOp is
// PURE: it reads only AssetOpFacts (EditorApp builds them; tests fake them) and
// never touches the disk or the app.

#include "Panels/AssetPanelModel.hpp"   // AssetKind, AssetKindOf

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    class AssetReferenceIndex;

    enum class AssetOpKind : std::uint8_t { Rename, Duplicate, Delete, Move, NewFolder };

    struct AssetOpRequest
    {
        AssetOpKind               kind = AssetOpKind::Rename;
        std::vector<Arcane::Guid> guids;           // Rename: exactly 1; NewFolder: empty
        std::string               newStem;         // Rename; NewFolder: the folder name
        std::string               destFolder;      // Move/NewFolder: relative to Content/, "" = root
        bool                      cascadeDerived = true;   // Delete (s7.5)
    };

    struct FileMove { std::filesystem::path from, to; };   // absolute; Delete: `to` empty
    struct AssetMove                                       // files[0] = the id-bearing file
    {
        Arcane::Guid          guid;
        AssetKind             kind = AssetKind::Other;
        std::vector<FileMove> files;
    };
    enum class RefSource : std::uint8_t { AssetOnDisk, OpenScene, UnsavedDocument, BootScene, InputActions };
    struct AssetReferencer { Arcane::Guid target, referencer; std::vector<RefSource> sources; std::string label; };
    struct DerivedChild { Arcane::Guid parent, child; bool cascades = true; std::vector<Arcane::Guid> referencers; };
    struct AssetRefusal { Arcane::Guid guid; std::string reason; };

    struct AssetOpPlan
    {
        AssetOpKind                  kind = AssetOpKind::Rename;
        std::vector<AssetMove>       moves;        // Rename/Move/Duplicate(copy)/Delete(doomed, incl. cascaded)
        std::vector<Arcane::Guid>    newGuids;     // Duplicate: minted here, parallel to moves
        std::vector<AssetRefusal>    refusals;     // non-empty => nothing runs
        std::vector<AssetReferencer> referencers;  // Delete only (s7.5)
        std::vector<DerivedChild>    derived;      // Delete only (s7.5)
        std::vector<Arcane::Guid>    openDocs, dirtyDocs;
        std::string                  label;        // the undo step's label (s7.4)
    };

    struct AssetOpFacts                            // built by EditorApp; faked by tests
    {
        std::filesystem::path contentDir;          // Project::Root()/"Content"
        std::filesystem::path diagDir;             // Project::Root()/"Saved"/"Diagnostics"
        std::span<const std::pair<Arcane::Guid, std::string>> registry;   // Registry().All()
        const AssetReferenceIndex* refs = nullptr; // m_assetModel.RefIndex()
        std::span<const Arcane::Guid> openSceneAssets;                    // s7.5
        Arcane::Guid openScene, bootScene, inputActions;
        struct Doc { Arcane::Guid guid; bool dirty = false; std::vector<Arcane::Guid> liveRefs; };
        std::span<const Doc> docs;
        std::function<bool(const std::filesystem::path&)> exists;
        std::function<std::optional<Arcane::Guid>(const std::filesystem::path&)> peekId;   // AssetRegistry::PeekId
        std::function<std::vector<std::string>(const std::filesystem::path&)> gltfUris;   // s7.8
        std::function<std::vector<std::filesystem::path>(const std::filesystem::path&)> diagSiblings;  // s7.5
    };

    // True when `from` and `to` differ only by letter case (one file on NTFS: the
    // destination counts as free, s7.6).
    [[nodiscard]] bool IsCaseOnlyRename(const std::filesystem::path& from, const std::filesystem::path& to);

    // s7.7's copy name: strip a trailing " <digits>", then the first "<base> N"
    // (N = 1, 2, ...) passing ValidateCreateNameSyntax whose file AND "<file>.meta"
    // are not `taken`. Empty when none is.
    [[nodiscard]] std::string NextCopyName(std::string_view stem, const std::filesystem::path& dir,
                                           std::string_view ext,
                                           const std::function<bool(const std::filesystem::path&)>& taken);

    [[nodiscard]] AssetOpPlan PlanAssetOp(const AssetOpRequest& op, const AssetOpFacts& facts);
}
