#pragma once

// The Inspector's contribution interfaces (inspector-ownership spec s3.1/s3.2).
// A SOURCE is anything that can select (the scene, an opted-in document); it
// contributes a PAGE (breadcrumb + body drawn through PropertyGrid). No ImGui
// here: InspectorHost routes these headlessly, InspectorWindows draws them.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    class PropertyGrid;

    // One breadcrumb segment. `select` re-selects that level in the SOURCE
    // (an unpinned instance). `key` is that level's selection key: a PINNED
    // instance re-targets its own pin to it (InspectorHost::RepinKey) and
    // never touches the source; nullopt = no page exists at that level (the
    // crumb draws inert while pinned).
    struct InspectorCrumb
    {
        std::string label;
        std::function<void()> select;
        std::optional<std::string> key;
    };

    class InspectorPage
    {
    public:
        virtual ~InspectorPage() = default;
        [[nodiscard]] virtual std::vector<InspectorCrumb> Breadcrumb() const = 0;
        virtual void Draw(PropertyGrid& grid) = 0;
    };

    class InspectorSource
    {
    public:
        virtual ~InspectorSource() = default;
        // The first crumb: "Scene", "Player.arcinput".
        [[nodiscard]] virtual std::string SourceName() const = 0;
        // The source's KIND (InspectorKinds.hpp's catalog id: "scene",
        // "input-actions", "assets"); an Inspector instance's filter admits or
        // excludes by it. "" = a source that never selects (mesh/sprite
        // documents): admitted only by an unfiltered (All) instance.
        [[nodiscard]] virtual std::string_view Kind() const = 0;
        // The page for the CURRENT selection. Null = nothing to show.
        // The returned page is a VIEW valid until the next Page()/PageFor() call on this source: resolve it immediately before Breadcrumb() + Draw().
        [[nodiscard]] virtual InspectorPage* Page() = 0;
        // The page for a SPECIFIC selection key (a pinned instance). Null when
        // the key no longer resolves. Pages are keyed on purpose: "pinned =
        // keep THIS page" must survive the source selecting something else.
        // The returned page is a VIEW valid until the next Page()/PageFor() call on this source: resolve it immediately before Breadcrumb() + Draw().
        [[nodiscard]] virtual InspectorPage* PageFor(std::string_view key) = 0;
        // Opaque, stable across frames; "" = nothing selected. The history
        // stores these, so a key must survive being handed back later.
        [[nodiscard]] virtual std::string SelectionKey() const = 0;
        // Re-select `key` inside the source. False = unresolvable now (the
        // entity died, the binding was removed): the host prunes the entry.
        virtual bool RestoreSelection(std::string_view key) = 0;
        // PURE: would RestoreSelection(key) succeed right now? Never selects,
        // never bumps an epoch. The host prunes stale history with it once
        // per frame so the back/forward arrows are truthful (spec s6 rule 3).
        [[nodiscard]] virtual bool Resolves(std::string_view key) const = 0;
    };

    // The page's crumb labels joined with " > ", or the source's name when
    // there is no page / an empty breadcrumb. History labels, the header
    // tooltips and the witness report's `inspector.breadcrumb` all use it.
    [[nodiscard]] inline std::string InspectorCrumbText(const InspectorSource& src, const InspectorPage* page)
    {
        std::string out;
        if (page)
            for (const InspectorCrumb& c : page->Breadcrumb()) { if (!out.empty()) out += " > "; out += c.label; }
        return out.empty() ? src.SourceName() : out;
    }
}
