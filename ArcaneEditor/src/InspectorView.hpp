#pragma once

// InspectorView: the reflected half of the Inspector -- the Astra field
// visitor that turns ONE component's reflected fields into rows, and the
// close machinery for a field-edit gesture whose widget stopped being drawn.
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

    // Close a field-edit gesture whose widget stopped being drawn.
    //
    // BeginGestureIfActivated opens the transaction and EndGesture closes it,
    // but EndGesture reads item state -- so it can only run while that widget
    // is still being SUBMITTED. Anything that stops it being submitted while
    // it owns the gesture strands the transaction open: its component header
    // (or a category sub-header) collapsing, the search query hiding the
    // field, the selection turning multi (which swaps the drag for text
    // boxes), the Inspector window collapsing or its tab going to the
    // background. That is not cosmetic -- CanEditStructure is
    // `!undo.InTransaction()`, so Add/Remove Component stay dead until some
    // LATER gesture force-closes the orphan and silently absorbs its stale
    // snapshots into an unrelated undo step.
    //
    // The collapse case is reachable, not theoretical: a ctrl+click text
    // entry on a DragFloat leaves g.ActiveIdAllowOverlap = !io.MouseDown[0]
    // (imgui_widgets.cpp:5004) and nothing resets it, so with the mouse up
    // the header above is still hoverable -- that flag is exactly what the
    // hover gate tests (imgui.cpp:5091) -- and a click over its arrow presses
    // on MouseDown (imgui_widgets.cpp:7012) and flips is_open in the SAME
    // frame (:7045/:7075), before the field would have been drawn. A
    // mouse-HELD drag is NOT reachable this way: it never sets
    // ActiveIdAllowOverlap, so the same gate rejects every other item.
    //
    // COMMIT, not Cancel, for both kinds of orphan:
    //   - mid-drag: the edits are already applied and the user watched them
    //     happen. Cancel drops the transaction WITHOUT reverting
    //     (CommandStack.cpp:75-82), which would leave them applied and
    //     permanently un-undoable -- the same hazard ApplyRegistryMutation
    //     refuses a structural memento over.
    //   - ctrl+click text entry: nothing was applied at all (a temp input
    //     writes only on submit). Commit re-snapshots, drops every component
    //     whose bytes match, and returns before pushing a step or clearing
    //     redo when none differ (CommandStack.cpp:61-62) -- so here Commit
    //     lands exactly where Cancel would.
    void CloseAbandonedGesture(Arcane::CommandStack& undo, InspectorState& state);

    // Runs CloseAbandonedGesture on EVERY exit from DrawInspectorPanel. RAII
    // rather than a call before each ImGui::End(): the defect being fixed IS
    // a missed close path, and the panel already has an early return (no
    // selection) that is one of them. Declared as the panel's FIRST local so
    // it destructs LAST -- after ImGui::End(), on both paths, and on frames
    // where ImGui::Begin returned false (collapsed or background tab), where
    // every widget inside bails before ItemAdd on window->SkipItems (e.g.
    // imgui_widgets.cpp:2722) and so cannot report its own deactivation.
    struct GestureCloseGuard
    {
        Arcane::CommandStack& undo;
        InspectorState&       state;
        ~GestureCloseGuard() { CloseAbandonedGesture(undo, state); }
    };
}
