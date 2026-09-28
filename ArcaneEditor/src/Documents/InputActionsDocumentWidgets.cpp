#include "Documents/InputActionsDocumentWidgets.hpp"

#include "Widgets/EditorWidgets.hpp"

#include <imgui.h>

#include <array>

namespace Arcane::Editor
{
    namespace
    {
        Guid Id(const nlohmann::json& row)
        {
            if (!row.is_object() || !row.contains("id") || !row["id"].is_string()) return {};
            return Guid::FromString(row["id"].get<std::string>()).value_or(Guid{});
        }

        std::string String(const nlohmann::json& row, const char* key)
        {
            if (!row.is_object() || !row.contains(key) || !row[key].is_string()) return {};
            return row[key].get<std::string>();
        }

        const nlohmann::json* Find(const nlohmann::json& row, const Guid& id)
        {
            if (row.is_object())
            {
                if (Id(row) == id) return &row;
                for (const auto& [key, child] : row.items())
                    if (const auto* match = Find(child, id)) return match;
            }
            else if (row.is_array())
                for (const auto& child : row)
                    if (const auto* match = Find(child, id)) return match;
            return nullptr;
        }

        constexpr std::array<const char*, 17> kPaths = {
            "<Keyboard>/a", "<Keyboard>/d", "<Keyboard>/w", "<Keyboard>/s",
            "<Keyboard>/space", "<Keyboard>/leftArrow", "<Keyboard>/rightArrow",
            "<Mouse>/leftButton", "<Mouse>/rightButton", "<Mouse>/delta/x",
            "<Gamepad>/leftStick/x", "<Gamepad>/leftStick/y",
            "<Gamepad>/buttonSouth", "<Gamepad>/buttonEast",
            "<Gamepad>/dpad/left", "<Gamepad>/dpad/right", "<Gamepad>/rightTrigger"
        };
    }

