#include "Documents/InputActionsDocumentWidgets.hpp"

#include "Documents/InputActionsJson.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/EditorWidgets.hpp"
#include "Widgets/IconsLucide.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace Arcane::Editor
{
    namespace
    {
        constexpr float kMapsColumnWidth = 180.0f;
        constexpr float kIndent = 16.0f;
        constexpr const char* kDragPayload = "ARC_INPUT_ROW";

        // IdOf / Str / Bool / FindMap: the tolerant draft reads (InputActionsJson.hpp).
        bool IsMapId(const nlohmann::json& draft, const Guid& id) { return FindMap(draft, id) != nullptr; }
        bool IsActionId(const nlohmann::json& draft, const Guid& id)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return false;
            for (const auto& m : draft["actionMaps"])
                if (m.is_object() && m.contains("actions") && m["actions"].is_array())
                    for (const auto& a : m["actions"]) if (IdOf(a) == id) return true;
            return false;
        }
        std::string NameOf(const InputActionsEditorModel& model, const Guid& id)
        {
            const auto* n = model.FindNode(id);
            return n ? Str(*n, "name") : std::string{};
        }
        const char* DeviceIcon(const std::string& device)
        {
            if (device == "Keyboard") return ICON_LC_KEYBOARD;
            if (device == "Mouse")    return ICON_LC_MOUSE;
            if (device == "Gamepad")  return ICON_LC_GAMEPAD_2;
            return ICON_LC_CIRCLE_DOT;
        }
        int SchemeVariant(const std::string& badge) { return badge == "KeyboardMouse" ? 2 : 3; }
        // The expander chevron's cell at the head of an action row (the button
        // plus its spacing). Action rows start their thumb cell after it; every
        // child row indents from it, so a binding sits directly under its
        // action's name.
        float ChevronCell()
        {
            return ImGui::CalcTextSize(ICON_LC_CHEVRON_DOWN).x + ImGui::GetStyle().FramePadding.x * 2.0f
                 + ImGui::GetStyle().ItemSpacing.x;
        }

        void MoveRowMenu(InputActionsEditorModel& model, const Guid& id, std::function<void()>& edit)
        {
            if (ImGui::MenuItem("Move up"))   edit = [&model, id] { (void)model.MoveRow(id, -1); };
            if (ImGui::MenuItem("Move down")) edit = [&model, id] { (void)model.MoveRow(id, 1); };
        }
        // Each column scrolls its OWN selection into view: the maps child draws
        // first, so one shared flag was always eaten by the selected map row and
        // the actions column never scrolled. A map id owns the maps column's
        // flag; every other id (action, binding, part) the actions column's.
        bool& ScrollFlagFor(const InputActionsEditorModel& model, InputActionsDocumentState& state, const Guid& id)
        { return IsMapId(model.Draft(), id) ? state.scrollMapToSelection : state.scrollRowToSelection; }

        // A new map/action opens in a rename box on its (unique, Task 6) name --
        // UE's new-item kick-off. Shared by the toolbar, the maps `+`, the
        // actions-column `+ Action`, F2 in both columns and both columns' Rename
        // menu items.
        void OpenRenameOn(InputActionsEditorModel& model, InputActionsDocumentState& state, const Guid& id)
        {
            state.renameTarget = id;
            state.renameBuf = NameOf(model, id);
            state.renameFocusPending = ScrollFlagFor(model, state, id) = true;
        }

        // The inline rename box, ONE for the maps and actions columns (the caller
        // positions the cursor). Validation runs every frame (blank, duplicate
        // sibling) and shows its reason; Enter with invalid text re-arms the box
        // next frame (ImGui deactivates on Enter; UE stays in edit), focus loss
        // with invalid text cancels. Escape reverts the buffer to its seed BEFORE
        // deactivating, so the Escape check is the cancel path. The commit is
        // trimmed (the model's name rules compare trimmed).
        void DrawRenameBox(InputActionsEditorModel& model, InputActionsDocumentState& state, const Guid& id,
                           const std::string& currentName, std::function<void()>& edit)
        {
            if (bool& scroll = ScrollFlagFor(model, state, id); scroll) { ImGui::SetScrollHereY(); scroll = false; }   // this column's flag only
            if (state.renameFocusPending) { ImGui::SetKeyboardFocusHere(); state.renameFocusPending = false; }
            ImGui::SetNextItemWidth(-FLT_MIN);
            const bool entered = InputTextString("##rename", &state.renameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            const auto reason = InputActionsEditorModel::ValidateName(model.Draft(), id, state.renameBuf);
            if (reason && ImGui::IsItemActive()) ImGui::SetItemTooltip("%s", reason->c_str());
            if (ImGui::IsItemDeactivated())
            {
                const bool cancelled = ImGui::IsKeyPressed(ImGuiKey_Escape);
                if (cancelled || !reason)
                {
                    const std::string trimmed = TrimName(state.renameBuf);
                    if (!cancelled && trimmed != currentName) edit = [&model, id, trimmed] { (void)model.SetField(id, "name", trimmed); };
                    state.renameTarget = {};
                }
                else if (entered) state.renameFocusPending = true;   // keep renameTarget + renameBuf
                else state.renameTarget = {};
            }
        }

        // The guard both column key handlers share; call it INSIDE the column's
        // child (the focus test reads the current window). A rebind capture owns
        // the keyboard: the key that completed (or is feeding) it is consumed
        // there (UE SInputKeySelector's OnPreviewKeyDown rule). No commands
        // mid-drag (UE FUICommandList). Then the Outliner's guard: this column
        // focused, no text box typing, no inline rename live.
        bool ColumnKeysLive(const InputActionsDocumentWidgets::Services& services, const InputActionsDocumentState& state)
        {
            if (services.inputSwallowed && services.inputSwallowed()) return false;
            if (ImGui::GetDragDropPayload() != nullptr) return false;   // the public form; IsDragDropActive is imgui_internal
            // NoPopupHierarchy: a popup opened from a column (a row's context
            // menu advertising "Delete  Del") must NOT count as the column's
            // focus, or Del would delete the SELECTED row, not the right-clicked
            // one. Keys stay inert while any popup owns focus.
            if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_NoPopupHierarchy) || ImGui::GetIO().WantTextInput) return false;
            return !state.renameTarget.IsValid();   // safe: DrawActions/DrawMaps sweep a target whose row is not drawn this frame
        }
    }

    void InputActionsDocumentWidgets::SelectRow(InputActionsEditorModel& model, const InputRow& row)
    {
        switch (row.kind)
        {
        case InputRowKind::Action:          model.SelectAction(row.id); model.SelectBinding({}); break;
        case InputRowKind::Binding:
        case InputRowKind::CompositeHeader: model.SelectAction(row.actionId); model.SelectBinding(row.id); break;
        case InputRowKind::Part:            model.SelectAction(row.actionId); model.SelectBinding(row.bindingId); model.SelectPart(row.id); break;
        case InputRowKind::AddBinding:      break;
        }
    }

    bool InputActionsDocumentWidgets::RowSelected(const InputActionsEditorModel& model, const InputRow& row)
    {
        switch (row.kind)
        {
        case InputRowKind::Action:          return model.SelectedAction() == row.id && !model.SelectedBinding().IsValid();
        case InputRowKind::Binding:
        case InputRowKind::CompositeHeader: return model.SelectedBinding() == row.id && !model.SelectedPart().IsValid();
        case InputRowKind::Part:            return model.SelectedPart() == row.id;
        default:                            return false;
        }
    }

    void InputActionsDocumentWidgets::Draw(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                            const Services& services)
    {
        const bool swallowed = services.inputSwallowed && services.inputSwallowed();
        Edit edit;
        // While a capture is armed the capture row is the only live control
        // (UE SKeySelector: the listening widget holds focus + capture): the
        // toolbar is disabled and neither column takes mouse input, so the
        // completing click retargets nothing and no other row's menu opens.
        // Scrolling pauses for the capture's duration (<= 10 s; Escape ends it).
        ImGui::BeginDisabled(swallowed);
        DrawToolbar(model, state, edit);
        ImGui::EndDisabled();
        ImGui::Separator();
        const ImGuiWindowFlags colFlags = swallowed ? ImGuiWindowFlags_NoInputs : 0;
        ImGui::BeginChild("##input_maps", ImVec2(kMapsColumnWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX, colFlags);
        DrawMaps(model, state, services, edit);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##input_actions", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, colFlags);
        DrawActions(model, state, services, edit);
        ImGui::EndChild();
        if (state.schemePopupPending) { ImGui::OpenPopup("Control schemes##input"); state.schemePopupPending = false; }
        DrawSchemePopup(model, state, edit);
        if (edit) edit();   // AFTER the draw: the draft must not mutate under the row loop
    }

    void InputActionsDocumentWidgets::DrawToolbar(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit)
    {
        const Guid map = model.SelectedMap(), action = model.SelectedAction();
        if (ImGui::Button(ICON_LC_PLUS " Add " ICON_LC_CHEVRON_DOWN)) ImGui::OpenPopup("##input_add");
        if (ImGui::BeginPopup("##input_add"))
        {
            // The model selects the new row and gives it a unique sibling name
            // ("Action Map 2", "Action 3" -- Task 6); the new row opens in rename.
            if (ImGui::MenuItem("Action map")) edit = [&model, &state] { if (model.AddMap()) OpenRenameOn(model, state, model.SelectedMap()); };
            if (ImGui::MenuItem("Action", nullptr, false, map.IsValid())) edit = [&model, &state, map] { if (model.AddAction(map)) OpenRenameOn(model, state, model.SelectedAction()); };
            if (ImGui::MenuItem("Binding", nullptr, false, action.IsValid())) edit = [&model, &state, map, action] { if (model.AddBinding(map, action)) state.scrollRowToSelection = true; };
            if (ImGui::BeginMenu("Composite", action.IsValid()))
            {
                if (ImGui::MenuItem("1D axis"))   edit = [&model, &state, map, action] { if (model.AddComposite(map, action, "1DAxis")) state.scrollRowToSelection = true; };
                if (ImGui::MenuItem("2D vector")) edit = [&model, &state, map, action] { if (model.AddComposite(map, action, "2DVector")) state.scrollRowToSelection = true; };
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Control scheme")) state.schemePopupPending = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##input_search", ICON_LC_SEARCH " Search", state.search, sizeof(state.search));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        const std::string preview = state.schemeFilter.empty() ? "All schemes" : state.schemeFilter;
        if (ImGui::BeginCombo("##input_scheme", preview.c_str()))
        {
            if (ImGui::Selectable("All schemes", state.schemeFilter.empty())) state.schemeFilter.clear();
            if (model.Draft().is_object() && model.Draft().contains("controlSchemes") && model.Draft()["controlSchemes"].is_array())
                for (const auto& s : model.Draft()["controlSchemes"])
                {
                    const std::string group = Str(s, "bindingGroup");
                    if (ImGui::Selectable(Str(s, "name").c_str(), state.schemeFilter == group)) state.schemeFilter = group;
                }
            ImGui::Separator();
            if (ImGui::Selectable("Edit schemes...")) state.schemePopupPending = true;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (state.previewArmed) ImGui::PushStyleColor(ImGuiCol_Button, Theme::WithAlpha(Theme::kAmber, 0.35f));
        if (ImGui::Button(ICON_LC_PLAY " Preview")) state.previewArmed = !state.previewArmed;
        if (state.previewArmed) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("Live preview: bindings glow as they fire; the Inspector's Live preview block reads live values");
    }

    void InputActionsDocumentWidgets::DrawMaps(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                const Services& services, Edit& edit)
    {
        ImGui::TextDisabled("ACTION MAPS");
        // Right-edge idiom (GetWindowContentRegionMax is obsolete in 1.92).
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_LC_PLUS "##addmap")) edit = [&model, &state] { if (model.AddMap()) OpenRenameOn(model, state, model.SelectedMap()); };
        const auto& draft = model.Draft();
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return;
        // A rename target that no row can draw (undo removed it) would wedge the
        // key handlers shut: sweep it -- the Outliner's sweep, EditorPanels.cpp:1644-1658.
        if (state.renameTarget.IsValid() && !IsMapId(draft, state.renameTarget) && !IsActionId(draft, state.renameTarget)) state.renameTarget = {};
        for (const auto& m : draft["actionMaps"])
        {
            const Guid id = IdOf(m);
            if (!id.IsValid()) continue;
            ImGui::PushID(id.ToString().c_str());
            const std::string name = Str(m, "name");
            if (state.renameTarget == id)
            {
                DrawRenameBox(model, state, id, name, edit);   // the same box as an action's (DrawRow)
                ImGui::PopID();
                continue;
            }
            const auto row = RowWithThumb("##map", 0, ICON_LC_LAYERS, name.c_str(), model.SelectedMap() == id, 0.0f);
            const ImVec2 rowBottom = ImGui::GetCursorScreenPos();   // restored after the trailing pills (same rule as DrawRow)
            if (row.clicked) model.SelectMap(id);
            if (model.SelectedMap() == id && state.scrollMapToSelection) { ImGui::SetScrollHereY(); state.scrollMapToSelection = false; }
            if (ImGui::BeginPopupContextItem("##mapmenu"))
            {
                if (ImGui::MenuItem("Rename", "F2")) OpenRenameOn(model, state, id);
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id] { if (model.DuplicateRow(id)) state.scrollMapToSelection = true; };
                if (ImGui::MenuItem("Set as default", nullptr, Str(draft, "defaultMap") == id.ToString()))
                    edit = [&model, id] { (void)model.SetDefaultMap(id); };
                ImGui::Separator();
                MoveRowMenu(model, id, edit);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, id] { (void)model.RemoveMap(id); };
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(row.trailingPos);
            if (Bool(m, "blocking")) { AssetPill("blocks", 1); ImGui::SameLine(); }
            const std::size_t count = m.contains("actions") && m["actions"].is_array() ? m["actions"].size() : 0;
            AssetPill(std::to_string(count).c_str(), 0);
            ImGui::SetCursorScreenPos(rowBottom);
            ImGui::PopID();
        }
        // Empty space in the maps column deselects: the ASSET is the container
        // (spec A s3.1). Not while an inline rename is live -- that click commits
        // the rename.
        if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            model.DeselectToAsset();   // silent (P18): not an event that takes the Inspector from another source
        HandleMapKeys(model, state, services, edit);   // still inside ##input_maps
    }

    void InputActionsDocumentWidgets::DrawActions(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                   const Services& services, Edit& edit)
    {
        const Guid map = model.SelectedMap();
        const nlohmann::json* m = FindMap(model.Draft(), map);
        if (!m) { ImGui::TextDisabled("Select an action map."); return; }
        ImGui::TextUnformatted(Str(*m, "name").c_str());
        ImGui::SameLine(); ImGui::TextDisabled("· actions");
        ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ICON_LC_PLUS " Action").x - ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::SmallButton(ICON_LC_PLUS " Action")) edit = [&model, &state, map] { if (model.AddAction(map)) OpenRenameOn(model, state, model.SelectedAction()); };
        ImGui::Separator();
        InputRowFilter filter;
        filter.search = state.search;
        filter.schemeGroup = state.schemeFilter;
        const std::vector<InputRow> rows = BuildInputRows(model.Draft(), map, filter, model.Conflicts(), state.collapsedActions);
        // A rename target can stop being drawable without its InputText ever
        // deactivating (undo/redo or an Inspector page removed the action; the
        // search/scheme filter or a collapse dropped it from `rows`): sweep it.
        if (state.renameTarget.IsValid())
        {
            bool drawn = false;
            for (const InputRow& r : rows) if (r.kind == InputRowKind::Action && r.id == state.renameTarget) { drawn = true; break; }
            if (!drawn && !IsMapId(model.Draft(), state.renameTarget)) state.renameTarget = {};
        }
        state.dragVerdictPrev = state.dragVerdict;
        state.dragVerdict = InputActionsDocumentState::DragVerdict::Illegal;   // hovering no row reads "Cannot move" (the drag op starts invalid)
        for (const InputRow& row : rows) DrawRow(row, model, state, services, edit, rows);
        // Empty space under the rows: the MAP is the container (spec A s3.1),
        // clearing action/binding/part SILENTLY (Ruling P18): not a selection
        // event, so it never takes the Inspector from the scene.
        if (!state.renameTarget.IsValid() && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            model.DeselectToMap(map);
        HandleKeys(model, state, services, edit, rows);
    }

    void InputActionsDocumentWidgets::DrawRow(const InputRow& row, InputActionsEditorModel& model,
                                               InputActionsDocumentState& state, const Services& services,
                                               Edit& edit, const std::vector<InputRow>& rows)
    {
        (void)rows;   // the keyboard nav (HandleKeys) reads the sibling rows; the row draw does not
        ImGui::PushID(row.id.ToString().c_str());
        ImGui::PushID(static_cast<int>(row.kind));
        const float indent = (row.depth > 0 ? ChevronCell() : 0.0f) + kIndent * static_cast<float>(row.depth);
        const bool swallowed = services.inputSwallowed && services.inputSwallowed();
        const Guid map = model.SelectedMap();
        if (row.kind == InputRowKind::AddBinding)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::kTextDim);
            if (ImGui::SmallButton(ICON_LC_PLUS " Binding") && !swallowed)
                edit = [&model, &state, map, action = row.actionId] { if (model.AddBinding(map, action)) state.scrollRowToSelection = true; };
            ImGui::PopStyleColor(2);
            ImGui::PopID(); ImGui::PopID();
            return;
        }

        // Inline rename (actions only; the maps column draws the same box).
        if (row.kind == InputRowKind::Action && state.renameTarget == row.id)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent + ImGui::GetFrameHeight());
            DrawRenameBox(model, state, row.id, row.name, edit);
            ImGui::PopID(); ImGui::PopID();
            return;
        }

        // Expander chevron cell before an action row: the row's thumb cell is
        // shifted right by it here, and the chevron itself is submitted AFTER
        // the row's Selectable (below, once the row's drag/menu/tooltip have
        // read it as the last item) -- RowWithThumb's documented foreground-item
        // pattern. Submitted before the row, the selected row's highlight
        // painted over it and it sat top-aligned in the 24 px row.
        const bool searching = state.search[0] != '\0';
        const char* chevron = nullptr;
        if (row.kind == InputRowKind::Action)
        {
            const bool collapsed = !searching && state.collapsedActions.count(row.id.ToString()) != 0;
            chevron = collapsed ? ICON_LC_CHEVRON_RIGHT "##x" : ICON_LC_CHEVRON_DOWN "##x";
        }
        const ImVec2 rowTop = ImGui::GetCursorScreenPos();

        const bool selected = RowSelected(model, row);
        const bool rebinding = (row.kind == InputRowKind::Binding || row.kind == InputRowKind::Part) && services.isRebinding && services.isRebinding(row.id);
        std::string label = row.name;
        if (rebinding)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "Press a control... Esc cancels · %.0f s", services.rebindRemaining ? services.rebindRemaining() : 0.0f);
            label = buf;
        }
        const char* icon = row.kind == InputRowKind::Action ? "" : row.kind == InputRowKind::CompositeHeader ? ICON_LC_LAYERS_2 : DeviceIcon(row.device);
        if (rebinding) ImGui::PushStyleColor(ImGuiCol_Text, Theme::kAmber);
        const AssetRowResult r = RowWithThumb("##row", 0, icon, label.c_str(), selected,
                                              row.kind == InputRowKind::Action ? ChevronCell() : indent);
        if (rebinding) ImGui::PopStyleColor();
        const ImVec2 rowBottom = ImGui::GetCursorScreenPos();   // RowWithThumb parked the cursor at the next row's start; restored at the end
        if (r.clicked && !swallowed) SelectRow(model, row);
        if (selected && state.scrollRowToSelection) { ImGui::SetScrollHereY(); state.scrollRowToSelection = false; }

        // Live glow: an amber bar at the row's left edge + a faint wash.
        if (const float v = services.glow ? services.glow(row.id) : 0.0f; v > 0.0f && row.kind != InputRowKind::Action)
        {
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(lo, hi, ImGui::ColorConvertFloat4ToU32(Theme::WithAlpha(Theme::kAmber, 0.12f + 0.2f * std::min(v, 1.0f))));
            dl->AddRectFilled(lo, ImVec2(lo.x + 2.0f, hi.y), ImGui::ColorConvertFloat4ToU32(Theme::kAmber));
        }

        // Drag-drop reorder within the parent (spec s6: plus Move up/down below).
        // Legality is decided while HOVERING, not on release: the target writes a
        // verdict, the source's preview reads it next frame (the drag decorator),
        // and only a legal target draws a drop highlight. Inert while a capture is
        // armed. The AddBinding ghost never reaches here (it returned above).
        if (!swallowed && ImGui::BeginDragDropSource())
        {
            const std::string id = row.id.ToString();
            ImGui::SetDragDropPayload(kDragPayload, id.c_str(), id.size() + 1);
            const bool legal = state.dragVerdictPrev == InputActionsDocumentState::DragVerdict::Legal;
            ImGui::PushStyleColor(ImGuiCol_Text, legal ? Theme::kText : Theme::kTextDim);
            ImGui::Text(legal ? "%s  Move '%s' here" : "%s  Cannot move '%s' here", legal ? ICON_LC_CHECK : ICON_LC_BAN, row.name.c_str());
            ImGui::PopStyleColor();
            ImGui::EndDragDropSource();
        }
        if (!swallowed && ImGui::BeginDragDropTarget())
        {
            // AcceptPeekOnly = AcceptBeforeDelivery | AcceptNoDrawDefaultRect: the
            // payload is visible every hovered frame and ImGui draws nothing itself.
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kDragPayload, ImGuiDragDropFlags_AcceptPeekOnly))
            {
                const Guid src = Guid::FromString(static_cast<const char*>(p->Data)).value_or(Guid{});
                const auto from = SiblingIndex(model.Draft(), src), to = SiblingIndex(model.Draft(), row.id);
                // Same parent array only (a Part cannot land among an action's
                // bindings, a Binding not among a composite's parts, nothing on
                // itself), and the index must change -- a no-op is not a target.
                const bool legal = from && to && from->parent == to->parent && src != row.id && from->index != to->index;
                state.dragVerdict = legal ? InputActionsDocumentState::DragVerdict::Legal : InputActionsDocumentState::DragVerdict::Illegal;
                if (legal)
                {
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                        ImGui::ColorConvertFloat4ToU32(Theme::kSelection), 0.0f, 0, 2.0f);
                    if (p->IsDelivery())
                        edit = [&model, src, index = to->index] { (void)model.MoveRowTo(src, index); };
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Context menu (inert while a capture is armed).
        if (!swallowed && ImGui::BeginPopupContextItem("##rowmenu"))
        {
            if (row.kind == InputRowKind::Action)
            {
                if (ImGui::MenuItem("Rename", "F2")) OpenRenameOn(model, state, row.id);
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, map, id = row.id] { if (model.DuplicateAction(map, id)) state.scrollRowToSelection = true; };
                if (ImGui::MenuItem("Add binding")) edit = [&model, &state, map, id = row.id] { if (model.AddBinding(map, id)) state.scrollRowToSelection = true; };
                if (ImGui::BeginMenu("Add composite"))
                {
                    if (ImGui::MenuItem("1D axis"))   edit = [&model, &state, map, id = row.id] { if (model.AddComposite(map, id, "1DAxis")) state.scrollRowToSelection = true; };
                    if (ImGui::MenuItem("2D vector")) edit = [&model, &state, map, id = row.id] { if (model.AddComposite(map, id, "2DVector")) state.scrollRowToSelection = true; };
                    ImGui::EndMenu();
                }
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, map, id = row.id] { (void)model.RemoveAction(map, id); };
            }
            else if (row.kind == InputRowKind::CompositeHeader)
            {
                if (ImGui::BeginMenu("Add part"))
                {
                    const bool axis = row.name == "1D Axis";
                    for (const char* role : axis ? std::vector<const char*>{ "negative", "positive" } : std::vector<const char*>{ "up", "down", "left", "right" })
                        if (ImGui::MenuItem(role)) edit = [&model, &state, id = row.id, role] { if (model.AddPart(id, role)) state.scrollRowToSelection = true; };
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id = row.id] { if (model.DuplicateRow(id)) state.scrollRowToSelection = true; };
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del")) edit = [&model, map, action = row.actionId, id = row.id] { (void)model.RemoveBinding(map, action, id); };
            }
            else   // Binding / Part
            {
                if (ImGui::MenuItem("Rebind...", "Enter") && services.beginRebind) services.beginRebind(row.id);
                if (ImGui::MenuItem("Duplicate")) edit = [&model, &state, id = row.id] { if (model.DuplicateRow(id)) state.scrollRowToSelection = true; };
                ImGui::Separator(); MoveRowMenu(model, row.id, edit); ImGui::Separator();
                if (ImGui::MenuItem("Delete", "Del"))
                {
                    if (row.kind == InputRowKind::Part) edit = [&model, composite = row.bindingId, id = row.id] { (void)model.RemovePart(composite, id); };
                    else edit = [&model, map, action = row.actionId, id = row.id] { (void)model.RemoveBinding(map, action, id); };
                }
            }
            ImGui::EndPopup();
        }
        if (!row.path.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", row.path.c_str());

        // The expander chevron, over the row (a SmallButton is FontSize tall:
        // centred in the 24 px row). A non-empty search forces every action
        // open (rows are built that way) and the click is a no-op then, so the
        // user's own collapse state survives the search.
        if (chevron)
        {
            ImGui::SetCursorScreenPos(ImVec2(rowTop.x, rowTop.y + (24.0f - ImGui::GetFontSize()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::kNone);
            if (ImGui::SmallButton(chevron) && !searching && !swallowed)
            {
                if (state.collapsedActions.count(row.id.ToString()) != 0) state.collapsedActions.erase(row.id.ToString());
                else state.collapsedActions.insert(row.id.ToString());
            }
            ImGui::PopStyleColor();
        }

        // Trailing: conflict dot, badges, detail, the Rebind hit region.
        ImGui::SetCursorScreenPos(r.trailingPos);
        if (row.conflict)
        {
            ImGui::TextColored(Theme::kAmber, ICON_LC_CIRCLE_DOT);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", ConflictTooltip(model.Conflicts(), row.id).c_str());
            }
            ImGui::SameLine();
        }
        if (row.kind == InputRowKind::Action)
        {
            if (!row.badge.empty()) AssetPill(row.badge.c_str(), 0);
            if (!row.detail.empty()) { if (!row.badge.empty()) ImGui::SameLine(); ImGui::TextDisabled("%s", row.detail.c_str()); }
        }
        else
        {
            if (!row.detail.empty()) { ImGui::TextDisabled("%s", row.detail.c_str()); ImGui::SameLine(); }
            for (const auto& g : row.groups) { AssetPill(g.c_str(), SchemeVariant(g)); ImGui::SameLine(); }   // one tinted pill PER scheme
            if (row.kind != InputRowKind::CompositeHeader && !rebinding && !swallowed)
            {
                // The Rebind button: its hit region is SUBMITTED every frame and
                // only its PAINT is gated on hover/selection. Gating the submission
                // on r.hovered oscillates on an AllowOverlap row (the Asset Browser
                // rail's documented bug, AssetBrowserPanel.cpp:304-341): the rule
                // for any trailing widget on a RowWithThumb row.
                const ImVec2 sz(ImGui::CalcTextSize("Rebind").x + ImGui::GetStyle().FramePadding.x * 2.0f, ImGui::GetFrameHeight());
                const bool rbClicked = ImGui::InvisibleButton("##rebind", sz);
                const bool rbHovered = ImGui::IsItemHovered();
                if (r.hovered || selected || rbHovered)
                {
                    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(lo, hi, ImGui::GetColorU32(rbHovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), ImGui::GetStyle().FrameRounding);
                    dl->AddText(ImVec2(lo.x + ImGui::GetStyle().FramePadding.x, lo.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), "Rebind");
                }
                if (rbClicked && services.beginRebind) services.beginRebind(row.id);
            }
        }
        ImGui::SetCursorScreenPos(rowBottom);   // every row pitches exactly one RowWithThumb height whatever the trailing item's height was
        ImGui::PopID(); ImGui::PopID();
    }

    void InputActionsDocumentWidgets::DrawSchemePopup(InputActionsEditorModel& model, InputActionsDocumentState& state, Edit& edit)
    {
        if (!ImGui::BeginPopupModal("Control schemes##input", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        if (ImGui::BeginTable("##schemes", 3, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Group"); ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, 24.0f);
            ImGui::TableHeadersRow();
            if (model.Draft().is_object() && model.Draft().contains("controlSchemes") && model.Draft()["controlSchemes"].is_array())
                for (const auto& s : model.Draft()["controlSchemes"])
                {
                    const Guid id = IdOf(s);
                    ImGui::PushID(id.ToString().c_str());
                    ImGui::TableNextRow();
                    std::string name = Str(s, "name"), group = Str(s, "bindingGroup");
                    ImGui::TableSetColumnIndex(0); ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString("##name", &name);
                    if (ImGui::IsItemDeactivatedAfterEdit() && !name.empty()) edit = [&model, id, name, group] { (void)model.EditScheme(id, name, group); };
                    ImGui::TableSetColumnIndex(1); ImGui::SetNextItemWidth(-FLT_MIN);
                    InputTextString("##group", &group);
                    if (ImGui::IsItemDeactivatedAfterEdit() && !group.empty()) edit = [&model, id, name, group] { (void)model.EditScheme(id, name, group); };
                    ImGui::TableSetColumnIndex(2);
                    if (ImGui::SmallButton(ICON_LC_TRASH_2)) edit = [&model, id] { (void)model.RemoveScheme(id); };
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f); ImGui::InputText("Name", state.newSchemeName, sizeof state.newSchemeName);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f); ImGui::InputText("Group", state.newSchemeGroup, sizeof state.newSchemeGroup);
        ImGui::SameLine();
        if (ImGui::Button(ICON_LC_PLUS " Scheme"))
            edit = [&model, name = std::string(state.newSchemeName), group = std::string(state.newSchemeGroup)] { (void)model.AddScheme(name, group); };
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Keyboard navigation (spec B s2.3), one handler per column. Each runs
    // inside its own column child, so IsWindowFocused(ChildWindows) routes the
    // keys to whichever column has focus. Every mutation is deferred through
    // `edit` (undoable, after the draw); selection and collapse state change in
    // place, as a click does.
    void InputActionsDocumentWidgets::HandleKeys(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                  const Services& services, Edit& edit, const std::vector<InputRow>& rows)
    {
        if (!ColumnKeysLive(services, state)) return;
        const Guid current = model.SelectedPart().IsValid() ? model.SelectedPart()
                           : model.SelectedBinding().IsValid() ? model.SelectedBinding()
                           : model.SelectedAction();
        const InputRow* row = nullptr;
        for (const auto& r : rows) if (r.kind != InputRowKind::AddBinding && r.id == current) { row = &r; break; }

        auto step = [&](int dir)
        {
            if (const auto next = StepSelection(rows, current, dir))
                for (const auto& r : rows) if (r.id == *next && r.kind != InputRowKind::AddBinding) { SelectRow(model, r); state.scrollRowToSelection = true; break; }
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(+1);   // navigation repeats (UE SListView)
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   step(-1);
        if (!row) return;
        const std::string actionKey = row->actionId.ToString();
        const bool searching = state.search[0] != '\0';   // a search forces every action open: Left/Right never edit collapsedActions then
        const bool collapsed = !searching && state.collapsedActions.count(actionKey) != 0;
        auto rowById = [&](InputRowKind kind, const Guid& id) -> const InputRow*
        {
            for (const auto& r : rows) if (r.kind == kind && r.id == id) return &r;
            return nullptr;
        };
        // Tree convention: Left collapses an expanded action, otherwise selects
        // the parent row (no collapse); Right expands a collapsed action,
        // otherwise descends to the first child. Commands (Left/Right/Enter/F2/
        // Delete) never auto-repeat: repeat = false, as the Outliner passes.
        // Alt-modified presses are left alone (window/menu chords).
        if (!ImGui::GetIO().KeyAlt)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
            {
                if (row->kind == InputRowKind::Action)
                {
                    if (!collapsed && !searching) state.collapsedActions.insert(actionKey);   // already collapsed: no-op
                }
                else if (row->kind == InputRowKind::Part)
                {
                    if (const auto* parent = rowById(InputRowKind::CompositeHeader, row->bindingId)) { SelectRow(model, *parent); state.scrollRowToSelection = true; }
                }
                else if (const auto* parent = rowById(InputRowKind::Action, row->actionId)) { SelectRow(model, *parent); state.scrollRowToSelection = true; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
            {
                if (row->kind == InputRowKind::Action && collapsed) state.collapsedActions.erase(actionKey);
                else if (row->kind == InputRowKind::Action || row->kind == InputRowKind::CompositeHeader)
                {
                    const std::size_t at = static_cast<std::size_t>(row - rows.data());
                    if (at + 1 < rows.size() && rows[at + 1].depth > row->depth && rows[at + 1].kind != InputRowKind::AddBinding)
                    { SelectRow(model, rows[at + 1]); state.scrollRowToSelection = true; }
                }
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && (row->kind == InputRowKind::Binding || row->kind == InputRowKind::Part) && services.beginRebind)
            services.beginRebind(row->id);
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && row->kind == InputRowKind::Action)
            OpenRenameOn(model, state, row->id);   // the same entry as the context menu's Rename
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            const Guid map = model.SelectedMap();
            switch (row->kind)
            {
            case InputRowKind::Action:          edit = [&model, map, id = row->id] { (void)model.RemoveAction(map, id); }; break;
            case InputRowKind::Binding:
            case InputRowKind::CompositeHeader: edit = [&model, map, action = row->actionId, id = row->id] { (void)model.RemoveBinding(map, action, id); }; break;
            case InputRowKind::Part:            edit = [&model, composite = row->bindingId, id = row->id] { (void)model.RemovePart(composite, id); }; break;
            default: break;
            }
        }
    }

    void InputActionsDocumentWidgets::HandleMapKeys(InputActionsEditorModel& model, InputActionsDocumentState& state,
                                                     const Services& services, Edit& edit)
    {
        if (!ColumnKeysLive(services, state)) return;
        const auto& draft = model.Draft();
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return;
        std::vector<Guid> ids;
        for (const auto& m : draft["actionMaps"]) if (const Guid id = IdOf(m); id.IsValid()) ids.push_back(id);
        if (ids.empty()) return;
        const Guid current = model.SelectedMap();
        const auto index = std::find(ids.begin(), ids.end(), current);
        auto step = [&](int dir)
        {
            const std::ptrdiff_t i = index == ids.end() ? (dir > 0 ? 0 : static_cast<std::ptrdiff_t>(ids.size()) - 1)
                                                        : std::clamp<std::ptrdiff_t>((index - ids.begin()) + dir, 0, static_cast<std::ptrdiff_t>(ids.size()) - 1);
            model.SelectMap(ids[static_cast<std::size_t>(i)]); state.scrollMapToSelection = true;
        };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) step(+1);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   step(-1);
        if (index == ids.end()) return;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) OpenRenameOn(model, state, current);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) edit = [&model, id = current] { (void)model.RemoveMap(id); };
    }
}
