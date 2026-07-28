#pragma once

// InspectorView: the reflected half of the Inspector -- the Astra field
// visitor that turns ONE component's reflected fields into rows. The gesture
// bracket those rows use, and the close machinery for a gesture whose widget
// stopped being drawn, are EditGesture's (EditGesture.hpp); the visitor keeps
// only the two thin wrappers its arms call.
//
// The panel (EditorPanels.cpp, DrawInspectorPanel) keeps the shell around it:
// the component loop, the component/category headers, the category
// enumeration, Add/Remove Component, the search box, and the field grid each
// group's rows are drawn into. It drives the rows through
// DrawReflectedComponent once per component-and-category group -- the
// uncategorised pass FIRST, then one call per named category.

#include "EditorPanels.hpp"   // InspectorState, InspectorServices, and the
                              // Arcane::CommandStack / Arcane::Project decls

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>   // Registry::ComponentInfo

#include <span>
#include <string_view>

namespace Arcane::Editor
{
    // Everything one drive of the field visitor needs. Grouped rather than
    // passed as eleven parameters because the panel builds it ONCE per
    // component and re-drives it per category -- only `activeCategory` moves
    // between the calls.
    struct ReflectedComponentArgs
    {
        Astra::Registry&                      registry;
        // descriptor + meta + data, all non-null: the panel's component loop
        // already skips an unreflected component (no meta) and a tag one (no
        // data) before it gets here.
        const Astra::Registry::ComponentInfo& component;
        // The last-clicked entity, which is what every single-entity consumer
        // operates on. NOT selection.front(): Entities() keeps SELECTION
        // order (front = oldest) and the primary is the most recently clicked
        // member, so the two disagree for any multi-select -- see
        // SelectionContext.hpp:4-6.
        Astra::Entity                         primary;
        // Fan-out targets, including the primary.
        std::span<const Astra::Entity>        selection;
        Arcane::CommandStack*                 undo;        // null while Play runs
        const Arcane::Project*                project;     // may be null
        const InspectorServices*              services;    // may be null
        InspectorState&                       state;
        std::string_view                      componentDisplayName;
        std::string_view                      activeCategory;
        std::string_view                      filterQuery;
    };

    // Draw one component's reflected fields, for ONE category group, as rows
    // into the grid the CALLER opened. Emits rows only -- no header, no grid,
    // no window -- so the panel's headers stay full-width around it.
    //
    // An EMPTY `activeCategory` is the uncategorised pass, not "draw
    // everything": it is compared against each field's own category, and
    // CategoryOfField returns empty for an unannotated field.
    void DrawReflectedComponent(const ReflectedComponentArgs& args);
}
