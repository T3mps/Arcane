#pragma once

// AssetActivityLog (asset-manager redesign, Plan 2 Task 5): a session-only
// ring of "things that happened to assets this session" (spec s9.2) -- a
// source changed on disk, a texture cooked or was refused, an asset was
// created (drop discovery, the unified create dialog, a new-file scene
// save, a crash report landing in the registry). Task 8 draws this as the
// Assets panel's activity feed; this unit only ever accumulates and replays
// it, with zero UI and zero engine-facade calls of its own.
//
// MAIN-THREAD ONLY, no mutex -- same invariant EditorApp::m_cookDiagnostics
// documents at its own declaration (EditorApp.hpp): every push site listed
// in the Task 5 brief runs from a main-thread-only poll/pump/dispatch path
// (PollAssetWatch, OnCookCompleted, OnArtifactRefused, ConsumeCreateResult,
// DoSaveScene, PollDiagnosticReports), so no cross-thread access is ever
// reachable and a mutex would guard nothing real.
//
// PERSISTENCE: none, by design -- a fresh session starts with an empty log
// (Clear() also runs on every project switch, EditorApp::SwitchProject),
// same "this session only" posture the spec gives the activity feed.

#include <Arcane/Guid.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    // What happened. Deleted is vocabulary only -- the editor has no delete
    // flow and the registry no Remove API (Ruling 11); it is recorded here
    // so Task 8's feed and any future delete flow share one enum, not
    // invented as a producer today.
    enum class AssetActivityKind : std::uint8_t
    {
        SourceChanged,   // external .arcmat edit, or a texture/.meta mtime change
        Cooked,          // a background cook finished successfully
        CookRefused,     // a cook failure OR an artifact refusal (Problems-pane twin)
        Created,         // drop discovery, the create dialog, a new-file scene save, a crash report
        Deleted,         // no producer today -- see above
    };

    // One row. `name` is a snapshot taken at push time -- the asset it
    // names may be renamed or vanish before the feed ever reads this back,
    // so the row must stand on its own rather than re-resolving the guid
    // later. `detail` carries a refusal reason/kind string; empty for the
    // kinds that have none (SourceChanged, Created).
    struct AssetActivityEntry
    {
        std::chrono::steady_clock::time_point when;
        Arcane::Guid      guid;
        std::string       name;    // snapshot at push time; the asset may vanish
        AssetActivityKind kind = AssetActivityKind::SourceChanged;
        std::string       detail;  // refusal reason etc.; may be empty
    };

    // Session-only ring (~100, spec s9.2). Main-thread only -- see the file
    // header. `when` is ALWAYS stamped by the caller, never taken here: a
    // test needs to control time to prove ordering/wrap, and every
    // production push site already has `std::chrono::steady_clock::now()`
    // in hand at the call site.
    class AssetActivityLog
    {
    public:
        static constexpr std::size_t kCapacity = 100;

        // Appends one entry, overwriting the oldest once kCapacity is
        // reached (plain modular index -- see the .cpp for the exact
        // wrap math).
        void Push(AssetActivityEntry e);

        // Newest-first replay -- Task 8's feed order. Safe to call at any
        // size, including zero (no-op).
        void ForEachNewestFirst(const std::function<void(const AssetActivityEntry&)>& fn) const;

        // Current entry count, 0..kCapacity.
        [[nodiscard]] std::size_t Size() const;

        // Drops every entry -- the project-switch reset (EditorApp::
        // SwitchProject), same shape as AssetPanelModel::ResetForProjectSwitch
        // and AssetReferenceIndex::Clear.
        void Clear();

    private:
        std::vector<AssetActivityEntry> m_ring;   // grows to kCapacity, then wraps
        std::size_t m_next = 0;   // the slot the NEXT Push writes into
    };
}
