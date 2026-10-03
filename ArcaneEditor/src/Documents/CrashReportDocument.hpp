#pragma once

// CrashReportDocument: the read-only .arcdiag viewer (GPU crash diagnostics
// arc, Task 10) -- the third EditorDocument, next to ShaderEditorDocument
// (.arcmat) and SpriteDocument (.arcsprite). Unlike either of those, a crash
// report is NEVER dirty: it presents a Diag::Envelope a backend already
// wrote to disk (Diagnostics.cpp's WriteReportImpl); there is nothing here
// for the user to edit or save.
//
// Same factory split as the other two: the factory registered at
// DocumentHost::RegisterFactory(".arcdiag", ...) (EditorApp.cpp, beside the
// .arcmat/.arcsprite routes) calls Diag::ReadFile and hands the
// already-loaded Envelope to this constructor -- mirrors
// ShaderEditorDocument(DocServices, path, MaterialAssetData) /
// SpriteDocument(Services, path, SpriteAssetData), both of which take their
// asset data pre-loaded for the same reason (SpriteDocument.hpp:68-71).
//
// The secondary file IO this class owns, both done ONCE at construction so
// Draw() never touches disk:
//   1. Sibling RESOLUTION (fix for a post-review finding: Diagnostics.cpp
//      bakes an ABSOLUTE path into envelope.siblingTxt/siblingDmp/
//      siblingGpuDump at capture time -- move or copy the reports folder,
//      or open the project on another machine, and that path goes stale).
//      ResolveSibling tries the stored path first, then falls back to a
//      same-named file beside THIS document's own .arcdiag (siblings are
//      minted at the same stem, beside each other, by WriteReportImpl --
//      Diagnostics.cpp:335-339), then gives up (empty = missing). A
//      `.symbolized.txt` that appears later (a hang report's, written after
//      the host registered the report, `CrashPathTest.cpp:488-498`) is
//      re-checked on the rising edge of window focus (`Tick`) and on
//      `NoteReopened()`, never on a timer (node-page phase s8.1).
//   2. The .gpudump container itself: when the gpudump sibling RESOLVES,
//      the constructor reads and parses it (Diag::ReadGpuDump,
//      IGpuCrashBackend.hpp) so the parsed section-tag inventory is exactly
//      what a headless test can assert on (CrashReportDocumentTest.cpp)
//      without touching ImGui.
//   3. The reporter's model (node-page phase s8.1, LoadReport): the
//      <stem>.symbolized.txt (ParseSymbolized), the log tail (<stem>.log.txt,
//      else the envelope's live log), the ReportView (BuildReportView) and
//      the "<headline> -- <local stamp>" title. Loaded at construction and
//      again only on NoteMoved and the late-.symbolized.txt re-checks above.
//
// Known CPU-report noise (Task 5 deferred minor, restated in the Task 10
// brief): a plain crash/hang envelope today carries fault.type ==
// "device-alive" and a "graphics" queue with an empty timeline (no
// lastCompleted, no inFlight). This class does NOT touch the emission side
// -- IsNoiseFault/IsEmptyQueueTimeline are the render-side filter that keeps
// a CPU-only report from presenting those as real (dead) sections. Both are
// general rules (an empty timeline / an empty-or-"device-alive" fault type),
// never a special case on the queue name or the report kind, so they keep
// working the day a real GPU fault report lands beside the CPU noise.

#include "Documents/EditorDocument.hpp"

#include "ReportView.hpp"        // ArcaneCrashReporter/src: the reporter's pure model (s8.1)
#include "SymbolizedText.hpp"

