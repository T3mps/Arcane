#pragma once

// The scene as an Inspector source (inspector-ownership spec s3.1, decision
// 1): the Outliner and the Viewport both write SelectionContext, so they are
// ONE source. Its page is the scene Inspector body (DrawInspectorBody) --
// unchanged pixels, now reached through the host like every other page.
// Owned by EditorApp; rebound every frame because the registry, undo stack
// and project it draws through can all be replaced by a project switch.

#include "Panels/EditorPanels.hpp"     // InspectorState, InspectorServices, SceneEditBinding
#include "Panels/InspectorSource.hpp"
#include "Scene/SelectionContext.hpp"

namespace Arcane { class CommandStack; class Project; }
namespace Astra { class Registry; }

namespace Arcane::Editor
{
    class SceneInspectorSource final : public InspectorSource, public InspectorPage
    {
    public:
        struct Deps
        {
            Astra::Registry*          registry = nullptr;
            SelectionContext*         selection = nullptr;
            Arcane::CommandStack*     undo = nullptr;
            const SceneEditBinding*   binding = nullptr;
            const Arcane::Project*    project = nullptr;
            InspectorState*           state = nullptr;
            const InspectorServices*  services = nullptr;
        };
        void Bind(const Deps& deps) { m_deps = deps; }

        // InspectorSource
        std::string SourceName() const override { return "Scene"; }
        std::string_view Kind() const override { return "scene"; }
        InspectorPage* Page() override;
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override;
        bool RestoreSelection(std::string_view key) override;
        bool Resolves(std::string_view key) const override;

        // InspectorPage
        std::vector<InspectorCrumb> Breadcrumb() const override;
        void Draw(PropertyGrid& grid) override;

    private:
        [[nodiscard]] bool Alive(Astra::Entity e) const;

        Deps m_deps{};
        SelectionContext m_pinnedSel;                 // PageFor's pinned selection: the alive subset of the pinned key, rebuilt every call
        const SelectionContext* m_drawSel = nullptr;  // what Draw/Breadcrumb read this frame
    };
}
