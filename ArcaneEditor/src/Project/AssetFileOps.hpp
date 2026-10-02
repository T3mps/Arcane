#pragma once

// AssetFileOps (spec 2026-09-30 s7.3): every asset file operation -- Rename,
// Duplicate, Delete, Move, New Folder -- is a filesystem TRANSACTION, planned
// purely, executed all-or-nothing, pushed as ONE undoable step. PlanAssetOp is
// PURE: it reads only AssetOpFacts (EditorApp builds them; tests fake them) and
// never touches the disk or the app.

#include "Panels/AssetActivityLog.hpp"    // AssetActivityEntry (the host's Activity feed)
#include "Panels/AssetPanelModel.hpp"   // AssetKind, AssetKindOf
#include "Project/OsShell.hpp"         // RecycleResult

#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/UndoPayload.hpp>
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
    // One row per referencer (s7.5): every source it was found through. `label` = the
    // referencer's file name ("Project" for a manifest row); `unsavedIn` = the dirty
    // document's title (RefSource::UnsavedDocument).
    struct AssetReferencer { Arcane::Guid target, referencer; std::vector<RefSource> sources; std::string label; std::string unsavedIn; };
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
        struct Doc { Arcane::Guid guid; bool dirty = false; std::vector<Arcane::Guid> liveRefs; std::string title; };
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

    // s7.7: write `m.to` as a copy of `m.from` carrying `newId`, BEFORE Register
    // (AddFile never mints). Native JSON: a new "id"; material/sprite/mesh "name" =
    // the copy's stem; a mesh's importedSource is stripped (one companion per model);
    // a scene re-mints every Identity id. Imported binaries: copy_file plus a .meta
    // with every source field and the new guid. False (and `error`) on failure, with
    // nothing of the copy left behind.
    [[nodiscard]] bool WriteAssetCopy(const FileMove& m, AssetKind kind, const Arcane::Guid& newId, std::string* error);

    // s7.8: the relative files a .gltf names -- buffers[].uri then images[].uri,
    // data: URIs skipped, percent-decoded. Empty for .glb (self-contained) and for an
    // unreadable file. The real AssetOpFacts::gltfUris.
    [[nodiscard]] std::vector<std::string> ReadGltfUris(const std::filesystem::path& gltf);

    // s7.5's delete analysis (PlanAssetOp's Delete walks `doomed`). `doomed` = the
    // request (deduplicated) + the cascaded children when `cascadeDerived`: a plain
    // sprite DerivesFrom a doomed Texture, a companion .arcmesh DerivesFrom a doomed
    // Model; never instance materials or sliced sprites. `derived` lists those children
    // (cascading or not). `referencers` = the union: index inbound, one hop through
    // each DerivesFrom child, the live scene manifest (only when the saved file lacks
    // it), dirty documents' LiveReferences, the project manifest (boot scene, input
    // actions; referencer = nil guid). Doomed referencers drop; one row each.
    struct DeleteAnalysis
    {
        std::vector<Arcane::Guid>    doomed;
        std::vector<DerivedChild>    derived;
        std::vector<AssetReferencer> referencers;
    };
    [[nodiscard]] DeleteAnalysis AnalyzeDelete(std::span<const Arcane::Guid> requested, bool cascadeDerived,
                                               const AssetOpFacts& facts);
    // The row's in-memory/manifest tags, space-separated ("(open scene, unsaved)",
    // "(unsaved in <title>)", "(project: boot scene)", "(project: input actions)");
    // empty for an on-disk-only referencer.
    [[nodiscard]] std::string ReferencerTags(const AssetReferencer& r);
    // s7.5's diag set beyond the report: the envelope's existing siblingTxt/Dmp/
    // GpuDump plus any <stem>.log.txt / <stem>.symbolized.txt beside it. The real
    // AssetOpFacts::diagSiblings.
    [[nodiscard]] std::vector<std::filesystem::path> DiagSiblingFiles(const std::filesystem::path& report);

    // s7.5's delete-confirm wording, PURE (the modal draws it; tests read it).
    // The title counts the REQUESTED assets (cascaded children only add doomed
    // rows): one -> "Delete <file name>?" (its move, else the first doomed file,
    // else the guid), several -> "Delete N assets?". The confirm escalates
    // "Delete" -> "Delete anyway" (referencers) -> "Discard changes and delete"
    // (dirty documents, named in `unsaved`).
    struct DeleteModalText { std::string title, confirm, unsaved, footer; };
    [[nodiscard]] DeleteModalText DescribeDeleteModal(const AssetOpPlan& plan, std::span<const Arcane::Guid> requested,
                                                      std::span<const std::string> dirtyTitles);

    [[nodiscard]] AssetOpPlan PlanAssetOp(const AssetOpRequest& op, const AssetOpFacts& facts);

    // ---- execution (s7.3/s7.4) ---------------------------------------------

    // s7.1's gates. The UI disables items with this text; the executor re-checks.
    struct AssetOpGates { bool projectOpen = false; bool editMode = false; };
    [[nodiscard]] std::optional<std::string> AssetOpGateRefusal(const AssetOpGates& gates, bool inTransaction);

    // The executor reaches the app ONLY through this (EditorApp implements it over
    // Runtime/DocumentHost/the asset model; tests fake it).
    struct AssetFileOpHost
    {
        virtual ~AssetFileOpHost() = default;
        virtual AssetOpGates                Gates() const = 0;
        virtual Arcane::RebindResult        Rebind(const Arcane::Guid&, const std::filesystem::path&) = 0;  // Runtime::RebindMovedAsset
        virtual bool                        Unregister(const Arcane::Guid&) = 0;                          // Runtime::UnregisterAsset
        virtual std::optional<Arcane::Guid> Register(const std::filesystem::path&) = 0;                   // Runtime::RegisterCreatedAsset
        virtual OsShell::RecycleResult      Recycle(std::span<const std::filesystem::path>) = 0;          // OsShell::ShellRecycle
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

    // s7.12's per-operation follow-up: the guid invalidation, EvictPath, model/diagnostic
    // refresh and activity row that close every Rename/Move/Delete/Duplicate side. The
    // executor's primitives call it as their last step, so it is the only caller of
    // NoteMoved/Invalidate/EvictPaths/AssetsChanged/Activity for those four verbs. New
    // Folder has no follow-up (s7.12): its two primitives call AssetsChanged({}, {})
    // themselves so the model re-walks its empty folders.
    //  - Rename/Move (both sides): NoteMoved, evict every from AND to, a Moved row.
    //  - Delete forward/redo, Duplicate undo: invalidate the removed guids (plus each
    //    non-cascaded derived sprite in plan.derived), evict, AssetsChanged(removed),
    //    a Deleted row ("permanently; ..." when `recycled` lists a file as nuked).
    //  - Delete undo (RestoreAssets, so also a duplicate's redo): invalidate + evict the
    //    restored files, AssetsChanged(added), a Created row.
    //  - Duplicate forward: evict the copies, AssetsChanged(newGuids), a Created row.
    enum class AssetOpSide : std::uint8_t { Forward, Undo, Redo };
    void RunAssetOpFollowUp(AssetFileOpHost& host, const AssetOpPlan& plan, AssetOpSide side,
                            const std::filesystem::path& contentDir, const OsShell::RecycleResult* recycled = nullptr);

    struct ExecResult { bool ok = false; std::string error; };   // !ok => nothing pushed

    struct FilePayload
    {
        std::filesystem::path            path;
        std::uint64_t                    size = 0;
        std::filesystem::file_time_type  mtime{};
        Arcane::UndoPayload              bytes;     // T1's store: spills above editor.undo.spillThresholdKB
    };
    struct AssetFiles    { Arcane::Guid guid; std::vector<std::filesystem::path> files; };   // files[0] = id-bearing
    struct AssetPayloads { Arcane::Guid guid; std::vector<FilePayload> files; };             // parallel to AssetFiles

    class AssetFileOpExecutor
    {
    public:
        enum class Side : std::uint8_t { Forward, Backward };   // Forward = from -> to (do/redo)
        using RenameFn = std::function<std::error_code(const std::filesystem::path&, const std::filesystem::path&)>;
        using CaptureFn = std::function<std::optional<Arcane::UndoPayload>(const std::filesystem::path&)>;

        AssetFileOpExecutor(AssetFileOpHost& host, Arcane::CommandStack& stack, std::filesystem::path contentDir);
        ~AssetFileOpExecutor();   // every pushed step goes inert (its anchor dies)
        AssetFileOpExecutor(const AssetFileOpExecutor&) = delete;
        AssetFileOpExecutor& operator=(const AssetFileOpExecutor&) = delete;

        // (1) refusals + gates, (2) the primitive with rollback, (3) follow-up,
        // (4) THEN push one step (the forward already happened, Command.hpp).
        // `stack` must be the constructor's stack.
        [[nodiscard]] ExecResult Execute(const AssetOpPlan& plan, Arcane::CommandStack& stack);
        [[nodiscard]] std::weak_ptr<AssetFileOpExecutor*> Anchor() const { return m_anchor; }
        // s7.7 Duplicate's forward: write every copy with its plan.newGuids id, Register
        // it, then evict + announce. On failure everything this call made is undone and
        // `copies` is cleared. `copies` (out) is what AssetDuplicateCommand replays.
        [[nodiscard]] std::optional<std::string> CopyForward(const AssetOpPlan& plan, std::vector<AssetFiles>& copies);

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
        [[nodiscard]] std::optional<std::string> CreateFolder(const std::filesystem::path& dir);
        [[nodiscard]] std::optional<std::string> RemoveEmptyFolder(const std::filesystem::path& dir);
        [[nodiscard]] bool FolderLost(const std::filesystem::path& dir) const;   // gone, or no longer empty
        void ReportRefusal(std::string title, std::string message) { m_host.ReportError(std::move(title), std::move(message)); }
        [[nodiscard]] std::string Display(const std::filesystem::path& p) const;   // "textures/uv.png"
        void SetRenameForTest(RenameFn fn) { m_rename = std::move(fn); }
        // Remove: docs close (a dirty one blocks unless discardDirty), capture EVERY
        // file, ONE Recycle call, verify gone (a survivor fails + rewrites), Unregister.
        [[nodiscard]] std::optional<std::string> RemoveAssets(std::span<const AssetFiles> doomed,
                                                              std::vector<AssetPayloads>& out, bool discardDirty);
        // Restore: occupancy pre-check, write .meta -> primaries -> companions with
        // their mtimes, Register must return the recorded guid. Any failure (a write,
        // an mtime, the guid) removes the files AND the folders this restore made.
        [[nodiscard]] std::optional<std::string> RestoreAssets(std::span<const AssetPayloads> payloads);
        [[nodiscard]] bool FilesLost(std::span<const AssetFiles> assets) const;
        [[nodiscard]] bool PayloadsLost(std::span<const AssetPayloads> payloads) const;   // a spilled undo copy is gone
        [[nodiscard]] const OsShell::RecycleResult& LastRecycle() const { return m_lastRecycle; }
        void SetCaptureForTest(CaptureFn fn) { m_capture = std::move(fn); }

    private:
        [[nodiscard]] std::optional<std::string> RollBack(std::span<const FileMove> done);
        // `created` (optional, appended): the folders this write made, shallow before deep.
        [[nodiscard]] std::optional<std::string> WritePayload(const FilePayload& p,
                                                              std::vector<std::filesystem::path>* created = nullptr) const;

        AssetFileOpHost&                     m_host;
        Arcane::CommandStack&                m_stack;
        std::filesystem::path                m_contentDir;
        RenameFn                             m_rename;
        CaptureFn                            m_capture;
        OsShell::RecycleResult               m_lastRecycle{};
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

    class NewFolderCommand final : public AssetFileCommand
    {
    public:
        NewFolderCommand(std::weak_ptr<AssetFileOpExecutor*> exec, std::string label, std::filesystem::path dir)
            : AssetFileCommand(std::move(exec), std::move(label)), m_dir(std::move(dir)) {}
    protected:
        std::optional<std::string> Run(AssetFileOpExecutor& exec, bool undo) override
        { return undo ? exec.RemoveEmptyFolder(m_dir) : exec.CreateFolder(m_dir); }
        bool SourceLost(const AssetFileOpExecutor& exec, bool undo) const override
        { return undo && exec.FolderLost(m_dir); }   // redo onto an occupied path: Run refuses, not expiry
    private:
        std::filesystem::path m_dir;
    };

    inline std::size_t PayloadByteCount(std::span<const AssetPayloads> payloads)
    {
        std::size_t n = 0;
        for (const AssetPayloads& a : payloads) for (const FilePayload& f : a.files) n += static_cast<std::size_t>(f.size);
        return n;
    }

    class AssetDeleteCommand final : public AssetFileCommand
    {
    public:
        AssetDeleteCommand(std::weak_ptr<AssetFileOpExecutor*> exec, std::string label,
                           std::vector<AssetFiles> assets, std::vector<AssetPayloads> payloads)
            : AssetFileCommand(std::move(exec), std::move(label)), m_assets(std::move(assets)), m_payloads(std::move(payloads)) {}
        std::size_t PayloadBytes() const override { return PayloadByteCount(m_payloads); }
    protected:
        std::optional<std::string> Run(AssetFileOpExecutor& exec, bool undo) override
        { return undo ? exec.RestoreAssets(m_payloads) : exec.RemoveAssets(m_assets, m_payloads, /*discardDirty*/ false); }
        // s7.4 expiry: applied (undo next) -- a spilled undo copy is gone (one stat via
        // UndoPayload::SpillPath, never a read); unapplied (redo next) -- restored files gone or re-identified.
        bool SourceLost(const AssetFileOpExecutor& exec, bool undo) const override
        { return undo ? exec.PayloadsLost(m_payloads) : exec.FilesLost(m_assets); }
    private:
        std::vector<AssetFiles>    m_assets;
        std::vector<AssetPayloads> m_payloads;
    };

    // s7.7's step: constructed APPLIED (the copies exist, registered under the plan's
    // newGuids). Undo = Remove on the copies; Redo = Restore (the same new guid).
    class AssetDuplicateCommand final : public AssetFileCommand
    {
    public:
        AssetDuplicateCommand(std::weak_ptr<AssetFileOpExecutor*> exec, std::string label, std::vector<AssetFiles> copies)
            : AssetFileCommand(std::move(exec), std::move(label)), m_copies(std::move(copies)) {}
        std::size_t PayloadBytes() const override { return PayloadByteCount(m_payloads); }
    protected:
        std::optional<std::string> Run(AssetFileOpExecutor& exec, bool undo) override
        { return undo ? exec.RemoveAssets(m_copies, m_payloads, /*discardDirty*/ false) : exec.RestoreAssets(m_payloads); }
        bool SourceLost(const AssetFileOpExecutor& exec, bool undo) const override { return undo ? exec.FilesLost(m_copies) : exec.PayloadsLost(m_payloads); }   // s7.4: Duplicate mirrors Delete
    private:
        std::vector<AssetFiles>    m_copies;
        std::vector<AssetPayloads> m_payloads;   // captured by each undo
    };
}