#include <Arcane/Base/DiagEnvelope.hpp>
#include <Arcane/Guid.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    class CrashReportDocument final : public EditorDocument
    {
    public:
        // `envelope` is already loaded (Diag::ReadFile happens in the
        // factory -- see EditorApp.cpp's crashReportFactory).
        struct Services
        {
            // [file:line] frame links; EditorApp::OpenSourceAtLine in the app, null in most tests.
            std::function<void(const std::filesystem::path&, int)> openSourceAtLine;
        };
        CrashReportDocument(std::filesystem::path path, Arcane::Diag::Envelope envelope, Services services = {});

        const std::string& Title() const override { return m_title; }
        Arcane::Guid AssetGuid() const override { return m_envelope.guid; }
        // Read-only, always. There is no edit surface anywhere on this
        // document, so Dirty() is a hardcoded false rather than a tracked
        // bool -- DocumentHost::RequestClose therefore always takes the
        // "close immediately" branch (DocumentHost.cpp:104-108) and the
        // unsaved-changes confirm modal never fires for a crash report.
        bool Dirty() const override { return false; }
        // Unreachable in practice (Dirty() is always false, so neither
        // RequestClose nor SaveAllDirty ever calls this) -- returns true
        // because there is nothing to persist, never "refused": nothing
        // failed.
        bool Save() override { return true; }
        bool WindowFocused() const override { return m_windowFocused; }
        void Draw(bool& requestClose) override;
        void NoteMoved(const std::filesystem::path& p) override;   // T5 s7.11; reloads at the new stem (s8.1)
        void Tick(double dt) override;    // rising focus edge: re-check a missing .symbolized.txt
        void NoteReopened() override;     // DocumentHost focus-not-reopen: the same re-check

        [[nodiscard]] const Arcane::Reporter::ReportView& View() const noexcept { return m_view; }
        [[nodiscard]] bool HasSymbolized() const noexcept { return m_symbolized.has_value(); }
        [[nodiscard]] const std::filesystem::path& SymbolizedPath() const noexcept { return m_symbolizedPath; }   // <stem>.symbolized.txt; HasSymbolized = it existed AND parsed at the last load
        [[nodiscard]] const std::filesystem::path& LogPath() const noexcept { return m_logResolved; }             // <stem>.log.txt, else envelope.logPath, else empty
        // Calls Services::openSourceAtLine; false when no service is wired.
        bool OpenSource(const std::filesystem::path& file, int line) const;

        // ---- headless-testable model half (no ImGui below this line) -----

        const Arcane::Diag::Envelope& Envelope() const noexcept { return m_envelope; }
        const std::filesystem::path& Path() const noexcept { return m_path; }

        // True when `q` carries nothing worth presenting: no lastCompleted
        // and no inFlight entries. This is the general rule the CPU-report
        // noise (an empty "graphics" queue on every crash/hang envelope)
        // happens to be one instance of -- Draw() hides any queue this
        // returns true for rather than special-casing the queue NAME.
        [[nodiscard]] static bool IsEmptyQueueTimeline(const Arcane::Diag::Envelope::Queue& q) noexcept;

        // True when the fault block is the CPU-report noise value (an empty
        // type, or the literal "device-alive") and should be hidden rather
        // than presented as a dead section.
        [[nodiscard]] static bool IsNoiseFault(const Arcane::Diag::Envelope::Fault& f) noexcept;

        // Queues worth drawing (IsEmptyQueueTimeline filtered out), in the
        // envelope's original order.
        [[nodiscard]] std::vector<const Arcane::Diag::Envelope::Queue*> VisibleQueues() const;
        [[nodiscard]] bool HasVisibleFault() const noexcept { return !IsNoiseFault(m_envelope.fault); }

        // Resolves a sibling field (e.g. envelope.siblingTxt) to an existing
        // file: `recorded` (the stored, possibly-stale absolute path) if it
        // still exists; else a same-named file beside `docPath` (this
        // document's own .arcdiag directory -- see the class header
        // comment); else an empty path ("recorded but missing"). An empty
        // `recorded` (the sibling was never produced) always resolves to
        // empty. Pure -- the only disk touch is std::filesystem::exists,
        // and it never throws (error-code overload). Static + public so a
        // headless test exercises the resolution rule directly.
        [[nodiscard]] static std::filesystem::path ResolveSibling(
            const std::string& recorded, const std::filesystem::path& docPath);

        // Resolved-at-construction sibling locations (see ResolveSibling
        // above). Empty means EITHER "not recorded" (check the envelope
        // field itself, e.g. Envelope().siblingTxt.empty()) OR "recorded
        // but missing" -- Draw() and any other caller must consult the
        // envelope field to tell those two apart, exactly as Draw() does.
        [[nodiscard]] const std::filesystem::path& ResolvedSiblingTxt() const noexcept
        {
            return m_siblingTxtResolved;
        }
        [[nodiscard]] const std::filesystem::path& ResolvedSiblingDmp() const noexcept
        {
            return m_siblingDmpResolved;
        }
        [[nodiscard]] const std::filesystem::path& ResolvedSiblingGpuDump() const noexcept
        {
            return m_siblingGpuDumpResolved;
        }

        // Parsed section-tag inventory of the .gpudump sibling (empty when
        // siblingGpuDump is empty, or ResolveSibling couldn't find it, or
        // the file doesn't parse -- see the ctor comment above). Loaded
        // once at construction, from the RESOLVED path, not the raw
        // (possibly-stale) envelope field.
        [[nodiscard]] const std::vector<std::string>& GpuDumpSectionTags() const noexcept
        {
            return m_gpuDumpTags;
        }

    private:
        void LoadReport();   // symbolized + log + view + title; the ctor and every re-check

        std::filesystem::path    m_path;
        Arcane::Diag::Envelope   m_envelope;
        std::string              m_title;         // headline + " -- " + local stamp; the stem when the stamp does not parse
        std::string              m_windowLabel;    // "<title>###crashdoc_<guid>"
        bool                     m_windowFocused = false;
        // Resolved once at construction (ResolveSibling); see the accessors above.
        std::filesystem::path    m_siblingTxtResolved;
        std::filesystem::path    m_siblingDmpResolved;
        std::filesystem::path    m_siblingGpuDumpResolved;
        std::vector<std::string> m_gpuDumpTags;    // parsed at construction; see GpuDumpSectionTags

        Services                                          m_services;
        std::optional<Arcane::Reporter::ParsedSymbolized> m_symbolized;
        Arcane::Reporter::ReportView                      m_view;
        std::filesystem::path                             m_symbolizedPath;
        std::filesystem::path                             m_logResolved;
        std::vector<std::vector<bool>>                    m_frameFileExists;   // [thread][frame]: probed once per load, never per frame
        std::size_t                                       m_threadIndex = 0;
        double                                            m_copyFlashUntil = 0.0;
        bool                                              m_wasFocused = false;
    };
}
