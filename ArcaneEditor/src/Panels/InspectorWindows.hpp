#pragma once

// The Inspector windows (inspector-ownership spec s3.2/s3.3): one window per
// InspectorHost instance -- "Inspector" (id 0, the registry panel) and
// "Inspector N" for Window -> New Inspector. Each draws the header (back /
// forward over the instance's FILTERED history, the kind-filter dropdown, the
// clickable breadcrumb, the pin) and then the page body of the source the
// host routes to it, through that instance's own PropertyGrid state (so two
// instances showing the same page keep separate text drafts).
// Instance ids are pool slots (InspectorHost::kMaxInstances), so a closed
// instance's `[Window]` entry is the one its next opener inherits.

#include "Panels/InspectorHost.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace Arcane::Editor
{
    // Instance 0's ImGui id. ImHashStr skips "###", so this is the same id as the
    // legacy bare "Inspector" window: the constant is for clarity, not a new id.
    inline constexpr const char* kPrimaryInspectorWindowId = "###Inspector";

    // The window title for an instance (inspector filters spec s5):
    // "Inspector" / "Inspector <N+1>", then " - <filter label>" when the
    // filter is not All, then the stable id -- "###Inspector" for instance 0,
    // "###inspector_<id>" otherwise -- so a filter change never moves the
    // window, its dock slot or its [Window] ini entry.
    [[nodiscard]] std::string InspectorWindowTitle(const InspectorHost::Instance& inst);

    // The Inspector header's responsive layout (final fix H): one row --
    // arrows, filter combo, breadcrumb, pin -- while that row fits the combo
    // and at least kInspectorHeaderMinCrumbWidth of breadcrumb; otherwise
    // the breadcrumb takes its OWN full-width row under the others. Below
    // kInspectorIconComboBelow the combo collapses to an icon-only combo (the
    // full label in its tooltip). The pin is never clipped: it ends row 1,
    // or leads the breadcrumb row when even arrows + combo + pin do not fit.
    // Pure, so the thresholds are unit-tested.
    inline constexpr float kInspectorHeaderMinCrumbWidth = 120.0f;
    inline constexpr float kInspectorIconComboBelow = 200.0f;
    struct InspectorHeaderMetrics
    {
        float avail = 0.0f;       // the header row's content width
        float arrows = 0.0f;      // back + spacing + forward
        float comboFull = 0.0f;   // the labelled combo's natural width (capped)
        float comboIcon = 0.0f;   // the icon-only combo's width
        float pin = 0.0f;
        float spacing = 0.0f;     // ItemSpacing.x
    };
    struct InspectorHeaderLayout
    {
        bool  crumbsOwnRow = false;
        bool  iconCombo = false;
        bool  pinOnCrumbRow = false;
        float comboWidth = 0.0f;  // the combo's frame width as drawn (the preview is ellipsized to it)
    };
    [[nodiscard]] InspectorHeaderLayout LayoutInspectorHeader(const InspectorHeaderMetrics& m);

    struct InspectorWindowsState
    {
        std::unordered_map<int, PropertyGridState> grids;   // per instance id (ids are pool slots, InspectorHost::kMaxInstances)
    };

    struct InspectorWindowsResult
    {
        std::vector<int> closed;   // ids whose X was clicked: the app removes them after the draw
        // Ctrl+S pressed while an instance window was focused: the source whose
        // page that instance showed (null when pinned-and-closed). The app maps
        // it to a document and saves it -- UE's asset-editor host resolves
        // Ctrl+S to the toolkit that OWNS the focused panel, never the panel.
        std::vector<InspectorSource*> saveRequested;
        // The source shown by the instance that held focus at this draw (null =
        // no instance focused). The app's raw-scancode scene Ctrl+S stands down
        // when this names a document, as it does for a focused document window.
        InspectorSource* focusedSource = nullptr;
    };

    // `primaryOpen` is PanelVisibility's flag for instance 0 (null = no X).
    // Instance 0 is skipped when the flag is false; extra instances always
    // draw (they carry their own X).
    InspectorWindowsResult DrawInspectorWindows(InspectorHost& host, InspectorWindowsState& state,
                                                bool* primaryOpen);

    // The Inspector instance ID LIST's imgui.ini section,
    // "[EditorInspector][Instances]": one `Ids=<extra ids>` line (instance 0
    // is implicit; an empty line restores {0}), then one always-written
    // `Filters=<id>:<kind>+<kind>,...` line (each instance's EXCLUDED kinds;
    // an absent instance is All -- inspector filters spec s7). A load that
    // sees no `Filters=` line flags the one-time legacy upgrade
    // (InspectorHost::TakeLegacyLayoutUpgrade, spec s6). Registered on the
    // CURRENT ImGui context with UserData = &host, so `host` must outlive the
    // context's settings use. Idempotent; a no-op with no current context.
    // Its ClearAllFn (ImGui::ClearIniSettings -- a windowed project switch)
    // resets the list to exactly {0} and instance 0's filter to All. Filters
    // are layout and persisted; pins are NOT: a pin names a selection, and a
    // selection does not survive a restart.
    void RegisterInspectorInstancesSettings(InspectorHost& host);
}
