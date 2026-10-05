#pragma once
// DialogSlot: one in-flight async file-dialog result (architecture pass sec 2).
// The OS dialog's completion thunk fires on an SDL BACKGROUND thread; the
// consumer Take()s at the top of the next frame. The epoch is what makes
// clear-on-switch airtight: a thunk completing AFTER the switch Stashes into
// a bumped epoch and is dropped, so a dialog opened in project A can never
// land in project B (audit defect A2's sibling). Contract:
//   Arm()   at the dialog-LAUNCH site; the returned epoch rides in the thunk.
//   Stash() from the thunk (any thread); ignored when the epoch is stale.
//   Take()  once per frame at the consume site; empties the slot.
//   Clear() on project switch (ResetPerProjectState).
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    template <typename Payload>
    class DialogSlot
    {
    public:
        [[nodiscard]] std::uint64_t Arm()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_value.reset();   // last-writer-wins, matching today's raw-string behavior
            return ++m_epoch;
        }

        void Stash(std::uint64_t epoch, Payload value)
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (epoch != m_epoch)
                return;        // Clear()/re-Arm() since Arm -> a dead dialog's result
            m_value = std::move(value);
        }

        [[nodiscard]] std::optional<Payload> Take()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            std::optional<Payload> out;
            out.swap(m_value);
            return out;
        }

        void Clear()
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            ++m_epoch;
            m_value.reset();
        }

    private:
        std::mutex             m_mutex;
        std::uint64_t          m_epoch = 0;
        std::optional<Payload> m_value;
    };

    // Heap request the OS file/folder picker trampoline owns. SDL fires the
    // callback exactly once (null path on cancel); PathPickedThunk is the
    // single-owner free. EditorApp::BrowseSettingsPath, scene/project/material
    // pickers, and tests all pass this to the same PathPickedThunk.
    struct PathDialogRequest
    {
        DialogSlot<std::string>* slot = nullptr;
        std::uint64_t            epoch = 0;
    };

    // The OS picker's completion trampoline (SDL background thread). A non-null
    // path Stashes into the armed slot; null = cancel. This is the production
    // function ShowOpenFileDialog / ShowOpenFolderDialog / ShowSaveFileDialog
    // register -- tests fire it the same way the OS picker does.
    inline void PathPickedThunk(const char* path, void* user)
    {
        std::unique_ptr<PathDialogRequest> req(static_cast<PathDialogRequest*>(user));
        if (path && req && req->slot)
            req->slot->Stash(req->epoch, path);
    }

    // One DialogSlot per dialog kind. EditorApp holds one of these; tests
    // drive settingsPath through the same type the frame consume site reads.
    struct DialogInbox
    {
        DialogSlot<std::string> sceneOpen;
        DialogSlot<std::string> sceneSave;
        DialogSlot<std::string> projectOpen;
        DialogSlot<std::string> materialOpen;
        DialogSlot<std::string> settingsPath;
        void ClearAll()
        {
            sceneOpen.Clear(); sceneSave.Clear(); projectOpen.Clear();
            materialOpen.Clear(); settingsPath.Clear();
        }
    };
}