    bool InputActionsDocumentWidgets::TextField(
        const char* label, const std::string& value,
        const std::function<void(std::string)>& commit)
    {
        const auto widgetId = ImGui::GetID(label);
        auto [it, inserted] = fieldDrafts_.try_emplace(widgetId, value);
        if (!ImGui::IsAnyItemActive() && !inserted && it->second != value)
            it->second = value;
        InputTextString(label, &it->second);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            const auto edited = it->second;
            fieldDrafts_.erase(it);
            if (edited != value) { commit(edited); return true; }
        }
        return false;
    }

    void InputActionsDocumentWidgets::Draw(InputActionsEditorModel& model,
                                            const InputSnapshot& snapshot)
    {
        if (captureTarget_.IsValid())
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) capture_.Cancel();
            else capture_.Observe(snapshot, 1.0f / 60.0f);
            const auto& result = capture_.Result();
            if (result.state == InputRebindState::Completed)
            {
                (void)model.SetField(captureTarget_, "path", result.replacementPath);
                captureTarget_ = {};
            }
            else if (result.state == InputRebindState::Canceled ||
                     result.state == InputRebindState::TimedOut)
                captureTarget_ = {};
        }
        if (!model.Draft().is_object() || !model.Draft().contains("actionMaps") ||
            !model.Draft()["actionMaps"].is_array())
        {
            ImGui::TextWrapped("Repair the JSON draft to resume visual editing.");
            return;
        }
        std::function<void()> edit;
        ImGui::BeginChild("##input_maps", ImVec2(205, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        ImGui::TextUnformatted("ACTION MAPS");
        if (ImGui::Button("+ Map")) edit = [&model] { (void)model.AddMap(); };
        for (const auto& map : model.Draft()["actionMaps"])
        {
            if (!map.is_object()) continue;
            const auto id = Id(map);
            ImGui::PushID(id.ToString().c_str());
            const bool selected = model.SelectedMap() == id;
            if (ImGui::Selectable(String(map, "name").c_str(), selected)) model.SelectMap(id);
            if (model.Draft().value("defaultMap", std::string{}) == id.ToString())
            { ImGui::SameLine(); ImGui::TextDisabled("*"); }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("CONTROL SCHEMES");
        if (model.Draft().contains("controlSchemes") && model.Draft()["controlSchemes"].is_array())
        {
            for (const auto& scheme : model.Draft()["controlSchemes"])
            {
                const auto id = Id(scheme);
                ImGui::PushID(id.ToString().c_str());
                ImGui::Text("%s (%s)", String(scheme, "name").c_str(),
                            String(scheme, "bindingGroup").c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) edit = [&model, id] { (void)model.RemoveScheme(id); };
                ImGui::PopID();
            }
        }
        ImGui::SetNextItemWidth(90); ImGui::InputText("Name##scheme", schemeName_, sizeof(schemeName_));
        ImGui::SetNextItemWidth(90); ImGui::InputText("Group##scheme", schemeGroup_, sizeof(schemeGroup_));
        if (ImGui::Button("+ Scheme"))
        {
            const std::string name = schemeName_, group = schemeGroup_;
            edit = [&model, name, group] { (void)model.AddScheme(name, group); };
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##input_hierarchy", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const auto mapId = model.SelectedMap();
        const auto* map = Find(model.Draft()["actionMaps"], mapId);
        if (!map || !map->contains("actions") || !(*map)["actions"].is_array())
            ImGui::TextDisabled("Select an action map.");
        else
        {
            ImGui::Text("%s", String(*map, "name").c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Default")) edit = [&model, mapId] { (void)model.SetDefaultMap(mapId); };
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Action")) edit = [&model, mapId] { (void)model.AddAction(mapId); };
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete Map")) edit = [&model, mapId] { (void)model.RemoveMap(mapId); };
            for (const auto& action : (*map)["actions"])
            {
                const auto actionId = Id(action);
                ImGui::PushID(actionId.ToString().c_str());
                const bool open = ImGui::TreeNodeEx("##action", ImGuiTreeNodeFlags_OpenOnArrow,
                    "%s (%s)", String(action, "name").c_str(), String(action, "type").c_str());
                if (ImGui::IsItemClicked()) { model.SelectAction(actionId); model.SelectBinding({}); }
                ImGui::SameLine(); if (ImGui::SmallButton("+")) edit = [&model, mapId, actionId] { (void)model.AddBinding(mapId, actionId); };
                ImGui::SameLine(); if (ImGui::SmallButton("1D")) edit = [&model, mapId, actionId] { (void)model.AddComposite(mapId, actionId, "1DAxis"); };
                ImGui::SameLine(); if (ImGui::SmallButton("2D")) edit = [&model, mapId, actionId] { (void)model.AddComposite(mapId, actionId, "2DVector"); };
                if (open)
                {
                    if (action.contains("bindings") && action["bindings"].is_array())
                        for (const auto& binding : action["bindings"])
                        {
                            const auto bindingId = Id(binding);
                            ImGui::PushID(bindingId.ToString().c_str());
                            const bool composite = binding.contains("composite");
                            const auto label = composite ? String(binding, "composite") : String(binding, "path");
                            const bool bindingOpen = ImGui::TreeNodeEx("##binding",
                                ImGuiTreeNodeFlags_OpenOnArrow | (composite ? 0 : ImGuiTreeNodeFlags_Leaf),
                                "%s", label.c_str());
                            if (ImGui::IsItemClicked())
                            { model.SelectAction(actionId); model.SelectBinding(bindingId); }
                            if (bindingOpen)
                            {
                                if (composite && binding.contains("parts") && binding["parts"].is_array())
                                    for (const auto& part : binding["parts"])
                                    {
                                        const auto partId = Id(part);
                                        ImGui::PushID(partId.ToString().c_str());
                                        if (ImGui::Selectable((String(part, "name") + ": " + String(part, "path")).c_str(),
                                                              model.SelectedPart() == partId))
                                        { model.SelectAction(actionId); model.SelectBinding(bindingId); model.SelectPart(partId); }
                                        ImGui::PopID();
                                    }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::Separator();
            const auto targetId = model.SelectedPart().IsValid() ? model.SelectedPart()
                : model.SelectedBinding().IsValid() ? model.SelectedBinding()
                : model.SelectedAction().IsValid() ? model.SelectedAction() : mapId;
            if (const auto* selected = Find(model.Draft(), targetId))
            {
                ImGui::TextUnformatted("PROPERTIES");
                if (selected->contains("name"))
                    TextField("Name", String(*selected, "name"),
                        [&model, targetId](std::string text) { (void)model.SetField(targetId, "name", std::move(text)); });
                if (targetId == mapId)
                {
                    bool blocking = selected->value("blocking", false);
                    if (ImGui::Checkbox("Blocking", &blocking))
                        edit = [&model, targetId, blocking] { (void)model.SetField(targetId, "blocking", blocking); };
                    int priority = selected->value("priority", 0);
                    if (ImGui::InputInt("Priority", &priority) && ImGui::IsItemDeactivatedAfterEdit())
                        edit = [&model, targetId, priority] { (void)model.SetField(targetId, "priority", priority); };
                }
                if (selected->contains("type"))
                {
                    const auto type = String(*selected, "type");
                    if (ImGui::BeginCombo("Action Type", type.c_str()))
                    {
                        for (const char* choice : {"Button", "Axis1D", "Axis2D"})
                            if (ImGui::Selectable(choice, type == choice))
                                edit = [&model, targetId, choice] { (void)model.SetField(targetId, "type", choice); };
                        ImGui::EndCombo();
                    }
                }
                if (selected->contains("path"))
                {
                    const auto path = String(*selected, "path");
                    TextField("Control Path", path, [&model, targetId](std::string text)
                        { (void)model.SetField(targetId, "path", std::move(text)); });
                    if (ImGui::BeginCombo("Pick Control", path.c_str()))
                    {
                        for (const char* choice : kPaths)
                            if (ImGui::Selectable(choice, path == choice))
                                edit = [&model, targetId, choice] { (void)model.SetField(targetId, "path", choice); };
                        ImGui::EndCombo();
                    }
                    ImGui::Combo("Capture Device", &captureDevice_, "Keyboard / Mouse\0Gamepad\0\0");
                    if (captureTarget_ == targetId)
                    {
                        ImGui::TextDisabled("Press a control (Esc cancels; 10 second timeout)");
                        if (ImGui::Button("Cancel Capture")) capture_.Cancel();
                    }
                    else if (ImGui::Button("Capture Control"))
                    {
                        captureTarget_ = targetId;
                        capture_.Begin(targetId,
                            captureDevice_ == 0 ? InputDevice::Kbm : InputDevice::Gamepad,
                            10.0f, snapshot);
                    }
                }
                for (const char* field : {"processors", "interactions"})
                {
                    std::string current;
                    if (selected->contains(field) && (*selected)[field].is_array())
                        for (const auto& entry : (*selected)[field])
                            if (entry.is_string())
                            { if (!current.empty()) current += ", "; current += entry.get<std::string>(); }
                    TextField(field, current, [&model, targetId, field](std::string value)
                    {
                        nlohmann::json entries = nlohmann::json::array();
                        size_t offset = 0;
                        while (offset < value.size())
                        {
                            const auto end = value.find(',', offset);
                            auto token = value.substr(offset, end == std::string::npos ? end : end - offset);
                            const auto first = token.find_first_not_of(" \t");
                            if (first != std::string::npos)
                            {
                                token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
                                if (!token.empty()) entries.push_back(token);
                            }
                            if (end == std::string::npos) break;
                            offset = end + 1;
                        }
                        (void)model.SetField(targetId, field, std::move(entries));
                    });
                }
                if (selected->contains("groups") || selected->contains("path") || selected->contains("composite"))
                {
                    auto groups = selected->value("groups", nlohmann::json::array());
                    if (!groups.is_array()) groups = nlohmann::json::array();
                    for (const auto& scheme : model.Draft().value("controlSchemes", nlohmann::json::array()))
                    {
                        const auto group = String(scheme, "bindingGroup");
                        bool enabled = false;
                        for (const auto& existing : groups) if (existing == group) enabled = true;
                        if (ImGui::Checkbox(group.c_str(), &enabled))
                        {
                            nlohmann::json nextGroups = groups;
                            if (enabled) nextGroups.push_back(group);
                            else for (size_t i = nextGroups.size(); i > 0; --i)
                                if (nextGroups[i - 1] == group) nextGroups.erase(nextGroups.begin() + i - 1);
                            edit = [&model, targetId, nextGroups] { (void)model.SetField(targetId, "groups", nextGroups); };
                        }
                    }
                }
                if (selected->contains("bindings"))
                {
                    if (ImGui::Button("Duplicate Action")) edit = [&model, mapId, targetId] { (void)model.DuplicateAction(mapId, targetId); };
                    ImGui::SameLine();
                    if (ImGui::Button("Delete Action")) edit = [&model, mapId, targetId] { (void)model.RemoveAction(mapId, targetId); };
                }
                else if (model.SelectedPart().IsValid())
                {
                    const auto bindingId = model.SelectedBinding();
                    if (ImGui::Button("Delete Part"))
                        edit = [&model, bindingId, targetId] { (void)model.RemovePart(bindingId, targetId); };
                }
                else if (selected->contains("path") || selected->contains("composite"))
                {
                    if (ImGui::Button("Delete Binding") && model.SelectedBinding().IsValid() && !model.SelectedPart().IsValid())
                    {
                        const auto actionId = model.SelectedAction(), bindingId = model.SelectedBinding();
                        edit = [&model, mapId, actionId, bindingId] { (void)model.RemoveBinding(mapId, actionId, bindingId); };
                    }
                }
                if (selected->contains("composite"))
                {
                    const auto composite = String(*selected, "composite");
                    if (ImGui::BeginCombo("Add Part", "Choose role"))
                    {
                        const auto roles = composite == "1DAxis"
                            ? std::vector<const char*>{"negative", "positive"}
                            : std::vector<const char*>{"up", "down", "left", "right"};
                        for (const auto* role : roles)
                            if (ImGui::Selectable(role))
                                edit = [&model, targetId, role] { (void)model.AddPart(targetId, role); };
                        ImGui::EndCombo();
                    }
                }
                if (ImGui::Button("Duplicate Row"))
                    edit = [&model, targetId] { (void)model.DuplicateRow(targetId); };
                ImGui::SameLine();
                if (ImGui::Button("Move Up")) edit = [&model, targetId] { (void)model.MoveRow(targetId, -1); };
                ImGui::SameLine();
                if (ImGui::Button("Move Down")) edit = [&model, targetId] { (void)model.MoveRow(targetId, 1); };
            }
        }
        ImGui::Separator();
        for (const auto& warning : model.Warnings())
            ImGui::TextWrapped("Warning: %s", warning.c_str());
        if (model.LastValidPreview())
        {
            if (!preview_ || previewSource_ != model.LastValidPreview()->ToJson())
            {
                preview_ = InputActions::Create();
                if (!preview_->LoadAsset(*model.LastValidPreview())) preview_.reset();
                previewSource_ = model.LastValidPreview()->ToJson();
            }
            if (preview_)
            {
                auto raw = snapshot;
                raw.wantCaptureKeyboard = false;
                raw.wantCaptureMouse = false;
                preview_->Update(1.0 / 60.0, raw);
                const auto action = model.SelectedAction();
                if (action.IsValid())
                {
                    const auto value = preview_->Value(action);
                    const char* phase = "Waiting";
                    switch (value.phase)
                    {
                    case InputActionPhase::Started: phase = "Started"; break;
                    case InputActionPhase::Performed: phase = "Performed"; break;
                    case InputActionPhase::Canceled: phase = "Canceled"; break;
                    default: break;
                    }
                    ImGui::Separator();
                    ImGui::Text("LIVE: %s  scalar %.2f  vector (%.2f, %.2f)",
                                phase, value.scalar, value.vector.x, value.vector.y);
                    ImGui::TextDisabled("Device: %s", preview_->ActiveDevice() == InputDevice::Gamepad
                                        ? "Gamepad" : "Keyboard / Mouse");
                }
            }
        }
        ImGui::EndChild();
        if (edit) edit();
    }
}
