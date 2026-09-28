#include "Documents/InputActionsEditorModel.hpp"

#include <Arcane/Edit/CommandStack.hpp>

#include <fstream>
#include <algorithm>
#include <set>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane::Editor
{
    namespace
    {
        class DraftEditCommand final : public Arcane::ICommand
        {
        public:
            DraftEditCommand(std::weak_ptr<InputActionsEditorModel*> anchor,
                             std::string label, nlohmann::json before,
                             nlohmann::json after)
                : anchor_(std::move(anchor)), label_(std::move(label)),
                  before_(std::move(before)), after_(std::move(after)) {}
            void Undo() override { Apply(before_); }
            void Redo() override { Apply(after_); }
            const char* Label() const override { return label_.c_str(); }
        private:
            void Apply(const nlohmann::json& value)
            {
                const auto alive = anchor_.lock();
                if (alive && *alive) (*alive)->RestoreDraft(value);
            }
            std::weak_ptr<InputActionsEditorModel*> anchor_;
            std::string label_;
            nlohmann::json before_;
            nlohmann::json after_;
        };

        void RefreshIds(nlohmann::json& node)
        {
            if (node.is_object())
            {
                if (node.contains("id")) node["id"] = Guid::Generate().ToString();
                for (auto& [key, value] : node.items()) RefreshIds(value);
            }
            else if (node.is_array())
                for (auto& value : node) RefreshIds(value);
        }

        nlohmann::json* FindId(nlohmann::json& node, const Guid& id)
        {
            if (node.is_object())
            {
                if (node.contains("id") && node["id"].is_string() &&
                    node["id"].get<std::string>() == id.ToString()) return &node;
                for (auto& [key, value] : node.items())
                    if (auto* found = FindId(value, id)) return found;
            }
            else if (node.is_array())
                for (auto& value : node)
                    if (auto* found = FindId(value, id)) return found;
            return nullptr;
        }

        nlohmann::json* FindChild(nlohmann::json& root, const Guid& parent,
                                  const char* array, const Guid& child)
        {
            auto* node = FindId(root, parent);
            if (!node || !node->contains(array) || !(*node)[array].is_array()) return nullptr;
            for (auto& item : (*node)[array])
                if (item.is_object() && item.value("id", std::string{}) == child.ToString())
                    return &item;
            return nullptr;
        }

        bool RemoveChild(nlohmann::json& root, const Guid& parent,
                         const char* array, const Guid& child)
        {
            auto* node = FindId(root, parent);
            if (!node || !node->contains(array) || !(*node)[array].is_array()) return false;
            auto& rows = (*node)[array];
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].is_object() && rows[i].value("id", std::string{}) == child.ToString())
                { rows.erase(rows.begin() + i); return true; }
            return false;
        }

        bool MoveInArray(nlohmann::json& node, const Guid& id, int direction)
        {
            if (node.is_array())
            {
                for (size_t i = 0; i < node.size(); ++i)
                {
                    if (!node[i].is_object() || node[i].value("id", std::string{}) != id.ToString()) continue;
                    if ((direction < 0 && i == 0) || (direction > 0 && i + 1 == node.size())) return false;
                    if (direction == 0) return false;
                    std::swap(node[i], node[static_cast<size_t>(static_cast<int>(i) + direction)]);
                    return true;
                }
                for (auto& child : node) if (MoveInArray(child, id, direction)) return true;
            }
            else if (node.is_object())
                for (auto& [key, child] : node.items())
                    if (MoveInArray(child, id, direction)) return true;
            return false;
        }

        bool DuplicateInArray(nlohmann::json& node, const Guid& id)
        {
            if (node.is_array())
            {
                for (size_t i = 0; i < node.size(); ++i)
                {
                    if (!node[i].is_object() || node[i].value("id", std::string{}) != id.ToString()) continue;
                    auto copy = node[i];
                    RefreshIds(copy);
                    if (copy.contains("name") && copy["name"].is_string() &&
                        !copy.contains("path"))
                        copy["name"] = copy["name"].get<std::string>() + " Copy";
                    node.insert(node.begin() + i + 1, std::move(copy));
                    return true;
                }
                for (auto& child : node) if (DuplicateInArray(child, id)) return true;
            }
            else if (node.is_object())
                for (auto& [key, child] : node.items())
                    if (DuplicateInArray(child, id)) return true;
            return false;
        }
    }

    InputActionsEditorModel::InputActionsEditorModel(nlohmann::json draft,
                                                     Arcane::CommandStack* commands)
        : draft_(std::move(draft)), saved_(draft_), commands_(commands),
          anchor_(std::make_shared<InputActionsEditorModel*>(this))
    { Validate(); }

    InputActionsEditorModel::~InputActionsEditorModel() { *anchor_ = nullptr; }

    void InputActionsEditorModel::Validate()
    {
        std::string error;
        auto parsed = InputActionAsset::FromJson(draft_, &error);
        diagnostics_.clear();
        if (parsed) preview_ = std::move(*parsed);
        else diagnostics_.push_back(error.empty() ? "Invalid gameplay input document" : error);
    }

    void InputActionsEditorModel::RestoreDraft(const nlohmann::json& draft)
    {
        draft_ = draft;
        Validate();
    }

    bool InputActionsEditorModel::ApplyEdit(std::string label, nlohmann::json before,
                                             nlohmann::json after)
    {
        if (before != draft_ || before == after) return false;
        RestoreDraft(after);
        if (commands_)
            commands_->Push(std::make_unique<DraftEditCommand>(anchor_, std::move(label),
                                                                std::move(before), std::move(after)));
        return true;
    }

    bool InputActionsEditorModel::Undo()
    {
        if (!commands_ || !commands_->CanUndo()) return false;
        commands_->Undo();
        return true;
    }

    bool InputActionsEditorModel::Redo()
    {
        if (!commands_ || !commands_->CanRedo()) return false;
        commands_->Redo();
        return true;
    }

    bool InputActionsEditorModel::Save(const std::filesystem::path& path)
    {
        std::string error;
        const auto asset = InputActionAsset::FromJson(draft_, &error);
        if (!asset || path.empty()) return false;
        auto temporary = path;
        temporary += "." + Guid::Generate().ToString() + ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) return false;
            stream << asset->ToJson().dump(2) << '\n';
            stream.flush();
            if (!stream)
            {
                stream.close();
                std::error_code ec;
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }
        std::error_code ec;
