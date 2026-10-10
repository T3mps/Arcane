#include "Panels/InspectorView.hpp"

#include "Panels/AssetPanelModel.hpp"    // AssetKind/AssetKindFilterForFieldName/MaterialSurfaceFilterForComponent (the cell's kind + surface filters)
#include "Panels/AssetReferenceField.hpp"   // the shared asset-reference cell (spec 2026-09-30 s4.2)
#include "Widgets/ColorPickerPopup.hpp"
#include "Scene/EditGesture.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"        // ICON_LC_PLUS -- the vector row's [+]
#include "Panels/InspectorFields.hpp"
#include "Panels/InspectorMeta.hpp"
#include "Settings/InspectorSettings.hpp"   // editor.inspector.dragSpeed / rotationDragSpeedDeg (settings S6-37)

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Guid.hpp>

#include <Astra/Reflection/FieldVisitor.hpp>

#include <glm/glm.hpp>
#include <imgui.h>
#include <imgui_internal.h>   // ImGuiItemFlags_MixedValue (:984 -- the tri-state
                              // checkbox's flag; PushItemFlag itself is public,
                              // imgui.h:546). The abandoned-gesture check that
                              // also needed GetActiveID from here moved to
                              // EditGesture (ScopeGuard), which pairs its own.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // editor.inspector.dragSpeed: a float / vector field's per-pixel drag
        // step when the field declares no Range step of its own.
        [[nodiscard]] float FieldDragSpeed() { return Arcane::Settings<InspectorSettings>().dragSpeed; }

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

        // FieldInfo-taking convenience over the reflection-free drag cores in
        // EditorWidgets (they take a RESOLVED std::optional<Astra::Range>, so
        // they are range-aware; what they do not know is Astra::FieldInfo):
        // resolving a field to its Astra::Range is reflection's
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
            // The asset-reference cell's services ride InspectorServices::
            // assetRefs. A texture drop on a sprite field mints the .arcsprite
            // outside ApplyGuidImmediate's transaction; in Play (stack null)
            // the Guid edit lands with no undo step, like every AssetRef edit.
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
            // The in-flight gesture's cross-frame state -- the ownership slots
            // (transaction token + owning item id) and the string row's
            // activation-time cancel seed -- owned by the panel's persistent
            // InspectorState. The visitor is rebuilt every frame; the gesture it
            // brackets is not. NEVER NULL: DrawReflectedComponent wires it
            // unconditionally, and unlike `stack` it is not gated on edit mode --
            // the string seed carries ImGui's Escape-cancel semantics, not the
            // undo stack's, so it matters while Play runs too. The bracket itself
            // still no-ops during Play, on `stack` being null.
            EditGesture::GestureState*        gesture = nullptr;
            // The colour a picker popup was opened on, for its Old/New pair.
            // Same lifetime, same owner, same shape as `gesture` above --
            // deliberately, so there is one rule to remember rather than two.
            glm::vec4*                        originalColor = nullptr;
            // Per-field Euler-view cache for Quat rows (F1 Task 2). Same
            // lifetime/ownership reason as `gesture`/`originalColor` --
            // NEVER NULL, DrawReflectedComponent wires it unconditionally --
            // but keyed rather than a single shared slot; see InspectorState::
            // quatEulerViews's own comment (EditorPanels.hpp) for why.
            std::unordered_map<std::uint64_t, Arcane::Editor::QuatEulerView>* quatEulerViews = nullptr;

            // The vector editor's test seam -- InspectorState::vectorProbe,
            // null in production (see its comment). Wired by
            // DrawReflectedComponent like every other state pointer here.
            std::unordered_map<std::string, glm::vec2>* probe = nullptr;

            // Record the centre of the item JUST SUBMITTED under `key`. One
            // null-check per control when the seam is unwired.
            void RecordProbe(const std::string& key)
            {
                if (!probe)
                    return;
                const ImVec2 lo = ImGui::GetItemRectMin();
                const ImVec2 hi = ImGui::GetItemRectMax();
                (*probe)[key] = glm::vec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            }

            // FieldKind::Vector: set while ONE element's rows are being drawn
            // (a recursive Visit over the element struct's own fields), null
            // otherwise. It does two things. ForEachTarget re-targets the
            // fan-out from each entity's COMPONENT to that entity's element --
            // an element field's offset is element-relative, so the editors'
            // write-backs (ApplyFloatEdit(f, d, v) etc.) need `d` to BE the
            // element. And the undo-label builders prefix the field with the
            // element path (ruling A8): "fixtures[0].radius", never a bare
            // "radius" that could be any element's.
            struct ElementContext
            {
                const Astra::FieldInfo* vectorField;   // the vector field on the component
                std::size_t             index;         // this element
                std::string             prefix;        // "fixtures[0]."
            };
            const ElementContext* elementCtx = nullptr;

            // The undo-label path for `field` in the current context.
            [[nodiscard]] std::string LabelPath(const std::string& field) const
            {
                return elementCtx ? elementCtx->prefix + field : field;
            }

            // One deferred list mutation for the Vector arm (ruling A7):
            // recorded by a button while the elements are being walked, applied
            // after the walk -- the walk holds vectorElement pointers that a
            // resize under it would invalidate.
            struct PendingListOp
            {
                enum Kind { Insert, Erase, Swap } kind;
                std::size_t a;   // Insert: position; Erase: index; Swap: first index
                std::size_t b;   // Swap: second index
            };

            // ONE immediate command per list op -- snapshot, mutate, snapshot,
            // push -- the AssetRef pick's exact shape (spec s7.3). The verb
            // is the undo label's tail: "Edit Arcane::Physics2D::Collider.fixtures.add".
            void ApplyListOp(const std::string& rawName, const Astra::FieldInfo& f, void* instance,
                             const PendingListOp& op)
            {
                const char* verb = op.kind == PendingListOp::Insert ? "add"
                                 : op.kind == PendingListOp::Erase  ? "remove"
                                 :                                    "move";
                ApplyImmediate(rawName + "." + verb, instance, [&](void* d)
                {
                    switch (op.kind)
                    {
                        case PendingListOp::Insert: Arcane::Editor::ApplyVectorInsert(f, d, op.a);       break;
                        case PendingListOp::Erase:  Arcane::Editor::ApplyVectorErase (f, d, op.a);       break;
                        case PendingListOp::Swap:   Arcane::Editor::ApplyVectorSwap  (f, d, op.a, op.b); break;
                    }
                });
            }

            // One element's header row: a tree node in the label column (the
            // element index; DefaultOpen, so a fresh list shows its fields
            // without a click), the three list buttons in the value column.
            // Returns whether the node is open -- the caller draws the element's
            // field rows and TreePops. The "###" id suffix keeps the node's id
            // off its visible text, so the open state survives whatever a later
            // preview might add to the label.
            [[nodiscard]] bool ElementHeaderRow(std::size_t i, std::size_t n, const std::string& rawName,
                                                std::optional<PendingListOp>& pending)
            {
                const std::string elementKey = rawName + "[" + std::to_string(i) + "]";
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                const std::string nodeLabel = "[" + std::to_string(i) + "]###element";
                const bool open = ImGui::TreeNodeEx(nodeLabel.c_str(),
                                                    ImGuiTreeNodeFlags_DefaultOpen |
                                                    ImGuiTreeNodeFlags_SpanAvailWidth);
                ImGui::TableSetColumnIndex(1);
                if (ImGui::SmallButton(ICON_LC_TRASH_2 "##remove"))
                    pending = PendingListOp{ PendingListOp::Erase, i, 0 };
                RecordProbe(elementKey + ".remove");
                ImGui::SameLine();
                // The end buttons are DISABLED rather than hidden: the row keeps
                // its shape, and a disabled button is a target that does nothing
                // -- which the drive pins.
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton(ICON_LC_ARROW_UP "##up"))
                    pending = PendingListOp{ PendingListOp::Swap, i, i - 1 };
                ImGui::EndDisabled();
                RecordProbe(elementKey + ".up");
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 >= n);
                if (ImGui::SmallButton(ICON_LC_ARROW_DOWN "##down"))
                    pending = PendingListOp{ PendingListOp::Swap, i, i + 1 };
                ImGui::EndDisabled();
                RecordProbe(elementKey + ".down");
                return open;
            }

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
            //
            // Every target is MARKED after fn (spec 2026-09-11 s6.4): GetComponentByHash
            // stamps nothing, so without this an Inspector edit is invisible to the
            // Edit-mode Changed<Transform> propagation (EditModeSchedule). Deliberate
            // OVER-MARK: this same fan-out also serves the gesture-begin SNAPSHOT
            // pass (BeginGestureIfActivated's SnapshotComponent lambda, and the
            // multi-select seed reads), which is a READ -- so a widget activation
            // that changes nothing still marks its component once. One spurious
            // recompose per click on the selected entities, never a missed edit;
            // a false positive is the safe direction here (TransformSystems.hpp).
            template<typename Fn>
            void ForEachTarget(void* primaryInstance, Fn&& fn)
            {
                if (!registry || selection.empty())
                {
                    // primaryInstance is already the element when an element
                    // row is drawing (Visit was handed the element pointer).
                    fn(entity, primaryInstance);
                    if (registry && descriptor)
                        (void)registry->Modified(entity, descriptor->id);
                    return;
                }
                for (Astra::Entity e : selection)
                    if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                    {
                        // Element context (FieldKind::Vector): the fan-out
                        // hands fn the COMPONENT, but an element field's
                        // offset is relative to the ELEMENT -- re-derive this
                        // target's element from its own component through the
                        // vector field's accessor. A target whose list is
                        // shorter than the primary's has no such element and
                        // is skipped; unreachable today (ruling A2: elements
                        // draw for a single selection only), kept as the
                        // correct answer if that ever widens.
                        void* target = data;
                        if (elementCtx)
                        {
                            target = elementCtx->vectorField->vectorElement(data, elementCtx->index);
                            if (!target)
                                continue;
                        }
                        fn(e, target);
                        (void)registry->Modified(e, descriptor->id);
                    }
            }

            // Open this row's gesture if its widget activated this frame. A thin
            // adapter over EditGesture, which owns the stale-close, the owner
            // parking, and the Play-mode no-op; the NAME stays because every arm
            // of Visit below calls it right after submitting its widget.
            void BeginGestureIfActivated(const std::string& field, void* primaryInstance)
            {
                EditGesture::BeginOnActivate(stack, *gesture,
                    [&] { return "Edit " + typeName + "." + LabelPath(field); },
                    [&]
                    {
                        // Snapshot-style: one Begin + N snapshots + one Commit = one
                        // undo step for the whole fan-out (CommandStack dedupes per
                        // (entity, descriptor)). Runs AFTER Begin, inside the
                        // transaction, so a gesture that JOINED a live gizmo drag
                        // still lands its snapshots in that drag's transaction.
                        // Nothing to build for later, so the pending-commit slot
                        // stays empty -- the before-state rides the transaction.
                        ForEachTarget(primaryInstance,
                                      [&](Astra::Entity e, void*) { stack->SnapshotComponent(e, descriptor); });
                        return std::function<void()>{};
                    });
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
                    txn.emplace(*stack, "Edit " + typeName + "." + LabelPath(field));
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

            // Close this row's gesture if THIS row is the one that opened it and
            // its widget deactivated. Safe to call for every row every frame --
            // the ownership guard and the Commit-vs-Cancel verdict are
            // EditGesture's (EvaluateEnd). Why the GROUP rows above (
            // AxisDragFloatN, MultiScalarRow) are safe to close through the
            // same call is spelled out by EndOnDeactivate's own comment
            // (EditGesture.hpp), whose "groups" bullet reads that EndGroup
            // re-points LastItemData.ID at the group's live ActiveId, else at
            // the child that deactivated inside it -- so both the activation
            // and the deactivation frame yield the child that held ActiveId.
            // Name kept for the same reason BeginGestureIfActivated's is.
            void EndGesture()
            {
                EditGesture::EndOnDeactivate(stack, *gesture);
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
                    txn.emplace(*stack, "Edit " + typeName + "." + LabelPath(field));
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
                //
                // Element rows (FieldKind::Vector) skip the group selector and
                // the search below (ruling A4): the VECTOR field's own row
                // already passed both, and an element's fields have no
                // category of their own to be drawn under elsewhere. They keep
                // the Hidden check -- an element can hide a field like anyone.
                if (!elementCtx && Arcane::Editor::CategoryOfField(f) != activeCategory)
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
                if (!elementCtx && !Arcane::Editor::MatchesInspectorFilter(componentDisplayName, label,
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
                bool labelTruncated = false;
                const bool labelHovered =
                    FieldLabelCell(label, readOnly || kind == Arcane::Editor::FieldKind::ReadOnly, &labelTruncated);
                if (readOnly)
                    ImGui::BeginDisabled();

                // Does this row want the tooltip below? An arm whose LAST item is
                // not the thing the user points at has to answer for itself while
                // its own widget is still the last item -- only the asset-ref arm
                // is in that position, and it fills this in. Left unset, the tail
                // asks about the last item, which is that row's own content.
                std::optional<bool> hovered;
                // A full value the row's widget could only show TRUNCATED (a
                // long mount path, a raw guid) -- composed into the row
                // tooltip below, so hovering the shortened text reveals the
                // whole thing. SetTooltip is last-wins per frame, which is
                // why this rides the one tail instead of a second call.
                std::optional<std::string> tooltipValue;

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
                    case Arcane::Editor::FieldKind::UInt32:
                    {
                        std::uint32_t v = f.Get<std::uint32_t>(instance);
                        if (Multi())
                        {
                            const double cur = static_cast<double>(v);
                            double out = cur;
                            if (MultiScalarRow(widgetId.c_str(), 1, &cur, MixedFor(f),
                                               /*integral*/ true, /*axisColors*/ false,
                                               out) >= 0)
                            {
                                // Same two-clamp rule as the Int32 row: the
                                // authored Range first, then the cast's own
                                // domain, kept separate so a Range lying
                                // outside u32 cannot invert std::clamp's
                                // precondition.
                                if (const auto bound = BindingRange(f))
                                    out = std::clamp(out, bound->first, bound->second);
                                const double hi =
                                    static_cast<double>(std::numeric_limits<std::uint32_t>::max());
                                const std::uint32_t uv =
                                    static_cast<std::uint32_t>(std::clamp(out, 0.0, hi));
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { Arcane::Editor::ApplyUIntEdit(f, d, uv); });
                            }
                            break;
                        }
                        // DragScalar directly rather than RangedDragInt: that
                        // helper is int-typed, and a u32 above INT_MAX would
                        // wrap through it. Range bounds clamp into the unsigned
                        // domain before use.
                        const auto bound = BindingRange(f);
                        const double uMax =
                            static_cast<double>(std::numeric_limits<std::uint32_t>::max());
                        std::uint32_t lo = 0;
                        std::uint32_t hi = std::numeric_limits<std::uint32_t>::max();
                        if (bound)
                        {
                            lo = static_cast<std::uint32_t>(std::clamp(bound->first, 0.0, uMax));
                            hi = static_cast<std::uint32_t>(std::clamp(bound->second, 0.0, uMax));
                        }
                        bool changed = ImGui::DragScalar(widgetId.c_str(), ImGuiDataType_U32, &v,
                                                         1.0f, bound ? &lo : nullptr,
                                                         bound ? &hi : nullptr);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { Arcane::Editor::ApplyUIntEdit(f, d, v); });
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
                        bool changed = RangedDragFloat(f, widgetId.c_str(), &v, FieldDragSpeed());
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
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 2, FieldDragSpeed());
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
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 3, FieldDragSpeed());
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec3* p = f.GetPtr<glm::vec3>(d)) *p = v; });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vec4:
                    {
                        glm::vec4 v = f.Get<glm::vec4>(instance);
                        // The name heuristic (IsColorFieldName): "tint"/"color"
                        // fields edit as a colour, everything else as 4 floats.
                        const bool isColor = Arcane::Editor::IsColorFieldName(rawName);
                        if (Multi())
                        {
                            // LINEAR floats, shown directly -- matching the
                            // single-select row's own boxes (this arm has no
                            // swatch and no popup; multi-select gets no picker).
                            // The 0-255 sRGB display this replaces made one
                            // stored value read two ways depending on how many
                            // entities were selected; showing storage directly
                            // cannot drift.
                            const double cur[4]{ v.x, v.y, v.z, v.w };
                            double out = 0.0;
                            // Axis strips only on the NON-colour flavour: XYZW
                            // colours under RGBA channels would label the values as
                            // something they are not.
                            const int c = MultiScalarRow(widgetId.c_str(), 4, cur, MixedFor(f),
                                                         /*integral*/ false,
                                                         /*axisColors*/ !isColor, out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
                        if (isColor)
                        {
                            // The shared colour VALUE cell (Widgets/ColorPickerPopup.hpp,
                            // node-page s4.1(c)): the sRGB-ENCODED swatch first (LEFT of
                            // the boxes since the migration, drafting pick 9.28), its
                            // popup, then the four LINEAR boxes LAST -- so the boxes are
                            // LastItemData here and keep the activation gesture they always
                            // had. They keep `widgetId` as their label: box ids unchanged.
                            // The Old/New latch stays in InspectorState (`originalColor`):
                            // the visitor is rebuilt per frame (InspectorView.cpp:143).
                            const ImVec2 cell = ImGui::GetCursorScreenPos();   // swatch top-left (probe seam)
                            const ColorValueResult colorCell =
                                ColorValue(widgetId.c_str(), &v.x, &originalColor->x, /*hdr*/ false);
                            const bool changed = colorCell.changed;
                            BeginGestureIfActivated(rawName, instance);
                            if (probe)
                            {
                                RecordProbe(rawName + "#boxes");
                                const float fh = ImGui::GetFrameHeight();
                                (*probe)[rawName + "#swatch"] = glm::vec2(cell.x + fh * 0.5f, cell.y + fh * 0.5f);
                            }

                            // POPUP-lifetime gesture, NOT the activation pair: ImGui
                            // only lends its ActiveId to popups it opened itself, and
                            // this one is ours (EditGesture.hpp, ShouldClosePopup).
                            // The popup body already ran inside ColorValue, but its
                            // writes land below (ForEachTarget), AFTER this pair, so the
                            // open frame's snapshots still precede every write.
                            // onOpened is BeginGestureIfActivated's fan-out verbatim:
                            // one Begin + N SnapshotComponent + one Commit = one step.
                            EditGesture::BeginOnPopupOpen(
                                stack, *gesture, colorCell.popupId,
                                [&] { return "Edit " + typeName + "." + rawName; },
                                [&]
                                {
                                    ForEachTarget(instance,
                                                  [&](Astra::Entity e, void*)
                                                  { stack->SnapshotComponent(e, descriptor); });
                                    return std::function<void()>{};
                                });
                            EditGesture::EndOnPopupClose(stack, *gesture, colorCell.popupId);

                            if (changed)
                                ForEachTarget(instance, [&](Astra::Entity, void* d)
                                              { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) *p = v; });
                            break;
                        }
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 4, FieldDragSpeed());
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) *p = v; });
                        break;
                    }
                    case Arcane::Editor::FieldKind::Quat:
                    {
                        // EULER IS A VIEW, THE QUATERNION IS THE STORAGE. All the
                        // logic that makes that true (no re-derivation on a plain
                        // display frame, one-axis edits leaving the other two
                        // untouched, gimbal-adjacent safety) lives in InspectorFields.
                        // cpp's pure Quat helpers and is driven directly by
                        // EditorInspectorQuatTest.cpp; this arm is the thin ImGui
                        // skin over them, same split as every other field kind.
                        const glm::quat live = f.Get<glm::quat>(instance);

                        // Keyed per (ENTITY, component, field): the drawn entity
                        // (`entity`, the primary of the selection THIS page draws)
                        // + the component hash + the field nameHash. Several
                        // Inspector instances can be on screen at once -- a
                        // follower on entity A and a pinned instance on entity B
                        // -- all drawing through the ONE InspectorState, so a
                        // per-field key would let each draw overwrite the other's
                        // view and re-derive Euler every frame (a pitch drag past
                        // +-90 deg would flip). The same entity shown in two
                        // instances correctly shares one view.
                        const std::uint64_t fieldKey =
                            descriptor->hash ^ (f.nameHash * 0x9E3779B97F4A7C15ULL);
                        const std::uint64_t quatKey =
                            fieldKey ^ (static_cast<std::uint64_t>(entity.GetValue()) * 0xC2B2AE3D27D4EB4FULL
                                        + 0x165667B19E3779F9ULL + (fieldKey << 6) + (fieldKey >> 2));
                        Arcane::Editor::QuatEulerView& view = (*quatEulerViews)[quatKey];

                        // Degrees unless the field explicitly asks for radians --
                        // Astra::AngleFormat's own default (AngleUnitForField).
                        const bool degrees = Arcane::Editor::AngleUnitForField(f)
                                            == Astra::AngleFormat::Unit::Degrees;

                        if (Multi())
                        {
                            // No cross-frame cache for multi-select: MultiScalarRow's
                            // boxes are freshly re-seeded from the PRIMARY's live
                            // value every frame (same as every other multi row),
                            // never dragged -- there is no continuous gesture here to
                            // protect from re-derivation jitter, only a discrete text
                            // commit, so QuatWithEulerAxisRadians's fresh decompose/
                            // recompose per TARGET entity is the right tool.
                            glm::vec3 primaryEuler = Arcane::Editor::QuatToEulerRadians(live);
                            if (degrees) primaryEuler = glm::degrees(primaryEuler);
                            const double cur[3]{ primaryEuler.x, primaryEuler.y, primaryEuler.z };

                            // A quaternion's raw storage components have no
                            // independent per-axis Euler meaning across DIFFERENT
                            // rotations, so mixed is all-or-nothing: any raw-
                            // component disagreement blanks every Euler box, not
                            // just the ones that happen to differ bitwise.
                            Arcane::Editor::FieldMixedMask mask;
                            if (MixedFor(f).Any())
                                mask.bits = 0b111u;

                            double out = 0.0;
                            const int c = MultiScalarRow(widgetId.c_str(), 3, cur, mask,
                                                         /*integral*/ false,
                                                         /*axisColors*/ true, out);
                            if (c >= 0)
                            {
                                const float radians = degrees
                                    ? glm::radians(static_cast<float>(out))
                                    : static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                {
                                    if (glm::quat* p = f.GetPtr<glm::quat>(d))
                                        *p = Arcane::Editor::QuatWithEulerAxisRadians(*p, c, radians);
                                });
                            }
                            break;
                        }

                        // Cache the triple in the DISPLAY unit itself -- no
                        // conversion here, not even a lossless one. Directive
                        // (2026-08-22): radians internally, degrees in the
                        // editor, no live conversion; degrees are purely a
                        // display artifact. The old code called the radian
                        // SyncQuatEulerView/ApplyQuatEulerEdit pair
                        // unconditionally and converted with glm::degrees()/
                        // glm::radians() around them, which sent the two axes
                        // the user did NOT touch through a degrees<->radians
                        // round trip on every edit. SyncQuatEulerViewDegrees/
                        // ApplyQuatEulerEditDegrees below cache degrees
                        // directly, so an untouched axis is the literal float
                        // last shown -- see InspectorFields.cpp/.hpp and
                        // EditorInspectorQuatTest.cpp's "no unit round trip"
                        // case.
                        glm::vec3 v = degrees
                            ? Arcane::Editor::SyncQuatEulerViewDegrees(view, live)
                            : Arcane::Editor::SyncQuatEulerView(view, live);

                        // A degree drag and a radian drag want visibly different
                        // step sizes for the same "feels like this much rotation"
                        // -- editor.inspector.rotationDragSpeedDeg (0.5
                        // degrees/pixel) matches the Vec3/Vec4 rows' 0.1
                        // units/pixel feel; the radian fallback follows it at
                        // the pre-sweep 0.5 deg : 0.01 rad ratio (x 0.02, exact
                        // at the default -- a rounder step than the 0.00873 the
                        // unit conversion would give).
                        const float degSpeed = Arcane::Settings<InspectorSettings>().rotationDragSpeedDeg;
                        bool changed = AxisDragFloatN(widgetId.c_str(), &v.x, 3,
                                                      degrees ? degSpeed : degSpeed * 0.02f);
                        BeginGestureIfActivated(rawName, instance);
                        if (changed)
                        {
                            const glm::quat newQuat = degrees
                                ? Arcane::Editor::ApplyQuatEulerEditDegrees(view, v)
                                : Arcane::Editor::ApplyQuatEulerEdit(view, v);
                            ForEachTarget(instance, [&](Astra::Entity, void* d)
                                          { if (glm::quat* p = f.GetPtr<glm::quat>(d)) *p = newQuat; });
                        }
                        break;
                    }
                    case Arcane::Editor::FieldKind::AssetRef:
                    {
                        // The shared asset-reference cell (spec 2026-09-30 s4.2):
                        // thumb + file name (the mount path is the tooltip),
                        // chevron picker, browse-to, clear, whole-cell drop. It
                        // keeps this arm's rules: "--" when mixed, no drop on a
                        // read-only field, an Identity guid never resolved,
                        // texture -> sprite mint. rawName (the C++ identifier)
                        // feeds the kind heuristic; typeName (the OWNING
                        // component) narrows a material field's surface.
                        static const Arcane::Editor::AssetRefServices kNoAssetRefServices{};
                        const int kindFilter = Arcane::Editor::AssetKindFilterForFieldName(rawName);
                        Arcane::Editor::AssetRefArgs refArgs;
                        refArgs.guid = f.Get<Arcane::Guid>(instance);
                        refArgs.kindFilter = kindFilter;
                        refArgs.surfaceFilter = kindFilter == static_cast<int>(Arcane::Editor::AssetKind::Material)
                            ? Arcane::Editor::MaterialSurfaceFilterForComponent(typeName) : -1;
                        refArgs.readOnly = readOnly;
                        refArgs.mixed = Multi() && MixedFor(f).Any();
                        refArgs.allowTextureMint = true;
                        refArgs.identityGuid = Arcane::Editor::IsIdentityGuidFieldName(rawName);
                        refArgs.ownTooltip = false;   // the row's tail composes label, value, prose, identifier
                        const Arcane::Editor::AssetRefServices& refServices =
                            (services && services->assetRefs) ? *services->assetRefs : kNoAssetRefServices;
                        const ImVec2 cellMin = ImGui::GetCursorScreenPos();
                        const float cellW = ImGui::GetContentRegionAvail().x;
                        const Arcane::Editor::AssetRefEdit edit =
                            Arcane::Editor::AssetReferenceValue("##assetref", refArgs, refServices);
                        if (edit.op == Arcane::Editor::AssetRefEdit::Op::Set)
                            ApplyGuidImmediate(rawName, f, instance, edit.guid);
                        else if (edit.op == Arcane::Editor::AssetRefEdit::Op::Clear)
                            ApplyGuidImmediate(rawName, f, instance, Arcane::Guid::Nil());
                        // The cell may end on a button: it answered for its name.
                        hovered = edit.hovered;
                        if (!edit.fullText.empty())
                            tooltipValue = edit.fullText;
                        if (probe)   // TEST SEAM (null in production): the cell's centre
                            (*probe)[(elementCtx ? elementCtx->prefix : std::string{}) + rawName + ".cell"] =
                                glm::vec2(cellMin.x + cellW * 0.5f, cellMin.y + ImGui::GetFrameHeight() * 0.5f);
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
                        if (ImGui::IsItemActivated())
                        {
                            gesture->stringSeed     = shown;
                            gesture->stringSeedItem = ImGui::GetItemID();
                        }
                        // The latch is ONE slot shared by every string row, so
                        // use it only when it names THIS row -- click into row A,
                        // then click row B drawn above it and B re-latches in the
                        // same frame A reports its deactivation. Same ownership
                        // guard as EndGesture's.
                        const bool latched = gesture->stringSeedItem == ImGui::GetItemID();
                        const std::string& seed = latched ? gesture->stringSeed : shown;
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
                    case Arcane::Editor::FieldKind::Enum:
                    {
                        // Classify guarantees registered metadata; the guard is
                        // a belt against a meta table torn down mid-frame.
                        const Astra::EnumInfo* info = Arcane::Editor::EnumInfoOf(f);
                        if (!info)
                        {
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextDisabled("unsupported");
                            break;
                        }
                        const std::int64_t cur = Arcane::Editor::ReadEnumValue(f, instance);
                        const bool enumMixed = Multi() && MixedFor(f).Any();
                        // Mixed renders the same blank-meaning "--" the numeric
                        // rows use; an out-of-roster value shows its NUMBER
                        // rather than pretending to be a named one.
                        std::string preview = "--";
                        if (!enumMixed)
                        {
                            if (const auto n = info->GetDisplayName(cur))
                                preview = std::string(*n);
                            else
                                preview = std::to_string(cur);
                        }
                        if (ImGui::BeginCombo(widgetId.c_str(), preview.c_str()))
                        {
                            for (const Astra::EnumValue& ev : info->values)
                            {
                                const bool selected = !enumMixed && ev.value == cur;
                                // Id by VALUE: two entries may share a display
                                // name, and the value is the identity.
                                const std::string item =
                                    std::string(ev.displayName.empty() ? ev.name : ev.displayName)
                                    + "##" + std::to_string(ev.value);
                                // ApplyImmediate: a pick is one discrete event
                                // fanned to the selection as ONE undo step --
                                // the AssetRef popup's exact semantics.
                                if (ImGui::Selectable(item.c_str(), selected))
                                    ApplyImmediate(rawName, instance, [&](void* d)
                                                   { Arcane::Editor::ApplyEnumEdit(f, d, ev.value); });
                                if (!ev.description.empty()
                                    && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                                    ImGui::SetTooltip("%s", std::string(ev.description).c_str());
                            }
                            ImGui::EndCombo();
                        }
                        break;
                    }
                    case Arcane::Editor::FieldKind::Vector:
                    {
                        // The HEADER row: the count + [+] in the value cell (the
                        // field's display name is in the label cell, opened
                        // above like every row). Element rows follow it (the
                        // walk below); an insert is DEFERRED past them (ruling
                        // A7) and bracketed as ONE immediate command.
                        const std::size_t n = Arcane::Editor::VectorSize(f, instance);
                        ImGui::AlignTextToFramePadding();
                        ImGui::Text("%zu element%s", n, n == 1 ? "" : "s");
                        if (Multi())
                        {
                            // A multi-selection draws the count and nothing
                            // else (ruling A2): the fan-out reads a field at
                            // ITS offset from each entity's COMPONENT
                            // (ComputeFieldMixed, the multi rows' seeds) --
                            // wrong for an element, whose offset is element-
                            // relative -- and the lists can differ in length
                            // across the selection. UE's Details panel shows
                            // "Multiple Values" for the array here; same
                            // refusal, said in the tooltip.
                            tooltipValue = "multi-selection: edit one entity at a time";
                            hovered = false;   // the count text is not a hover target
                            break;
                        }
                        ImGui::SameLine();
                        std::optional<PendingListOp> pending;
                        if (ImGui::SmallButton(ICON_LC_PLUS "##add"))
                            pending = PendingListOp{ PendingListOp::Insert, n, 0 };
                        RecordProbe(rawName + ".add");

                        // The element walk. Each block is a tree node row plus
                        // the element struct's OWN reflected fields, drawn by
                        // this same visitor one level down: Visit is handed the
                        // ELEMENT pointer, and elementCtx re-targets the fan-out
                        // and the undo labels (Step 3-5). Nothing here resizes
                        // the list -- `pending` is applied after the walk.
                        const Astra::TypeMeta* em = Astra::GetMeta(f.elementTypeHash);   // non-null: Classify vetted it
                        for (std::size_t i = 0; i < n && em; ++i)
                        {
                            ImGui::PushID(static_cast<int>(i));
                            const bool open = ElementHeaderRow(i, n, rawName, pending);
                            if (open)
                            {
                                void* elem = f.vectorElement(instance, i);
                                const ElementContext ctx{ &f, i, rawName + "[" + std::to_string(i) + "]." };
                                const ElementContext* saved = elementCtx;
                                elementCtx = &ctx;
                                for (const Astra::FieldInfo& nf : em->fields)
                                {
                                    // The same skip Astra's VisitFields applies to a
                                    // component's own fields one level up, and the
                                    // JSON bridge applies on the element path: a
                                    // Serializable(false) element field is not drawn,
                                    // because an edit to it could never be saved.
                                    if (!nf.IsSerializable())
                                        continue;
                                    Visit(nf, elem);
                                }
                                elementCtx = saved;
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }

                        if (pending)
                            ApplyListOp(rawName, f, instance, *pending);
                        // The last item is a list control or an element row,
                        // never this row's own content: the header tooltip
                        // follows the label alone.
                        hovered = false;
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
                // Element rows are mouse targets for the device-less drive:
                // every arm but the asset-ref one ends on the row's own
                // widget, so this is the widget's rect (recorded before
                // EndGesture reads item state, which a rect read leaves alone).
                if (elementCtx)
                    RecordProbe(elementCtx->prefix + rawName);
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
                    // Order: the full LABEL first when its cell cut it (node-page
                    // s4.1(e); SetTooltip is last-wins, so this tail replaces
                    // FieldLabelCell's own label tooltip), then the full truncated
                    // value, the authored prose, and the greppable identifier last.
                    const std::string_view tip = Arcane::Editor::TooltipOfField(f);
                    std::string text;
                    if (labelTruncated)
                    {
                        text += label;
                        text += "\n\n";
                    }
                    if (tooltipValue)
                    {
                        text += *tooltipValue;
                        text += "\n\n";
                    }
                    if (!tip.empty())
                    {
                        text += tip;
                        text += "\n\n";
                    }
                    text += rawName;
                    ImGui::SetTooltip("%s", text.c_str());
                }
                ImGui::PopID();
            }
        };
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
        visitor.services   = args.services;
        // Wired UNCONDITIONALLY -- also while Play runs, unlike `stack`: the
        // slots must outlive the per-frame visitor, and the string seed carries
        // the Escape-cancel semantics, which are ImGui's and not the undo
        // stack's.
        visitor.gesture    = &args.state.gesture;
        visitor.originalColor = &args.state.colorPopupOriginal;
        visitor.quatEulerViews = &args.state.quatEulerViews;
        visitor.probe          = args.state.vectorProbe;   // null in production
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
