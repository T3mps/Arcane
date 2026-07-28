#include "InspectorView.hpp"

#include "AssetBrowser.hpp"
#include "EditorWidgets.hpp"
#include "InspectorFields.hpp"
#include "InspectorMeta.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Project/Project.hpp>

#include <Astra/Reflection/FieldVisitor.hpp>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // ImGuiItemFlags_MixedValue (:984 -- the tri-state
                              // checkbox's flag; PushItemFlag itself is public,
                              // imgui.h:546) and GetActiveID (:3532 -- who owns
                              // ActiveId, which the abandoned-gesture check asks
                              // and imgui.h does not expose)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // The authored Range as a [lo, hi] pair, or nullopt when the field has no
        // Range or the authored one does not actually bind.
        //
        // "Binds" is ImGui's own rule for the drags below, so the typed paths that
        // call this agree with them: min < max, or a NON-ZERO min == max
        // (imgui_widgets.cpp:2540, minus its ClampZeroRange term -- these calls do
        // not pass that flag). imgui.h:687's "if v_min >= v_max we have no bound"
        // is only the header's doc comment; the implementation pins Range(5, 5) at
        // 5 and leaves just min > max and Range(0, 0) unbounded.
        [[nodiscard]] std::optional<std::pair<double, double>>
        BindingRange(const Astra::FieldInfo& f)
        {
            const std::optional<Astra::Range> r = Arcane::Editor::RangeOfField(f);
            if (!r)
                return std::nullopt;
            if (r->min < r->max || (r->min == r->max && r->min != 0.0))
                return std::make_pair(r->min, r->max);
            return std::nullopt;
        }

        // FieldInfo-taking convenience over the range-free drag cores in
        // EditorWidgets: resolving a field to its Astra::Range is reflection's
        // business, which is why the widget layer takes the resolved optional
        // and these two do the resolving. The bodies name the widget-layer
        // cores QUALIFIED because unqualified lookup stops at this namespace,
        // where only these two overloads live.
        [[nodiscard]] bool RangedDragFloat(const Astra::FieldInfo& f, const char* label,
                                           float* v, float fallbackSpeed)
        {
            return Arcane::Editor::RangedDragFloat(label, v, fallbackSpeed,
                                                   Arcane::Editor::RangeOfField(f));
        }

        [[nodiscard]] bool RangedDragInt(const Astra::FieldInfo& f, const char* label, int* v)
        {
            return Arcane::Editor::RangedDragInt(label, v, Arcane::Editor::RangeOfField(f));
        }

        // Renders one widget per reflected field and applies edits through the
        // pure InspectorFields writers (kept ImGui-free so the write-back is unit-
        // testable). Unsupported/compound types (e.g. glm::mat3, enums) render
        // disabled text -- never crash, never silently misinterpret bytes.
        //
        // Each field's edit gesture is bracketed into the undo stack: the first
        // frame a widget activates (ImGui::IsItemActivated(), BEFORE the edit
        // applies) opens a transaction and snapshots the owning component's
        // pre-edit bytes; the widget's release closes it -- Commit() if the value
        // actually changed (IsItemDeactivatedAfterEdit), Cancel() on a pure click
        // (IsItemDeactivated with no edit) so a no-op click never leaks an empty
        // undo step.
        struct ImGuiFieldVisitor : Astra::IFieldVisitor
        {
            Arcane::CommandStack*             stack = nullptr;
            Astra::Entity                     entity{};
            const Astra::ComponentDescriptor* descriptor = nullptr;
            std::string                       typeName;
            const Arcane::Project*            project = nullptr;   // asset-ref resolve/pick; may be null
            // Sprite-asset arc, Task 4: texture-drop auto-mint on a Sprite-typed
            // AssetRef field. Wired UNCONDITIONALLY today -- EditorAppFrame.cpp
            // passes &m_inspectorServices on every DrawInspectorPanel call, and
            // EditorApp::Init sets mintSpriteForTexture unconditionally (not
            // gated on Play/Edit) -- so in the shipping app this is never
            // actually null. The null check in the AssetRef arm below is
            // defensive, for a caller that does not wire InspectorServices at
            // all (DrawInspectorPanel's `services` parameter defaults to
            // nullptr).
            //
            // Play-mode note: `stack` above IS gated (null while Play runs,
            // binding.editMode -> nullptr), but `services` is not, so a
            // texture drop during Play still mints/reuses the .arcsprite and
            // calls ApplyGuidImmediate, which applies the Guid edit via its
            // unconditional ForEachTarget even with stack == nullptr (it only
            // skips opening the ScopedTransaction and taking the per-target
            // Snapshot -- both live inside the same `if (stack)` block,
            // InspectorView.cpp:397-402). A Play-mode texture drop
            // therefore mints the file and writes the Guid with NO undo step
            // -- exactly the no-undo-in-Play behavior every other AssetRef
            // drop already has; this branch adds a minted file as a
            // consequence, not a new undo hole.
            const InspectorServices*          services = nullptr;

            // The Inspector's live search, set per component before the visit.
            // `componentDisplayName` is the header's prose name, which is part of
            // what MatchesInspectorFilter tests: a hit on it shows EVERY field in
            // this component. `query` views the panel's persistent
            // InspectorState buffer, which outlives the visitor; empty matches
            // everything, so the unfiltered case needs no branch of its own.
            std::string                       componentDisplayName;
            std::string_view                  query;
            // Which category group this drive is rendering. Astra's VisitFields
            // always walks every field, so a visitor that must draw one group at
            // a time has to select here; the panel re-drives it once per group.
            // EMPTY IS THE UNCATEGORISED PASS, not "draw everything":
            // CategoryOfField returns empty for an unannotated field, so the two
            // compare equal and only those fields draw.
            std::string_view                  activeCategory{};
            // The in-flight gesture's CommandStack ownership token, owned by the
            // panel's persistent InspectorState -- the visitor is rebuilt every
            // frame but the gesture it brackets is not. Never null when `stack` is
            // non-null.
            Arcane::TransactionId*            gestureTxn = nullptr;
            // ImGui id of the widget that opened that gesture, parked in the same
            // persistent state for the same reason -- it is what CloseAbandonedGesture
            // below compares ActiveId against. Never null when `stack` is non-null.
            ImGuiID*                          gestureItem = nullptr;
            // The string row's activation-time cancel reference and the id of
            // the row that latched it, both parked in the same persistent
            // InspectorState for the same reason -- see the String arm below
            // and InspectorState in EditorPanels.hpp. Wired unconditionally by
            // DrawReflectedComponent (they describe ImGui cancel semantics, not
            // undo, so they matter while Play runs too); the arm still falls
            // back to a per-frame seed if either is null.
            std::string*                      stringSeed = nullptr;
            ImGuiID*                          stringSeedItem = nullptr;

            // Fan-out targets. `selection` includes the primary; entities lacking
            // this component are skipped (the panel only shows components the whole
            // selection shares, but selection and panel are a frame apart).
            Astra::Registry*                  registry = nullptr;
            // A span rather than the panel's vector: an EMPTY one is exactly
            // the old null pointer's "no fan-out context" (the two guards
            // below asked `!selection || empty` and `!selection` and now ask
            // only about emptiness -- ComputeFieldMixed over an empty span
            // walks no entity and returns the same empty mask the null guard
            // returned).
            std::span<const Astra::Entity>    selection{};

            bool IsWriting() const noexcept override { return true; }

            // Run `fn(instanceOfThatEntity)` for every selected entity carrying
            // this component. Falls back to the primary's own instance when the
            // fan-out context is absent, so a field is never silently un-editable.
            template<typename Fn>
            void ForEachTarget(void* primaryInstance, Fn&& fn)
            {
                if (!registry || selection.empty())
                {
                    fn(entity, primaryInstance);
                    return;
                }
                for (Astra::Entity e : selection)
                    if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                        fn(e, data);
            }

            void BeginGestureIfActivated(const std::string& field, void* primaryInstance)
            {
                if (stack && ImGui::IsItemActivated())
                {
                    // Clicking field B while field A is mid-gesture moves ActiveId in
                    // ONE frame, and if B is drawn ABOVE A then B activates before A's
                    // EndGesture runs. Without this, B's Begin would see A's
                    // transaction still open, return None (CommandStack.cpp:16-17),
                    // and A's later Commit(None) would no-op -- leaving A's
                    // transaction open forever holding stale `before` bytes. Close
                    // A's here so its edit lands, then open ours. A's own EndGesture
                    // still runs later this frame, but the slot below now names B as
                    // the owner, so EndGesture's ownership guard makes it return --
                    // it does NOT hand B's LIVE token to Commit/Cancel.
                    if (*gestureTxn != Arcane::TransactionId::None)
                        stack->Commit(*gestureTxn);
                    // Park the token for the deactivation frame (EndGesture). None
                    // means a gizmo drag already owns the stack -- this gesture then
                    // JOINS it: the snapshots below still land, and the drag's own
                    // Commit records them, so the edit is never lost.
                    *gestureTxn = stack->Begin("Edit " + typeName + "." + field);
                    // Remember WHICH widget owns the gesture, so the panel can tell a
                    // live one from an abandoned one. IsItemActivated() above is true
                    // exactly when g.ActiveId == g.LastItemData.ID (imgui.cpp:6549),
                    // so this is ActiveId -- including through a ctrl+click text entry
                    // (TempInputText re-derives the id from the same window+label) and
                    // through AxisDragFloatN, where EndGroup forwards the live component
                    // id into LastItemData (imgui.cpp:12480).
                    *gestureItem = ImGui::GetItemID();
                    // One Begin + N snapshots + one Commit = one undo step for the
                    // whole fan-out (CommandStack dedupes per (entity, descriptor)).
                    ForEachTarget(primaryInstance,
                                  [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
                }
            }

            // UE parity: a multi-selection gets NO drag widget and ignores every
            // non-committed change. Both belts are Unreal's, in
            // ComponentTransformDetails.cpp -- `.AllowSpin(SelectedObjects.Num()
            // == 1)` (:505/:551/:628) and, at :1248, "Ignore interactive changes
            // when we have more than one selected object". A single selection
            // keeps the drag exactly as before.
            [[nodiscard]] bool Multi() const noexcept
            { return selection.size() > 1; }

            [[nodiscard]] Arcane::Editor::FieldMixedMask MixedFor(const Astra::FieldInfo& f) const
            {
                if (!registry || !descriptor)
                    return {};
                return Arcane::Editor::ComputeFieldMixed(*registry, selection,
                                                         descriptor->hash, f);
            }

            // Single-shot fan-out for the multi-select path: the commit is one
            // discrete event (no widget gesture spanning frames to bracket), so
            // Begin + snapshot-all + apply + Commit happen in one call.
            //
            // ScopedTransaction, NOT a bare Begin/Commit pair: this can fire in the
            // same frame as a gizmo press (clicking a handle deactivates a text box
            // that still holds typed text), and an unconditional Commit here used to
            // close the DRAG's transaction -- the rest of the drag then ran against a
            // closed stack and lost its entire undo record. The scope commits only
            // what it opened; when it joins a live drag instead, the snapshots ride
            // along and the drag's own Commit records them.
            template<typename Fn>
            void ApplyImmediate(const std::string& field, void* primaryInstance, Fn&& apply)
            {
                std::optional<Arcane::ScopedTransaction> txn;
                if (stack)
                {
                    txn.emplace(*stack, "Edit " + typeName + "." + field);
                    ForEachTarget(primaryInstance,
                                  [&](Astra::Entity e, void*) { txn->Snapshot(e, descriptor); });
                }
                ForEachTarget(primaryInstance, [&](Astra::Entity, void* d) { apply(d); });
            }

            // One multi-select scalar row: `count` TEXT-ENTRY boxes (never a
            // drag), each rendered BLANK when that component differs across the
            // selection -- Unreal's "unset means multiple differing values"
            // (ComponentTransformDetails.cpp:1026). Returns the index of the
            // component the user COMMITTED this frame, or -1 for none; typing
            // alone writes nothing. Laid out like ImGui's own InputScalarN.
            //
            // DOUBLE, not float, and an `integral` flag: an int32 field used to
            // round-trip int32 -> float -> "%.3f" -> strtof -> int, which both
            // showed "7.000" in an integer box and TRUNCATED silently past 2^24
            // where float can no longer represent consecutive integers. double
            // represents every int32 and every float exactly, so this row is now
            // lossless for both kinds and `integral` picks the formatting.
            //
            // `idSeed` is an id scope only -- it keeps the N boxes' ids off the
            // row's other items -- and is NOT drawn: the display name lives in
            // the grid's label column now, so the trailing SameLine'd text this
            // row used to end on is gone with it.
            //
            // `axisColors` asks for the X/Y/Z strip on each box. It is a
            // parameter rather than `count > 1` because the two are not the same
            // question: this row also serves plain scalars, and a future
            // two-box row that is NOT a vector (a min/max pair, say) would be
            // silently painted red/green by that shortcut.
            int MultiScalarRow(const char* idSeed, int count, const double* vals,
                               const Arcane::Editor::FieldMixedMask& mask, bool integral,
                               bool axisColors, double& outValue)
            {
                int committed = -1;
                ImGui::BeginGroup();
                ImGui::PushID(idSeed);
                // Fills the value cell: the caller's SetNextItemWidth(-FLT_MIN)
                // is still pending here (nothing between it and this call
                // submits an item, and PushMultiItemsWidths consumes the flag
                // itself at imgui.cpp:12292), so CalcItemWidth resolves to the
                // cell's full width and the boxes split THAT.
                ImGui::PushMultiItemsWidths(count, ImGui::CalcItemWidth());
                for (int i = 0; i < count; ++i)
                {
                    ImGui::PushID(i);
                    if (i > 0)
                        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

                    char buf[64];
                    if (mask.Test(i))
                        buf[0] = '\0';                       // differs: show nothing
                    else if (integral)
                        std::snprintf(buf, sizeof(buf), "%lld",
                                      static_cast<long long>(vals[i]));
                    else
                        std::snprintf(buf, sizeof(buf), "%.3f", vals[i]);

                    // Scientific notation is offered only for real numbers -- "1e3"
                    // in an integer box is not something to encourage.
                    const bool entered = ImGui::InputText(
                        "", buf, sizeof(buf),
                        ImGuiInputTextFlags_CharsDecimal |
                        (integral ? 0 : ImGuiInputTextFlags_CharsScientific) |
                        ImGuiInputTextFlags_EnterReturnsTrue);
                    // Enter, or focus lost after an edit: the two ways ImGui says
                    // "the user is done with this box".
                    if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && buf[0] != '\0')
                    {
                        char* end = nullptr;
                        const double parsed = std::strtod(buf, &end);
                        if (end != buf)
                        {
                            outValue = parsed;
                            committed = i;
                        }
                    }
                    // After the commit block, not before it: everything above
                    // reads g.LastItemData (IsItemDeactivatedAfterEdit), and
                    // putting the decoration last means no reader has to
                    // re-establish that a draw-list call left that state alone.
                    if (axisColors)
                        DrawAxisBar(i);
                    ImGui::PopID();
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
                ImGui::EndGroup();
                return committed;
            }

            void EndGesture()
            {
                if (!stack) return;
                // ONLY THE WIDGET THAT OPENED THE GESTURE MAY CLOSE IT. This runs for
                // every row, and a row that never opened a gesture still reports its
                // own deactivation: click into a string box, then press on a drag
                // drawn ABOVE it, and in ONE frame the drag activates (parking its
                // LIVE token in *gestureTxn) while the string box, submitted after
                // it, reports IsItemDeactivated -- imgui.cpp:6559 answers from
                // DeactivatedItemData.ID, which is the string box's own id. Without
                // this guard that click-without-typing would Cancel the drag's
                // transaction, and Cancel discards WITHOUT reverting
                // (CommandStack.cpp:75-82), so the whole drag stays applied and
                // permanently un-undoable.
                //
                // GetItemID() is g.LastItemData.ID (imgui.cpp:6676-6680), the same
                // value BeginGestureIfActivated recorded at activation, for every
                // shape this panel draws:
                //   - bare DragFloat/DragInt/Checkbox: ItemAdd stamps it on both
                //     frames (imgui.cpp:11979).
                //   - ctrl+click temp input: the id comes from the same window+label
                //     (imgui_widgets.cpp:2727, :4728) and the temp input skips
                //     ItemAdd (:4791-4792), so the drag's own id stands.
                //   - AxisDragFloatN and MultiScalarRow (groups): EndGroup re-points
                //     LastItemData.ID at the group's live ActiveId, ELSE at the child
                //     that deactivated inside it (imgui.cpp:12477-12482) -- so the
                //     activation frame yields the child that took ActiveId, and the
                //     deactivation frame that same child.
                //     One such frame is tabbing .x -> .y inside ONE group, where
                //     those two branches disagree (the live-ActiveId one wins and
                //     forwards .y while .x deactivates); any ActiveId handoff
                //     between siblings of one group does the same. But such a
                //     frame is also an
                //     activation: BeginGestureIfActivated has already committed .x's
                //     gesture and parked .y here, so this compares .y against .y.
                //     Unchanged by this guard -- it is what that path already did.
                if (ImGui::GetItemID() != *gestureItem)
                    return;
                // Both no-op on TransactionId::None, so a field that never opened a
                // gesture -- and a gesture that JOINED a gizmo drag -- leaves the
                // stack strictly alone here.
                if (ImGui::IsItemDeactivatedAfterEdit()) stack->Commit(*gestureTxn);
                else if (ImGui::IsItemDeactivated())     stack->Cancel(*gestureTxn);
                else return;
                *gestureTxn  = Arcane::TransactionId::None;   // spent
                *gestureItem = 0;                             // ...and unowned
            }

            // Single-shot edit (asset drop / popup pick / clear): no widget gesture
            // to bracket, so the whole transaction happens in one call. Scoped for
            // the same ownership reason as ApplyImmediate above.
            void ApplyGuidImmediate(const std::string& field, const Astra::FieldInfo& f,
                                    void* instance, const Arcane::Guid& v)
            {
                std::optional<Arcane::ScopedTransaction> txn;
                if (stack)
                {
                    txn.emplace(*stack, "Edit " + typeName + "." + field);
                    ForEachTarget(instance,
                                  [&](Astra::Entity e, void*) { txn->Snapshot(e, descriptor); });
                }
                ForEachTarget(instance, [&](Astra::Entity, void* d)
                              { Arcane::Editor::ApplyGuidEdit(f, d, v); });
            }

            void Visit(const Astra::FieldInfo& f, void* instance) override
            {
                // Group selector: this drive renders exactly one category, so a
                // field belonging to any other one is another drive's business.
                // Leaves from the UNPUSHED scope, like the two skips below.
                if (Arcane::Editor::CategoryOfField(f) != activeCategory)
                    return;

                // Astra::Hidden -- the FIELD ATTRIBUTE, "do not show this
                // property". Nothing to do with Arcane::Hidden, the marker
                // component that makes render submission skip an entity; the
                // names collide, the meanings do not. Returning BEFORE the PushID
                // below is what keeps the ID stack balanced without an early-out
                // inside the scope.
                if (Arcane::Editor::FieldIsAttributeHidden(f))
                    return;

                // Two names on purpose. `label` is prose for the row; `rawName` is
                // the C++ identifier and stays the input to everything the user
                // does not read as prose -- the undo description and the
                // asset-kind heuristic, both of which would change meaning if
                // handed a display name.
                //
                // Built ABOVE the PushID because the search below needs `label` to
                // decide, and its skip has to leave from the UNPUSHED scope for the
                // same reason the Hidden check above does.
                const std::string label = Arcane::Editor::DisplayNameForField(f);
                const std::string rawName(f.name);
                // Both names are searchable: a user who knows the source can type
                // `sortingLayer`, one who does not can type `sorting`. The
                // component-name-hit rule (a match on the header shows every field)
                // lives inside the predicate, not here.
                if (!Arcane::Editor::MatchesInspectorFilter(componentDisplayName, label,
                                                            rawName, query))
                    return;

                ImGui::PushID(static_cast<int>(f.nameHash));
                // Astra::ReadOnly -- the field is still SHOWN, it just cannot be
                // edited. Distinct from FieldKind::ReadOnly below, which means
                // "this panel has no widget for that type"; a field can be
                // either, both, or neither, and the two dim different halves of
                // the row: this one disables the value WIDGET, that one has no
                // widget to disable and greys its value TEXT. Both grey the
                // label, via the flag handed to FieldLabelCell.
                const bool readOnly = Arcane::Editor::FieldIsReadOnly(f);
                // Classified once, above the switch: the label cell needs the
                // answer before the switch that used to ask for it.
                const Arcane::Editor::FieldKind kind = Arcane::Editor::ClassifyField(f);
                // Every widget below draws with its visible label HIDDEN -- the
                // display name is its own item in column 0 now, so a widget
                // still drawing its own would double it. "##", not "###":
                // "##" hides the text while leaving the id seeded by the rest of
                // the string, "###" would reseed the hash (ImHashStr,
                // imgui.cpp:2557). rawName, not label, so the id follows the C++
                // identifier rather than prose that a display-name tweak moves.
                const std::string widgetId = "##" + rawName;
                // The label cell OPENS THE ROW, so it has to run before anything
                // in the value cell -- and deliberately before BeginDisabled: a
                // disabled scope also multiplies alpha (imgui.cpp:8899-8900),
                // which on top of the grey below would dim the name twice.
                const bool labelHovered =
                    FieldLabelCell(label, readOnly || kind == Arcane::Editor::FieldKind::ReadOnly);
                if (readOnly)
                    ImGui::BeginDisabled();

                // Does this row want the tooltip below? An arm whose LAST item is
                // not the thing the user points at has to answer for itself while
                // its own widget is still the last item -- only the asset-ref arm
                // is in that position, and it fills this in. Left unset, the tail
                // asks about the last item, which is that row's own content.
                std::optional<bool> hovered;

                switch (kind)
                {
                    case Arcane::Editor::FieldKind::Bool:
                    {
                        bool v = f.Get<bool>(instance);
                        // ImGui's native tri-state. ImGuiItemFlags_MixedValue is
                        // Checkbox-ONLY in our vendored ImGui (imgui_internal.h:984),
                        // which is why the numeric kinds below blank by hand.
                        // Clicking a mixed checkbox resolves the whole selection to
                        // one value -- ImGui's documented behaviour, and UE's.
                        const bool boolMixed = Multi() && MixedFor(f).Any();
                        if (boolMixed)
                            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
                        bool changed = ImGui::Checkbox(widgetId.c_str(), &v);
                        if (boolMixed)
                            ImGui::PopItemFlag();
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyBoolEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        if (Multi())
                        {
                            const double cur = static_cast<double>(v);
                            double out = cur;
                            if (MultiScalarRow(widgetId.c_str(), 1, &cur, MixedFor(f),
                                               /*integral*/ true, /*axisColors*/ false,
                                               out) >= 0)
                            {
                                // The field's Range binds here too. This row is a
                                // raw text box rather than a drag, so ImGui clamps
                                // nothing for it -- and an out-of-range value typed
                                // into a multi-selection reaches the same
                                // narrowing casts downstream as one typed into the
                                // single-selection drag.
                                if (const auto bound = BindingRange(f))
                                    out = std::clamp(out, bound->first, bound->second);
                                // Then the cast's own domain: converting an
                                // out-of-range double to int32 is UB, and the box
                                // accepts arbitrary digits. Two clamps rather than
                                // one intersected pair because an authored range
                                // lying entirely outside int32 would invert the
                                // intersection and break std::clamp's lo <= hi
                                // precondition.
                                const double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
                                const double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
                                const int32_t iv = static_cast<int32_t>(std::clamp(out, lo, hi));
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { Arcane::Editor::ApplyIntEdit(f, d, iv); });
                            }
                            break;
                        }
                        bool changed = RangedDragInt(f, widgetId.c_str(), &v);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyIntEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        if (Multi())
                        {
                            const double cur = static_cast<double>(v);
                            double out = cur;
                            if (MultiScalarRow(widgetId.c_str(), 1, &cur, MixedFor(f),
                                               /*integral*/ false, /*axisColors*/ false,
                                               out) >= 0)
                            {
                                // Same rule as the Int32 row above and as the drag
                                // one branch down: a Range bounds the typed value,
                                // because nothing in a plain text box does.
                                if (const auto bound = BindingRange(f))
                                    out = std::clamp(out, bound->first, bound->second);
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { Arcane::Editor::ApplyFloatEdit(f, d, fv); });
                            }
                            break;
                        }
                        bool changed = RangedDragFloat(f, widgetId.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyFloatEdit(f, d, v); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        if (Multi())
                        {
                            const double cur[2]{ v.x, v.y };
                            double out = 0.0;
                            // Only the COMMITTED component is written, so typing
                            // into one blank axis leaves the others alone on every
                            // target -- the point of per-axis mixed values.
                            const int c = MultiScalarRow(widgetId.c_str(), 2, cur, MixedFor(f),
                                                         /*integral*/ false, /*axisColors*/ true,
                                                         out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 2, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec2* p = f.GetPtr<glm::vec2>(d)) *p = v; });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        if (Multi())
                        {
                            const double cur[3]{ v.x, v.y, v.z };
                            double out = 0.0;
                            const int c = MultiScalarRow(widgetId.c_str(), 3, cur, MixedFor(f),
                                                         /*integral*/ false, /*axisColors*/ true,
                                                         out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 3, 0.1f);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) *p = v; });
                        break;
                    }
                    case Arcane::Editor::FieldKind::AssetRef:
                    {
                        // Asset-reference (Guid) field: button shows the resolved
                        // mount path (or raw guid), opens a pick popup, and accepts
                        // browser drags; an "x" clears an EDITABLE one (see below).
                        // Kind filter is inferred from the field name
                        // (AssetKindFilterForFieldName).
                        const Arcane::Guid v = f.Get<Arcane::Guid>(instance);
                        // rawName, NOT the display label: this heuristic reads the
                        // C++ identifier, which is what its documented contract
                        // (AssetBrowser.hpp) is written against.
                        const int kindFilter = Arcane::Editor::AssetKindFilterForFieldName(rawName);

                        // Mixed asset refs render BLANK, same rule as the numeric
                        // kinds. This was the one kind the "mixed shows blank" work
                        // missed: ComputeFieldMixed already handles AssetRef (width
                        // 1), but the result was never consulted here, so a
                        // multi-selection showed the PRIMARY's asset as if the whole
                        // selection shared it.
                        const bool refMixed = Multi() && MixedFor(f).Any();

                        std::string display = "(none)";
                        if (refMixed)
                        {
                            display = "--";   // differing values across the selection
                        }
                        else if (v.IsValid())
                        {
                            display = v.ToString();
                            if (project)
                                if (const auto mount = project->Registry().Resolve(v))
                                    display = *mount;
                        }

                        // "###", not "##": only "###" resets the id hash
                        // (ImHashStr, imgui.cpp:2557), so with "##" the button's id
                        // was seeded by `display` -- a MUTABLE string that changes
                        // the moment an asset is picked. The id then changed under
                        // ImGui mid-interaction, dropping the item's state.
                        if (ImGui::Button((display + "###assetref").c_str()))
                            ImGui::OpenPopup("##assetpick");
                        // This row may end on the clear button rather than on the
                        // asset button, so the tail below would ask about THAT and
                        // the identifier tooltip would never appear over the
                        // asset. Asked here, while the button IS the last item.
                        hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                        // `!readOnly` GUARDS THE DROP, and the BeginDisabled wrap
                        // above does NOT: drag-drop acceptance never consults
                        // ImGuiItemFlags_Disabled. BeginDragDropTarget tests only
                        // DragDropActive, the item's ImGuiItemStatusFlags_HoveredRect,
                        // and the hovered window (imgui.cpp:15823-15834), and ItemAdd
                        // stamps HoveredRect from a plain IsMouseHoveringRect
                        // regardless of the disabled flag (imgui.cpp:12070-12071);
                        // AcceptDragDropPayload checks the payload type and the target
                        // rect, nothing about being disabled (imgui.cpp:15860-15905).
                        // So a drag from the Asset Browser onto the disabled
                        // Identity::id row landed here and fanned that asset's Guid
                        // into the durable identity of every selected entity --
                        // AssetKindFilterForFieldName("id") returns -1, which accepts
                        // ANY kind. Read-only means read-only on every path in.
                        if (!readOnly && ImGui::BeginDragDropTarget())
                        {
                            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(Arcane::Editor::kAssetDragType))
                            {
                                const auto* payload = static_cast<const Arcane::Editor::AssetDragPayload*>(p->Data);
                                if (kindFilter < 0 || static_cast<int>(payload->kind) == kindFilter)
                                    ApplyGuidImmediate(rawName, f, instance, payload->guid);
                                // Sprite-asset arc, Task 4: dropping a TEXTURE on a sprite-typed
                                // field auto-mints (or reuses) the wrapping .arcsprite -- Unity's
                                // drop-and-go on UE's explicit-asset storage (sprite-asset spec,
                                // Section 3). The mint runs OUTSIDE ApplyGuidImmediate's
                                // ScopedTransaction (see MintOrReuseSpriteForTexture,
                                // EditorAppProject.cpp), so undo covers only the Guid edit --
                                // undoing this drop does NOT delete the minted file, same as any
                                // created asset outliving a later edit to a field referencing it.
                                else if (kindFilter == static_cast<int>(Arcane::Editor::AssetKind::Sprite)
                                        && payload->kind == Arcane::Editor::AssetKind::Texture
                                        && services && services->mintSpriteForTexture)
                                {
                                    if (const Arcane::Guid minted = services->mintSpriteForTexture(payload->guid);
                                        minted.IsValid())
                                        ApplyGuidImmediate(rawName, f, instance, minted);
                                }
                            }
                            ImGui::EndDragDropTarget();
                        }
                        // Offered when MIXED too: "clear all of them" is a
                        // meaningful action even though the primary may be nil.
                        //
                        // NOT offered on a read-only ref, where it used to render
                        // as a disabled "x" -- an affordance promising an action
                        // that is not merely unavailable right now but can never
                        // exist. Dropping it removes no reachable write: a
                        // disabled item is refused by ItemHoverable
                        // (imgui.cpp:5128-5134), so ButtonBehavior could never
                        // press it. Note this is the OPPOSITE of the drop target
                        // above, which the disabled wrap does not gate at all --
                        // hence its own explicit `!readOnly`.
                        if (!readOnly && (v.IsValid() || refMixed))
                        {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("x##assetclear"))
                                ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                        }

                        if (ImGui::BeginPopup("##assetpick"))
                        {
                            if (!project)
                            {
                                ImGui::TextDisabled("no project open");
                            }
                            else
                            {
                                // Type-to-filter (one popup is open at a time,
                                // so a function-local buffer serves them all).
                                static char s_pickSearch[64] = {};
                                if (ImGui::IsWindowAppearing())
                                {
                                    s_pickSearch[0] = '\0';
                                    ImGui::SetKeyboardFocusHere();
                                }
                                ImGui::InputTextWithHint("##assetsearch", "Search...",
                                                         s_pickSearch, sizeof(s_pickSearch));
                                ImGui::Separator();
                                if (ImGui::Selectable("(none)"))
                                    ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                                for (const Arcane::Editor::AssetEntry& e :
                                     Arcane::Editor::BuildAssetEntries(project->Registry()))
                                {
                                    if (!Arcane::Editor::MatchesFilter(e, kindFilter, s_pickSearch))
                                        continue;
                                    if (ImGui::Selectable((e.name + "##" + e.mountPath).c_str(), e.guid == v))
                                        ApplyGuidImmediate(rawName, f, instance, e.guid);
                                }
                            }
                            ImGui::EndPopup();
                        }
                        break;
                    }
                    case Arcane::Editor::FieldKind::String:
                    {
                        const std::string* live = f.GetPtr<std::string>(instance);
                        // Blank when the selection disagrees -- the same "unset
                        // means multiple differing values" rule the numeric rows
                        // above use.
                        const bool strMixed = Multi() && MixedFor(f).Any();
                        // Reseeded from the component every frame while the box is
                        // idle; once it is active ImGui's own state owns the text
                        // and this local is ignored (see InputTextString), so the
                        // per-frame seed cannot stomp what the user is typing.
                        std::string text = (strMixed || !live) ? std::string() : *live;
                        const std::string shown = text;   // what this frame rendered
                        InputTextString(widgetId.c_str(), &text);
                        // LATCH THE CANCEL REFERENCE AT ACTIVATION. ImGui copies
                        // the buffer it was handed into TextToRevertTo when the
                        // box takes ActiveId (imgui_widgets.cpp:4865-4866) and
                        // writes exactly that back on Escape (:5300-5308) -- so
                        // the reference this guard needs is the ACTIVATION-time
                        // text, and `shown` is that text on this frame (ImGui
                        // writes into the buffer only on a change, and the
                        // activation frame has none yet).
                        //
                        // The per-frame seed this replaced was NOT that reference
                        // whenever the value moved externally mid-edit -- an
                        // Outliner rename committing a frame after this box was
                        // activated on the same entity. Escape then restored the
                        // OLD name into `text`, the guard saw text != seed, and
                        // the documented CANCEL path pushed an undo entry.
                        // Latched, seed == TextToRevertTo by construction, so a
                        // revert is a guaranteed equality no-op no matter what
                        // moved underneath; a real commit still differs and still
                        // writes.
                        //
                        // For a MIXED multi-selection the latched value is the
                        // blank the user saw at activation -- which is right:
                        // Escape restores that same blank, so an untouched mixed
                        // row still commits nothing.
                        if (ImGui::IsItemActivated() && stringSeed && stringSeedItem)
                        {
                            *stringSeed     = shown;
                            *stringSeedItem = ImGui::GetItemID();
                        }
                        // The latch is ONE slot shared by every string row, so
                        // use it only when it names THIS row -- click into row A,
                        // then click row B drawn above it and B re-latches in the
                        // same frame A reports its deactivation. Same ownership
                        // guard as EndGesture's.
                        const bool latched = stringSeed && stringSeedItem
                                          && *stringSeedItem == ImGui::GetItemID();
                        const std::string& seed = latched ? *stringSeed : shown;
                        // Commit on deactivation, guarded by that equality test --
                        // and the guard IS the cancel path, not a nicety. Escape
                        // also sets value_changed (imgui_widgets.cpp:5300-5309), so
                        // IsItemDeactivatedAfterEdit would report a revert as an
                        // edit; "it deactivated and ended up different from what it
                        // started as" is the honest predicate, and it also makes a
                        // plain click-in-click-out write nothing.
                        //
                        // ApplyImmediate, not a widget gesture: the commit is one
                        // discrete event, and it fans the typed text to every
                        // selected entity as ONE undo step.
                        if (ImGui::IsItemDeactivated() && text != seed)
                            ApplyImmediate(rawName, instance, [&](void* d)
                                           { Arcane::Editor::ApplyStringEdit(f, d, text); });
                        break;
                    }
                    case Arcane::Editor::FieldKind::ReadOnly:
                    default:
                        // A type this panel has no widget for. It keeps the grid's
                        // rhythm rather than breaking out of it as bare mid-list
                        // text: the name is in column 0 (greyed, like every other
                        // uneditable label) and only the word goes here. The C++
                        // type is deliberately absent -- the row's tooltip already
                        // carries the raw identifier, which is what you grep for.
                        //
                        // TextDisabled rather than this arm opening a
                        // BeginDisabled of its own: on a field that is ONLY
                        // unsupported (the common case) it is the same grey the
                        // label cell pushes, so the two halves of a dead row
                        // match. A field that is ALSO Astra::ReadOnly is still
                        // inside that wrap and so does get both treatments --
                        // TextDisabled's colour over the disabled alpha
                        // (imgui.cpp:8899-8900) -- which is accepted: it is the
                        // rarest row in the panel and the extra dimming is not
                        // wrong, merely darker than its label.
                        //
                        // AlignTextToFramePadding for the ROW HEIGHT, not the
                        // baseline (the label cell already handed this cell its
                        // baseline via RowTextBaseline): it is what keeps a
                        // widget-less row as tall as its neighbours instead of
                        // collapsing to a line of text.
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("unsupported");
                        break;
                }
                EndGesture();
                if (readOnly)
                    ImGui::EndDisabled();

                // Every arm but the asset-ref one ends on the row's own content, so
                // asking about the LAST item is asking about the field: ImGui
                // restores g.LastItemData when a window closes (imgui.cpp:8849), so
                // neither the asset-pick popup nor the SetTooltip below can
                // retarget it. The asset-ref arm may end on its clear button
                // instead and has already answered above. Asked after EndGesture
                // for the same reason the gesture is bracketed at all -- nothing
                // may sit between a widget and the item-state reads that close its
                // transaction.
                //
                // ORed with the label cell's own answer: the display name is a
                // separate item in column 0 now, and pointing at it is pointing at
                // the field. (Before the grid, a numeric row's name was INSIDE the
                // widget's rect -- DragScalar builds total_bb from its label,
                // imgui_widgets.cpp:2734, and registers THAT at :2738 -- so the
                // single query covered both.)
                //
                // The raw identifier is always in the tooltip, so a friendly label
                // never costs the ability to grep for the field. ForTooltip adds a
                // stationary+delay gate (style.HoverFlagsForTooltipMouse, imgui.h:1515)
                // so sweeping the cursor down the panel does not flicker a tooltip per
                // row; that default already carries AllowWhenDisabled, so an
                // Astra::ReadOnly row still explains itself on hover.
                if (!hovered.has_value())
                    hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
                if (labelHovered || *hovered)
                {
                    const std::string_view tip = Arcane::Editor::TooltipOfField(f);
                    if (tip.empty())
                        ImGui::SetTooltip("%s", rawName.c_str());
                    else
                        ImGui::SetTooltip("%.*s\n\n%s", static_cast<int>(tip.size()),
                                          tip.data(), rawName.c_str());
                }
                ImGui::PopID();
            }
        };
    }

    void CloseAbandonedGesture(Arcane::CommandStack& undo, InspectorState& state)
    {
        // None also covers the gesture that JOINED a gizmo drag: that drag
        // owns the transaction and closes it on mouse-up.
        if (state.gestureTxn == Arcane::TransactionId::None)
            return;
        // Healthy for exactly as long as the owning widget still holds
        // ActiveId. ImGui hands ActiveId over within ONE frame on a
        // click-through between two fields or into the search box, but the
        // field losing it is still submitted that frame, so its own
        // EndGesture has already run and cleared the token above.
        if (ImGui::GetActiveID() == state.gestureItem)
            return;
        undo.Commit(state.gestureTxn);
        state.gestureTxn  = Arcane::TransactionId::None;
        state.gestureItem = 0;
    }

    void DrawReflectedComponent(const ReflectedComponentArgs& args)
    {
        ImGuiFieldVisitor visitor;
        visitor.stack      = args.undo;
        visitor.entity     = args.primary;
        visitor.descriptor = args.component.descriptor;
        // TypeMeta::typeName is a std::string_view into a substring of a
        // larger compile-time literal; the member is a std::string because
        // every undo label below is built by concatenating onto it.
        visitor.typeName   = args.component.meta->typeName;
        visitor.project    = args.project;
        visitor.services   = args.services;
        visitor.gestureTxn  = &args.state.gestureTxn;
        visitor.gestureItem = &args.state.gestureItem;
        // Wired even while Play runs (unlike `stack`): these carry
        // the string row's Escape-cancel semantics, which are
        // ImGui's, not the undo stack's.
        visitor.stringSeed     = &args.state.stringEditSeed;
        visitor.stringSeedItem = &args.state.stringEditItem;
        visitor.registry   = &args.registry;
        visitor.selection  = args.selection;
        visitor.componentDisplayName = args.componentDisplayName;
        visitor.query                = args.filterQuery;
        // Empty for the uncategorised pass, which is what the visitor's own
        // group selector compares every field's category against.
        visitor.activeCategory       = args.activeCategory;

        args.component.descriptor->visitFields(args.component.data, visitor);
    }
}