#ifdef _WIN32
        const bool replaced = MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::filesystem::rename(temporary, path, ec);
        const bool replaced = !ec;
#endif
        if (!replaced)
        {
            std::filesystem::remove(temporary, ec);
            return false;
        }
        saved_ = draft_;
        return true;
    }

    bool InputActionsEditorModel::DuplicateAction(const Guid& mapId, const Guid& actionId)
    {
        if (!draft_.is_object() || !draft_.contains("actionMaps") ||
            !draft_["actionMaps"].is_array()) return false;
        nlohmann::json next = draft_;
        for (auto& map : next["actionMaps"])
        {
            if (!map.is_object() || map.value("id", std::string{}) != mapId.ToString() ||
                !map.contains("actions") || !map["actions"].is_array()) continue;
            for (size_t i = 0; i < map["actions"].size(); ++i)
            {
                if (map["actions"][i].value("id", std::string{}) != actionId.ToString()) continue;
                auto duplicate = map["actions"][i];
                RefreshIds(duplicate);
                duplicate["name"] = duplicate.value("name", std::string("Action")) + " Copy";
                map["actions"].insert(map["actions"].begin() + i + 1, std::move(duplicate));
                return ApplyEdit("Duplicate action", draft_, next);
            }
        }
        return false;
    }

    bool InputActionsEditorModel::AddMap(std::string name)
    {
        if (name.empty() || !draft_.is_object() || !draft_.value("actionMaps", nlohmann::json{}).is_array()) return false;
        auto next = draft_;
        const auto id = Guid::Generate();
        next["actionMaps"].push_back({{"id", id.ToString()}, {"name", std::move(name)},
                                      {"actions", nlohmann::json::array()}});
        if (next["actionMaps"].size() == 1) next["defaultMap"] = id.ToString();
        if (!ApplyEdit("Add action map", draft_, next)) return false;
        SelectMap(id);
        return true;
    }

    bool InputActionsEditorModel::RemoveMap(const Guid& map)
    {
        if (!draft_.is_object() || !draft_.contains("actionMaps") || !draft_["actionMaps"].is_array()) return false;
        auto next = draft_;
        auto& rows = next["actionMaps"];
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (rows[i].value("id", std::string{}) != map.ToString()) continue;
            rows.erase(rows.begin() + i);
            if (next.value("defaultMap", std::string{}) == map.ToString())
            {
                if (rows.empty()) next.erase("defaultMap");
                else next["defaultMap"] = rows[0]["id"];
            }
            if (!ApplyEdit("Remove action map", draft_, next)) return false;
            SelectMap(rows.empty() ? Guid{} : *Guid::FromString(rows[0]["id"].get<std::string>()));
            return true;
        }
        return false;
    }

    bool InputActionsEditorModel::AddAction(const Guid& map, std::string name)
    {
        if (name.empty()) return false;
        auto next = draft_;
        auto* owner = FindId(next, map);
        if (!owner || !owner->contains("actions") || !(*owner)["actions"].is_array()) return false;
        const auto id = Guid::Generate();
        (*owner)["actions"].push_back({{"id", id.ToString()}, {"name", std::move(name)},
                                        {"type", "Button"}, {"bindings", nlohmann::json::array()}});
        if (!ApplyEdit("Add action", draft_, next)) return false;
        SelectMap(map); SelectAction(id);
        return true;
    }

    bool InputActionsEditorModel::RemoveAction(const Guid& map, const Guid& action)
    {
        auto next = draft_;
        if (!RemoveChild(next, map, "actions", action)) return false;
        if (!ApplyEdit("Remove action", draft_, next)) return false;
        SelectMap(map);
        return true;
    }

    bool InputActionsEditorModel::AddBinding(const Guid& map, const Guid& action, std::string path)
    {
        if (path.empty()) return false;
        auto next = draft_;
        auto* owner = FindChild(next, map, "actions", action);
        if (!owner || !owner->contains("bindings") || !(*owner)["bindings"].is_array()) return false;
        const auto id = Guid::Generate();
        (*owner)["bindings"].push_back({{"id", id.ToString()}, {"path", std::move(path)}});
        if (!ApplyEdit("Add binding", draft_, next)) return false;
        SelectMap(map); SelectAction(action); SelectBinding(id);
        return true;
    }

    bool InputActionsEditorModel::AddComposite(const Guid& map, const Guid& action,
                                                std::string composite)
    {
        if (composite != "1DAxis" && composite != "2DVector") return false;
        auto next = draft_;
        auto* owner = FindChild(next, map, "actions", action);
        if (!owner || !owner->contains("bindings") || !(*owner)["bindings"].is_array()) return false;
        const auto id = Guid::Generate();
        nlohmann::json parts = nlohmann::json::array();
        const std::vector<std::string> names = composite == "1DAxis"
            ? std::vector<std::string>{"negative", "positive"}
            : std::vector<std::string>{"up", "down", "left", "right"};
        for (const auto& name : names)
            parts.push_back({{"id", Guid::Generate().ToString()}, {"name", name},
                             {"path", "<Keyboard>/space"}});
        (*owner)["bindings"].push_back({{"id", id.ToString()}, {"composite", composite},
                                         {"parts", std::move(parts)}});
        if (!ApplyEdit("Add composite binding", draft_, next)) return false;
        SelectMap(map); SelectAction(action); SelectBinding(id);
        return true;
    }

    bool InputActionsEditorModel::RemoveBinding(const Guid& map, const Guid& action,
                                                 const Guid& binding)
    {
        auto next = draft_;
        auto* owner = FindChild(next, map, "actions", action);
        if (!owner || !RemoveChild(next, action, "bindings", binding)) return false;
        if (!ApplyEdit("Remove binding", draft_, next)) return false;
        SelectMap(map); SelectAction(action);
        return true;
    }

    bool InputActionsEditorModel::AddPart(const Guid& binding, std::string role, std::string path)
    {
        auto next = draft_;
        auto* owner = FindId(next, binding);
        if (!owner || !owner->contains("composite") || !owner->contains("parts") ||
            !(*owner)["parts"].is_array() || path.empty()) return false;
        const auto composite = owner->value("composite", std::string{});
        const bool valid = composite == "1DAxis"
            ? role == "negative" || role == "positive"
            : composite == "2DVector" &&
              (role == "up" || role == "down" || role == "left" || role == "right");
        if (!valid) return false;
        const auto id = Guid::Generate();
        (*owner)["parts"].push_back({{"id", id.ToString()}, {"name", std::move(role)},
                                     {"path", std::move(path)}});
        if (!ApplyEdit("Add composite part", draft_, next)) return false;
        SelectBinding(binding); SelectPart(id);
        return true;
    }

    bool InputActionsEditorModel::RemovePart(const Guid& binding, const Guid& part)
    {
        auto next = draft_;
        auto* owner = FindId(next, binding);
        if (!owner || !owner->contains("parts") || !(*owner)["parts"].is_array() ||
            (*owner)["parts"].size() <= 1 || !RemoveChild(next, binding, "parts", part)) return false;
        if (!ApplyEdit("Remove composite part", draft_, next)) return false;
        SelectPart({});
        return true;
    }

    bool InputActionsEditorModel::DuplicateRow(const Guid& id)
    {
        auto next = draft_;
        return DuplicateInArray(next, id) && ApplyEdit("Duplicate input row", draft_, next);
    }

    bool InputActionsEditorModel::MoveRow(const Guid& id, int direction)
    {
        if (direction != -1 && direction != 1) return false;
        auto next = draft_;
        return MoveInArray(next, id, direction) && ApplyEdit("Reorder input row", draft_, next);
    }

    bool InputActionsEditorModel::SetField(const Guid& id, std::string key, nlohmann::json value)
    {
        if (key == "id" || key.empty()) return false;
        auto next = draft_;
        auto* node = FindId(next, id);
        if (!node) return false;
        (*node)[key] = std::move(value);
        return ApplyEdit("Edit " + key, draft_, next);
    }

    bool InputActionsEditorModel::SetDefaultMap(const Guid& map)
    {
        auto next = draft_;
        if (!FindId(next, map) || !next.contains("actionMaps")) return false;
        bool found = false;
        for (const auto& row : next["actionMaps"])
            if (row.value("id", std::string{}) == map.ToString()) found = true;
        if (!found) return false;
        next["defaultMap"] = map.ToString();
        return ApplyEdit("Select default action map", draft_, next);
    }

    bool InputActionsEditorModel::AddScheme(std::string name, std::string group)
    {
        if (name.empty() || group.empty() || !draft_.is_object() ||
            !draft_.contains("controlSchemes") || !draft_["controlSchemes"].is_array()) return false;
        auto next = draft_;
        next["controlSchemes"].push_back({{"id", Guid::Generate().ToString()},
                                          {"name", std::move(name)}, {"bindingGroup", std::move(group)}});
        return ApplyEdit("Add control scheme", draft_, next);
    }

    bool InputActionsEditorModel::EditScheme(const Guid& scheme, std::string name,
                                             std::string group)
    {
        if (name.empty() || group.empty() || !draft_.is_object() ||
            !draft_.contains("controlSchemes") || !draft_["controlSchemes"].is_array()) return false;
        auto next = draft_;
        auto* row = FindId(next["controlSchemes"], scheme);
        if (!row) return false;
        const auto oldGroup = row->value("bindingGroup", std::string{});
        (*row)["name"] = std::move(name);
        (*row)["bindingGroup"] = group;
        if (oldGroup != group && next.contains("actionMaps"))
        {
            auto replace = [&](auto&& self, nlohmann::json& node) -> void
            {
                if (node.is_object())
                {
                    if (node.contains("groups") && node["groups"].is_array())
                        for (auto& entry : node["groups"])
                            if (entry == oldGroup) entry = group;
                    for (auto& [key, child] : node.items()) self(self, child);
                }
                else if (node.is_array()) for (auto& child : node) self(self, child);
            };
            replace(replace, next["actionMaps"]);
        }
        return ApplyEdit("Edit control scheme", draft_, next);
    }

    bool InputActionsEditorModel::RemoveScheme(const Guid& scheme)
    {
        if (!draft_.is_object() || !draft_.contains("controlSchemes") || !draft_["controlSchemes"].is_array()) return false;
        auto next = draft_;
        auto& schemes = next["controlSchemes"];
        for (size_t i = 0; i < schemes.size(); ++i)
        {
            if (schemes[i].value("id", std::string{}) != scheme.ToString()) continue;
            const auto group = schemes[i].value("bindingGroup", std::string{});
            schemes.erase(schemes.begin() + i);
            auto strip = [&](auto&& self, nlohmann::json& node) -> void
            {
                if (node.is_object())
                {
                    if (node.contains("groups") && node["groups"].is_array())
                    {
                        auto& groups = node["groups"];
                        for (size_t j = groups.size(); j > 0; --j)
                            if (groups[j - 1] == group) groups.erase(groups.begin() + j - 1);
                    }
                    for (auto& [key, child] : node.items()) self(self, child);
                }
                else if (node.is_array()) for (auto& child : node) self(self, child);
            };
            strip(strip, next["actionMaps"]);
            return ApplyEdit("Remove control scheme", draft_, next);
        }
        return false;
    }

    std::vector<std::string> InputActionsEditorModel::Warnings() const
    {
        std::vector<std::string> warnings;
        if (!preview_) return warnings;
        for (const auto& map : preview_->actionMaps)
        {
            std::set<std::pair<std::string, std::string>> seen;
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    auto check = [&](const std::string& path, const std::vector<std::string>& groups)
                    {
                        if (path.find('>') == std::string::npos || path.empty())
                            warnings.push_back("Unrecognized control path: " + path);
                        const auto effective = groups.empty() ? std::vector<std::string>{"*"} : groups;
                        for (const auto& group : effective)
                            if (!seen.emplace(group, path).second)
                                warnings.push_back("Conflicting " + path + " binding in " + map.name);
                    };
                    if (binding.composite.empty()) check(binding.path, binding.groups);
                    else for (const auto& part : binding.parts) check(part.path,
                        part.groups.empty() ? binding.groups : part.groups);
                }
        }
        return warnings;
    }
}
