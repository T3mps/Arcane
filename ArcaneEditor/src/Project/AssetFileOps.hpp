#pragma once

// AssetFileOps (spec 2026-09-30 s7.3): every asset file operation -- Rename,
// Duplicate, Delete, Move, New Folder -- is a filesystem TRANSACTION, planned
// purely, executed all-or-nothing, pushed as ONE undoable step. PlanAssetOp is
// PURE: it reads only AssetOpFacts (EditorApp builds them; tests fake them) and
// never touches the disk or the app.

#include "Panels/AssetActivityLog.hpp"    // AssetActivityEntry (the host's Activity feed)
#include "Panels/AssetPanelModel.hpp"   // AssetKind, AssetKindOf

#include <Arcane/Edit/Command.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/AssetRegistry.hpp>   // RebindResult

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane { class CommandStack; }

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

    // s7.8: the relative files a .gltf names -- buffers[].uri then images[].uri,
    // data: URIs skipped, percent-decoded. Empty for .glb (self-contained) and for an
    // unreadable file. The real AssetOpFacts::gltfUris.
    [[nodiscard]] std::vector<std::string> ReadGltfUris(const std::filesystem::path& gltf);

    [[nodiscard]] AssetOpPlan PlanAssetOp(const AssetOpRequest& op, const AssetOpFacts& facts);

    // ---- execution (s7.3/s7.4) ---------------------------------------------

    // s7.1's gates. The UI disables items with this text; the executor re-checks.
    struct AssetOpGates { bool projectOpen = false; bool editMode = false; };
    [[nodiscard]] std::optional<std::string> AssetOpGateRefusal(const AssetOpGates& gates, bool inTransaction);

    // The executor reaches the app ONLY through this (EditorApp implements it over
    // Runtime/DocumentHost/the asset model; tests fake it). Recycle: T5-A13.
    struct AssetFileOpHost
    {
        virtual ~AssetFileOpHost() = default;
        virtual AssetOpGates                Gates() const = 0;
        virtual Arcane::RebindResult        Rebind(const Arcane::Guid&, const std::filesystem::path&) = 0;  // Runtime::RebindMovedAsset
        virtual bool                        Unregister(const Arcane::Guid&) = 0;                          // Runtime::UnregisterAsset
        virtual std::optional<Arcane::Guid> Register(const std::filesystem::path&) = 0;                   // Runtime::RegisterCreatedAsset
        virtual bool                        CloseDocumentFor(const Arcane::Guid&, bool discardDirty) = 0; // false = a dirty doc blocks
        virtual void NoteMoved(const Arcane::Guid&, const std::filesystem::path& from,
                               const std::filesystem::path& to) = 0;                                      // s7.11
        virtual void AssetsChanged(std::span<const Arcane::Guid> removed,
                                   std::span<const Arcane::Guid> added) = 0;                              // s7.12
        virtual void Invalidate(const Arcane::Guid&, AssetKind) = 0;                                      // s7.12
        virtual void EvictPaths(std::span<const std::filesystem::path>) = 0;                              // Assets::EvictPath
        virtual void Activity(AssetActivityEntry) = 0;
        virtual void ReportError(std::string title, std::string message) = 0;                             // ModalErrorQueue + ARC_ERROR
    };

    struct ExecResult { bool ok = false; std::string error; };   // !ok => nothing pushed

    class AssetFileOpExecutor
    {
    public:
        enum class Side : std::uint8_t { Forward, Backward };   // Forward = from -> to (do/redo)
        using RenameFn = std::function<std::error_code(const std::filesystem::path&, const std::filesystem::path&)>;

        AssetFileOpExecutor(AssetFileOpHost& host, Arcane::CommandStack& stack, std::filesystem::path contentDir);
        ~AssetFileOpExecutor();   // every pushed step goes inert (its anchor dies)
        AssetFileOpExecutor(const AssetFileOpExecutor&) = delete;
        AssetFileOpExecutor& operator=(const AssetFileOpExecutor&) = delete;

        // (1) refusals + gates, (2) the primitive with rollback, (3) follow-up,
        // (4) THEN push one step (the forward already happened, Command.hpp).
        // `stack` must be the constructor's stack.
        [[nodiscard]] ExecResult Execute(const AssetOpPlan& plan, Arcane::CommandStack& stack);
        [[nodiscard]] std::weak_ptr<AssetFileOpExecutor*> Anchor() const { return m_anchor; }

        // ---- primitives the commands replay (one code path per disk effect) ----
        // nullopt = applied; a string = refused (nothing touched) or failed (rolled back).
        [[nodiscard]] std::optional<std::string> PreflightMove(std::span<const AssetMove> moves, Side side) const;
        // `dirs` (in/out, optional): on entry the folders the OTHER side created. A
        // success removes those still empty (deepest first) and hands back the folders
        // THIS apply created; a failure removes its own and leaves `dirs` untouched.
        // A folder that existed before, or that gained anything since, is never removed.
        [[nodiscard]] std::optional<std::string> ApplyMove(std::span<const AssetMove> moves, Side side,
                                                           std::vector<std::filesystem::path>* dirs = nullptr);
        // s7.4 expiry: true when an asset's id-bearing file on `side`'s source end no
        // longer holds its guid (deleted, or re-identified outside the editor). One
        // PeekId per asset: a stat plus the .meta/JSON header, never the binary.
        [[nodiscard]] bool MoveSourceLost(std::span<const AssetMove> moves, Side side) const;
        void ReportRefusal(std::string title, std::string message) { m_host.ReportError(std::move(title), std::move(message)); }
        [[nodiscard]] std::string Display(const std::filesystem::path& p) const;   // "textures/uv.png"
        void SetRenameForTest(RenameFn fn) { m_rename = std::move(fn); }

    private:
        [[nodiscard]] std::optional<std::string> RollBack(std::span<const FileMove> done);

        AssetFileOpHost&                     m_host;
        Arcane::CommandStack&                m_stack;
        std::filesystem::path                m_contentDir;
        RenameFn                             m_rename;
        std::shared_ptr<AssetFileOpExecutor*> m_anchor;
    };

    // s7.4: one batch = one command = one step. Inert when the executor dies;
    // a refused side BLOCKS the step (it then reads expired and both sides skip).
    class AssetFileCommand : public Arcane::ICommand
    {
    public:
        void Undo() final { Step(true); }
        void Redo() final { Step(false); }
        const char* Label() const final { return m_label.c_str(); }
        bool AffectsScene() const final { return false; }   // s3.3: file steps never dirty the scene
        bool IsExpired() const override
        {
            const AssetFileOpExecutor* exec = Exec();
            return m_blocked || !exec || SourceLost(*exec, /*undo side next*/ m_applied);
        }

    protected:
        AssetFileCommand(std::weak_ptr<AssetFileOpExecutor*> exec, std::string label)
            : m_exec(std::move(exec)), m_label(std::move(label)) {}
        // Run the side about to happen (pre-check, primitive with rollback, follow-up).
        virtual std::optional<std::string> Run(AssetFileOpExecutor& exec, bool undo) = 0;
        // True when the side about to run (undo if m_applied) has nothing left to act
        // on. An OCCUPIED destination is not expiry (it may be temporary; Run refuses it).
        virtual bool SourceLost(const AssetFileOpExecutor& exec, bool undo) const = 0;
        [[nodiscard]] AssetFileOpExecutor* Exec() const { const auto p = m_exec.lock(); return p ? *p : nullptr; }

        std::weak_ptr<AssetFileOpExecutor*> m_exec;
        std::string m_label;
        bool m_applied = true;    // the forward op ran before the push
        bool m_blocked = false;

    private:
        void Step(bool undo);
    };

    class AssetMoveCommand final : public AssetFileCommand   // Rename, Move
    {
    public:
        // `createdDirs`: the folders the forward apply created (the undo removes them).
        AssetMoveCommand(std::weak_ptr<AssetFileOpExecutor*> exec, std::string label, std::vector<AssetMove> moves,
                         std::vector<std::filesystem::path> createdDirs = {})
            : AssetFileCommand(std::move(exec), std::move(label)), m_moves(std::move(moves)), m_dirs(std::move(createdDirs)) {}
    protected:
        std::optional<std::string> Run(AssetFileOpExecutor& exec, bool undo) override
        {
            return exec.ApplyMove(m_moves, undo ? AssetFileOpExecutor::Side::Backward : AssetFileOpExecutor::Side::Forward,
                                  &m_dirs);
        }
        bool SourceLost(const AssetFileOpExecutor& exec, bool undo) const override
        {
            return exec.MoveSourceLost(m_moves, undo ? AssetFileOpExecutor::Side::Backward : AssetFileOpExecutor::Side::Forward);
        }
    private:
        std::vector<AssetMove> m_moves;
        std::vector<std::filesystem::path> m_dirs;   // the folders the side that ran last created
    };
}
