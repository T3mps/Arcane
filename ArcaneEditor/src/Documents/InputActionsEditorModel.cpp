#include "Documents/InputActionsEditorModel.hpp"

#include "Documents/InputActionsJson.hpp"

#include <Arcane/Edit/CommandStack.hpp>
#include <Arcane/Input/InputActions.hpp>

#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <set>
#include <string_view>

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
                             nlohmann::json after, std::string undoKey)
                : anchor_(std::move(anchor)), label_(std::move(label)),
                  before_(std::move(before)), after_(std::move(after)),
                  undoKey_(std::move(undoKey)) {}
            // The selection is restored LIVE, trimmed to the deepest surviving
            // ancestor (a deleted binding falls back to its action), and
            // SILENTLY: undo/redo is a mechanical change, not a gesture, so
            // Ctrl+Z never steals the Inspector from the scene. The redo-side
            // key is captured AT UNDO TIME because callers such as AddBinding
            // select the new row only after ApplyEdit returns.
            void Undo() override
            {
                if (auto* m = Model()) { redoKey_ = m->SelectionKey(); m->RestoreDraft(before_); m->RestoreSelectionOrAncestor(undoKey_); }
            }
            void Redo() override
            {
                if (auto* m = Model()) { undoKey_ = m->SelectionKey(); m->RestoreDraft(after_); m->RestoreSelectionOrAncestor(redoKey_); }
            }
            const char* Label() const override { return label_.c_str(); }
            bool AffectsScene() const override { return false; }
            bool IsExpired() const override { return Model() == nullptr; }   // ~InputActionsEditorModel nulls the anchor
        private:
            [[nodiscard]] InputActionsEditorModel* Model() const
            {
                const auto alive = anchor_.lock();
                return alive ? *alive : nullptr;
            }
            std::weak_ptr<InputActionsEditorModel*> anchor_;
            std::string label_;
            nlohmann::json before_;
            nlohmann::json after_;
            std::string undoKey_;
            std::string redoKey_;
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

        const nlohmann::json* FindId(const nlohmann::json& node, const Guid& id)
        {
            if (node.is_object())
            {
                if (node.contains("id") && node["id"].is_string() &&
                    node["id"].get<std::string>() == id.ToString()) return &node;
                for (const auto& [key, value] : node.items())
                    if (const auto* found = FindId(value, id)) return found;
            }
            else if (node.is_array())
                for (const auto& value : node)
                    if (const auto* found = FindId(value, id)) return found;
            return nullptr;
        }

        nlohmann::json* FindChild(nlohmann::json& root, const Guid& parent,
                                  const char* array, const Guid& child)
        {
            auto* node = FindId(root, parent);
            if (!node || !node->contains(array) || !(*node)[array].is_array()) return nullptr;
            for (auto& item : (*node)[array])
                if (IdIs(item, child))
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
                if (IdIs(rows[i], child))
                { rows.erase(rows.begin() + i); return true; }
            return false;
        }

        bool MoveInArray(nlohmann::json& node, const Guid& id, int direction)
        {
            if (node.is_array())
            {
                for (size_t i = 0; i < node.size(); ++i)
                {
                    if (!IdIs(node[i], id)) continue;
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

        bool MoveToInArray(nlohmann::json& node, const Guid& id, std::size_t index)
        {
            if (node.is_array())
            {
                for (std::size_t i = 0; i < node.size(); ++i)
                    if (IdIs(node[i], id))
                    {
                        if (index >= node.size()) index = node.size() - 1;
                        if (index == i) return false;
                        nlohmann::json row = std::move(node[i]);
                        node.erase(node.begin() + static_cast<std::ptrdiff_t>(i));
                        node.insert(node.begin() + static_cast<std::ptrdiff_t>(index), std::move(row));
                        return true;
                    }
                for (auto& child : node) if (MoveToInArray(child, id, index)) return true;
            }
            else if (node.is_object())
                for (auto& [key, child] : node.items()) if (MoveToInArray(child, id, index)) return true;
            return false;
        }

        // base, "base 2", "base 3"... until no sibling's "name" equals it: the
        // runtime keys maps and actions by name (InputActions.cpp:757-775), so
        // a default or a copy must never collide with a sibling.
        std::string UniqueSiblingName(const nlohmann::json& siblings, std::string base)
        {
            auto used = [&](const std::string& candidate) {
                if (!siblings.is_array()) return false;
                for (const auto& s : siblings)
                    if (s.is_object() && Str(s, "name") == candidate) return true;
                return false; };
            if (!used(base)) return base;
            for (int n = 2;; ++n)
            {
                const std::string candidate = base + " " + std::to_string(n);
                if (!used(candidate)) return candidate;
            }
        }

        bool DuplicateInArray(nlohmann::json& node, const Guid& id)
        {
            if (node.is_array())
            {
                for (size_t i = 0; i < node.size(); ++i)
                {
                    if (!IdIs(node[i], id)) continue;
                    auto copy = node[i];
                    RefreshIds(copy);
                    if (copy.contains("name") && copy["name"].is_string() &&
                        !copy.contains("path"))
                        copy["name"] = UniqueSiblingName(node, copy["name"].get<std::string>() + " Copy");
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

        // Every entry names a scheme's bindingGroup, once: the loader rejects an
        // unknown or a duplicate group (InputActionAsset.cpp:73-87).
        bool GroupsKnown(const nlohmann::json& draft, const std::vector<std::string>& groups)
        {
            for (std::size_t i = 0; i < groups.size(); ++i)
            {
                if (!SchemeGroupExists(draft, groups[i])) return false;
                if (std::find(groups.begin(), groups.begin() + static_cast<std::ptrdiff_t>(i), groups[i]) != groups.begin() + static_cast<std::ptrdiff_t>(i)) return false;
            }
            return true;
        }
        bool ValidPartRole(std::string_view composite, std::string_view role)
        {
            if (composite == "1DAxis")   return role == "negative" || role == "positive";
            if (composite == "2DVector") return role == "up" || role == "down" || role == "left" || role == "right";
            return false;
        }
    }

    InputActionsEditorModel::InputActionsEditorModel(nlohmann::json draft, UndoResolver undo)
        : draft_(std::move(draft)), saved_(draft_), undo_(std::move(undo)),
          anchor_(std::make_shared<InputActionsEditorModel*>(this))
    { Validate(); }

    InputActionsEditorModel::~InputActionsEditorModel() { *anchor_ = nullptr; }

    void InputActionsEditorModel::Validate()
    {
        ++draftRevision_;
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

    void InputActionsEditorModel::SelectMap(const Guid& map) noexcept
    {
        const bool changed = selectedMap_ != map || selectedAction_.IsValid() ||
                             selectedBinding_.IsValid() || selectedPart_.IsValid();
        selectedMap_ = map; selectedAction_ = {}; selectedBinding_ = {}; selectedPart_ = {};
        if (map.IsValid() || changed) ++selectionEpoch_;   // a re-select IS a gesture; a clear bumps only when it changes something
    }
    void InputActionsEditorModel::SelectAction(const Guid& action) noexcept
    { const bool changed = selectedAction_ != action; selectedAction_ = action; if (action.IsValid() || changed) ++selectionEpoch_; }
    void InputActionsEditorModel::SelectBinding(const Guid& binding) noexcept
    { const bool changed = selectedBinding_ != binding || selectedPart_.IsValid(); selectedBinding_ = binding; selectedPart_ = {}; if (binding.IsValid() || changed) ++selectionEpoch_; }
    void InputActionsEditorModel::SelectPart(const Guid& part) noexcept
    { const bool changed = selectedPart_ != part; selectedPart_ = part; if (part.IsValid() || changed) ++selectionEpoch_; }

    std::string InputActionsEditorModel::SelectionKey() const
    { return EncodeSelectionKey({ selectedMap_, selectedAction_, selectedBinding_, selectedPart_ }); }

    std::optional<std::array<Guid, 4>> InputActionsEditorModel::ResolveKey(std::string_view key) const
    {
        auto ids = ParseSelectionKey(key);
        if (!ids) return std::nullopt;
        for (const Guid& g : *ids) if (g.IsValid() && !FindNode(g)) return std::nullopt;
        return ids;
    }
    bool InputActionsEditorModel::Resolves(std::string_view key) const { return ResolveKey(key).has_value(); }
    bool InputActionsEditorModel::RestoreSelection(std::string_view key)
    {
        const auto ids = ResolveKey(key);
        if (!ids) return false;
        SelectMap((*ids)[0]);
        if ((*ids)[1].IsValid()) SelectAction((*ids)[1]);
        if ((*ids)[2].IsValid()) SelectBinding((*ids)[2]);
        if ((*ids)[3].IsValid()) SelectPart((*ids)[3]);
        return true;
    }
    void InputActionsEditorModel::SetSelectionSilently(const std::array<Guid, 4>& ids)
    { selectedMap_ = ids[0]; selectedAction_ = ids[1]; selectedBinding_ = ids[2]; selectedPart_ = ids[3]; }   // NO epoch bump: undo/redo is not a gesture
    void InputActionsEditorModel::RestoreSelectionOrAncestor(std::string_view key)
    {
        // Strict parse (a malformed key selects nothing), then keep the deepest
        // chain of levels that still exist: a deleted binding falls back to its action.
        std::array<Guid, 4> raw{};
        if (const auto ids = ParseSelectionKey(key)) raw = *ids;
        std::array<Guid, 4> keep{};
        for (std::size_t level = 0; level < 4; ++level)
        {
            if (!raw[level].IsValid() || !FindNode(raw[level])) break;
            keep[level] = raw[level];
        }
        SetSelectionSilently(keep);
    }
    // The map and action that own `binding` in the draft; false when none.
    bool InputActionsEditorModel::OwnerOfBinding(const Guid& binding, Guid& map, Guid& action) const
    {
        if (!draft_.is_object() || !draft_.contains("actionMaps") || !draft_["actionMaps"].is_array()) return false;
        for (const auto& m : draft_["actionMaps"])
        {
            if (!m.is_object() || !m.contains("actions") || !m["actions"].is_array()) continue;
            for (const auto& a : m["actions"])
            {
                if (!a.is_object() || !a.contains("bindings") || !a["bindings"].is_array()) continue;
                for (const auto& b : a["bindings"])
                    if (IdIs(b, binding)) { map = IdOf(m); action = IdOf(a); return map.IsValid() && action.IsValid(); }
            }
        }
        return false;
    }
    const nlohmann::json* InputActionsEditorModel::FindNode(const Guid& id) const { return FindId(draft_, id); }

    bool InputActionsEditorModel::SelectByPath(std::string_view namePath)
    {
        // A name segment that matches more than one sibling is refused: the
        // runtime keys maps and actions by name and refuses such a file
        // (InputActions.cpp:757-775).
        const auto segs = SplitKey(namePath);
        if (segs.empty() || segs.size() > 4 || segs[0].empty() || !draft_.is_object() || !draft_.contains("actionMaps") || !draft_["actionMaps"].is_array())
            return false;
        auto idOf = [](const nlohmann::json& row) { return IdOf(row); };
        // The ONE sibling named `name`; nullptr when none or more than one match.
        auto unique = [](const nlohmann::json& siblings, const std::string& name) -> const nlohmann::json* {
            const nlohmann::json* hit = nullptr;
            for (const auto& s : siblings)
            {
                if (!s.is_object() || Str(s, "name") != name) continue;
                if (hit) return nullptr;   // ambiguous
                hit = &s;
            }
            return hit; };
        const nlohmann::json* map = unique(draft_["actionMaps"], std::string(segs[0]));
        if (!map || !idOf(*map).IsValid()) return false;
        const nlohmann::json* action = nullptr;
        if (segs.size() > 1)
        {
            if (!map->contains("actions") || !(*map)["actions"].is_array()) return false;
            action = unique((*map)["actions"], std::string(segs[1]));
            if (!action || !idOf(*action).IsValid()) return false;
        }
        auto index = [](const std::string& s, const nlohmann::json& arr) -> const nlohmann::json* {
            if (s.empty() || s.size() > 9 || !arr.is_array() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return nullptr;
            const std::size_t i = std::stoul(s);
            return i < arr.size() ? &arr[i] : nullptr; };
        const nlohmann::json* binding = nullptr;
        if (segs.size() > 2)
        {
            binding = action->contains("bindings") ? index(std::string(segs[2]), (*action)["bindings"]) : nullptr;
            if (!binding || !idOf(*binding).IsValid()) return false;
        }
        const nlohmann::json* part = nullptr;
        if (segs.size() > 3)
        {
            part = binding->contains("parts") ? index(std::string(segs[3]), (*binding)["parts"]) : nullptr;
            if (!part || !idOf(*part).IsValid()) return false;
        }
        SelectMap(idOf(*map));
        if (action) SelectAction(idOf(*action));
        if (binding) SelectBinding(idOf(*binding));
        if (part) SelectPart(idOf(*part));
        return true;
    }

    bool InputActionsEditorModel::ApplyEdit(std::string label, nlohmann::json before,
                                             nlohmann::json after)
    {
        if (before != draft_ || before == after) return false;
        std::string undoKey = SelectionKey();   // what was selected BEFORE the edit; ApplyEdit itself never touches the selection
        RestoreDraft(after);
        if (Arcane::CommandStack* stack = undo_ ? undo_() : nullptr)
            stack->Push(std::make_unique<DraftEditCommand>(anchor_, std::move(label),
                                                            std::move(before), std::move(after),
                                                            std::move(undoKey)));
        return true;
    }

    bool InputActionsEditorModel::Undo()
    {
        Arcane::CommandStack* stack = undo_ ? undo_() : nullptr;
        if (!stack || !stack->CanUndo()) return false;
        stack->Undo();
        return true;
    }

    bool InputActionsEditorModel::Redo()
    {
        Arcane::CommandStack* stack = undo_ ? undo_() : nullptr;
        if (!stack || !stack->CanRedo()) return false;
        stack->Redo();
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
            if (!IdIs(map, mapId) ||
                !map.contains("actions") || !map["actions"].is_array()) continue;
            for (size_t i = 0; i < map["actions"].size(); ++i)
            {
                if (!IdIs(map["actions"][i], actionId)) continue;
                auto duplicate = map["actions"][i];
                RefreshIds(duplicate);
                const std::string base = duplicate.contains("name") && duplicate["name"].is_string() ? Str(duplicate, "name") : std::string("Action");
                duplicate["name"] = UniqueSiblingName(map["actions"], base + " Copy");
                map["actions"].insert(map["actions"].begin() + i + 1, std::move(duplicate));
                return ApplyEdit("Duplicate action", draft_, next);
            }
        }
        return false;
    }

    bool InputActionsEditorModel::AddMap(std::string name)
    {
        name = TrimName(name);
        if (name.empty() || !draft_.is_object() || !draft_.value("actionMaps", nlohmann::json{}).is_array()) return false;
        auto next = draft_;
        const auto id = Guid::Generate();
        next["actionMaps"].push_back({{"id", id.ToString()}, {"name", UniqueSiblingName(next["actionMaps"], std::move(name))},
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
            if (!IdIs(rows[i], map)) continue;
            rows.erase(rows.begin() + i);
            // The survivor's id is whatever the text holds: a malformed one
            // (a hand edit) neither becomes the default nor the selection.
            const Guid survivor = rows.empty() ? Guid{} : IdOf(rows[0]);
            if (Str(next, "defaultMap") == map.ToString())
            {
                if (!survivor.IsValid()) next.erase("defaultMap");
                else next["defaultMap"] = survivor.ToString();
            }
            if (!ApplyEdit("Remove action map", draft_, next)) return false;
            SelectMap(survivor);
            return true;
        }
        return false;
    }

    bool InputActionsEditorModel::AddAction(const Guid& map, std::string name)
    {
        name = TrimName(name);
        if (name.empty()) return false;
        auto next = draft_;
        auto* owner = FindId(next, map);
        if (!owner || !owner->contains("actions") || !(*owner)["actions"].is_array()) return false;
        const auto id = Guid::Generate();
        (*owner)["actions"].push_back({{"id", id.ToString()}, {"name", UniqueSiblingName((*owner)["actions"], std::move(name))},
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

    bool InputActionsEditorModel::AddBinding(const Guid& map, const Guid& action, std::string path,
                                              std::vector<std::string> groups)
    {
        if (path.empty() || !GroupsKnown(draft_, groups)) return false;
        auto next = draft_;
        auto* owner = FindChild(next, map, "actions", action);
        if (!owner || !owner->contains("bindings") || !(*owner)["bindings"].is_array()) return false;
        const auto id = Guid::Generate();
        nlohmann::json row = {{"id", id.ToString()}, {"path", std::move(path)}};
        if (!groups.empty()) row["groups"] = std::move(groups);   // the loader's shape (InputActionAsset.cpp:73-87)
        (*owner)["bindings"].push_back(std::move(row));
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

    bool InputActionsEditorModel::AddComposite(const Guid& map, const Guid& action, std::string composite,
                                                std::vector<std::pair<std::string, std::string>> parts,
                                                std::vector<std::string> groups)
    {
        if ((composite != "1DAxis" && composite != "2DVector") || parts.empty() || !GroupsKnown(draft_, groups)) return false;
        nlohmann::json rows = nlohmann::json::array();
        for (auto& [role, path] : parts)
        {
            if (path.empty() || !ValidPartRole(composite, role)) return false;
            rows.push_back({{"id", Guid::Generate().ToString()}, {"name", std::move(role)}, {"path", std::move(path)}});
        }
        auto next = draft_;
        auto* owner = FindChild(next, map, "actions", action);
        if (!owner || !owner->contains("bindings") || !(*owner)["bindings"].is_array()) return false;
        const auto id = Guid::Generate();
        nlohmann::json row = {{"id", id.ToString()}, {"composite", std::move(composite)}, {"parts", std::move(rows)}};
        if (!groups.empty()) row["groups"] = std::move(groups);
        (*owner)["bindings"].push_back(std::move(row));
        if (!ApplyEdit("Add composite binding", draft_, next)) return false;   // ONE step, whatever was captured
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
        if (!ValidPartRole(Str(*owner, "composite"), role)) return false;
        const auto id = Guid::Generate();
        (*owner)["parts"].push_back({{"id", id.ToString()}, {"name", std::move(role)},
                                     {"path", std::move(path)}});
        if (!ApplyEdit("Add composite part", draft_, next)) return false;
        Guid map, action;
        if (OwnerOfBinding(binding, map, action)) { SelectMap(map); SelectAction(action); }
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

    bool InputActionsEditorModel::SetPartPaths(const Guid& binding, std::vector<std::pair<Guid, std::string>> paths)
    {
        if (paths.empty()) return false;
        auto next = draft_;
        auto* owner = FindId(next, binding);
        if (!owner || !owner->contains("composite") || !owner->contains("parts") || !(*owner)["parts"].is_array()) return false;
        for (auto& [part, path] : paths)
        {
            if (path.empty()) return false;
            nlohmann::json* row = nullptr;
            for (auto& p : (*owner)["parts"]) if (IdIs(p, part)) { row = &p; break; }
            if (!row) return false;
            (*row)["path"] = std::move(path);
        }
        return ApplyEdit("Rebind composite", draft_, next);   // drafting pick 9.28 #41
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

    bool InputActionsEditorModel::MoveRowTo(const Guid& id, std::size_t index)
    {
        auto next = draft_;
        return MoveToInArray(next, id, index) && ApplyEdit("Reorder input row", draft_, next);
    }

    std::optional<std::string> InputActionsEditorModel::ValidateName(const nlohmann::json& draft, const Guid& id, std::string_view proposed)
    {
        const std::string name = TrimName(proposed);
        if (name.empty()) return "Names cannot be blank";
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return std::nullopt;
        auto taken = [&](const nlohmann::json& siblings) {
            for (const auto& s : siblings)
                if (s.is_object() && !IdIs(s, id) && TrimName(Str(s, "name")) == name) return true;
            return false; };
        for (const auto& map : draft["actionMaps"])
        {
            if (!map.is_object()) continue;
            if (IdIs(map, id))
                return taken(draft["actionMaps"]) ? std::optional<std::string>("A map named '" + name + "' already exists") : std::nullopt;
            if (!map.contains("actions") || !map["actions"].is_array()) continue;
            for (const auto& action : map["actions"])
                if (IdIs(action, id))
                    return taken(map["actions"]) ? std::optional<std::string>("Another action in this map is already named '" + name + "'") : std::nullopt;
        }
        return std::nullopt;   // a part role or a scheme: only the blank rule applies
    }

    bool InputActionsEditorModel::SiblingNameTaken(const Guid& id, std::string_view name) const
    {
        return !TrimName(name).empty() && ValidateName(draft_, id, name).has_value();
    }

    bool InputActionsEditorModel::SetField(const Guid& id, std::string key, nlohmann::json value)
    {
        if (key == "id" || key.empty()) return false;
        if (key == "name")
        {
            // A name commits trimmed and validated: a refused name is no edit
            // and no undo entry (the rename box keeps it live, Task 8).
            if (!value.is_string()) return false;
            const std::string name = TrimName(value.get<std::string>());
            if (ValidateName(draft_, id, name)) return false;
            value = name;
        }
        auto next = draft_;
        auto* node = FindId(next, id);
        if (!node) return false;
        (*node)[key] = std::move(value);
        return ApplyEdit("Edit " + key, draft_, next);
    }

    bool InputActionsEditorModel::SetDefaultMap(const Guid& map)
    {
        auto next = draft_;
        if (!FindMap(next, map)) return false;
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
        const auto oldGroup = Str(*row, "bindingGroup");
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
            if (!IdIs(schemes[i], scheme)) continue;
            const auto group = Str(schemes[i], "bindingGroup");
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
            if (next.contains("actionMaps")) strip(strip, next["actionMaps"]);
            return ApplyEdit("Remove control scheme", draft_, next);
        }
        return false;
    }

    std::vector<InputActionsEditorModel::BindingConflict> InputActionsEditorModel::Conflicts() const
    {
        std::vector<BindingConflict> out;
        if (!preview_) return out;
        struct Entry { Guid id; Guid action; std::string actionName; std::string path; std::string key; std::vector<std::string> groups; };
        for (const auto& map : preview_->actionMaps)
        {
            std::vector<Entry> entries;
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    if (binding.composite.empty())
                        entries.push_back({ binding.id, action.id, action.name, binding.path,
                                            InputActions::CanonicalControlKey(binding.path), binding.groups });
                    else
                        for (const auto& part : binding.parts)
                            entries.push_back({ part.id, action.id, action.name, part.path,
                                                InputActions::CanonicalControlKey(part.path),
                                                part.groups.empty() ? binding.groups : part.groups });
                }
            // Every scheme group in which BOTH bindings are live. Ungrouped is
            // live in every scheme: both ungrouped -> {"*"}; one ungrouped ->
            // the grouped side's groups; else the intersection.
            auto overlap = [](const Entry& a, const Entry& b) -> std::vector<std::string> {
                if (a.groups.empty() && b.groups.empty()) return { "*" };
                if (a.groups.empty()) return b.groups;
                if (b.groups.empty()) return a.groups;
                std::vector<std::string> out;
                for (const auto& g : a.groups)
                    if (std::find(b.groups.begin(), b.groups.end(), g) != b.groups.end()) out.push_back(g);
                return out; };
            auto schemeNames = [&](const std::vector<std::string>& groups) {
                std::string out;
                if (groups.size() == 1 && groups[0] == "*") return out;
                for (const auto& g : groups)
                {
                    std::string name = g;
                    for (const auto& s : preview_->controlSchemes) if (s.bindingGroup == g) { name = s.name; break; }
                    if (!out.empty()) out += ", ";
                    out += name;
                }
                return out; };
            for (std::size_t i = 0; i < entries.size(); ++i)
                for (std::size_t j = i + 1; j < entries.size(); ++j)
                {
                    if (entries[i].key.empty() || entries[i].key != entries[j].key) continue;   // compare the COMPILED control, not the spelling: the rebind capture writes the scancode form while assets author the keycode form
                    const auto groups = overlap(entries[i], entries[j]);
                    if (groups.empty()) continue;
                    const std::string group = groups.front(), scheme = schemeNames(groups);
                    out.push_back({ entries[i].id, entries[j].id, entries[j].action, entries[j].actionName, entries[i].path, group,
                                    entries[i].action, entries[i].actionName, map.id, map.name, scheme });
                    out.push_back({ entries[j].id, entries[i].id, entries[i].action, entries[i].actionName, entries[i].path, group,
                                    entries[j].action, entries[j].actionName, map.id, map.name, scheme });
                }
        }
        return out;
    }

    // A pure function of the draft (draft_/preview_, set only by Validate) plus SDL's key-name/layout state; the document memoises it on DraftRevision(), so a keyboard-layout change shows on the next edit.
    std::vector<std::string> InputActionsEditorModel::Warnings() const
    {
        std::vector<std::string> warnings;
        // Names first, from the DRAFT: a file loaded from disk with duplicate
        // names parses (FromJson keys ids, not names) but the runtime's
        // LoadAsset refuses it (InputActions.cpp:757-775) -- surface it in
        // Problems (Task 11 maps this prefix to `input.name.invalid`).
        if (draft_.is_object() && draft_.contains("actionMaps") && draft_["actionMaps"].is_array())
            for (const auto& map : draft_["actionMaps"])
            {
                if (!map.is_object()) continue;
                const Guid mapId = IdOf(map);
                const std::string mapName = Str(map, "name");
                if (mapId.IsValid())
                    if (const auto why = ValidateName(draft_, mapId, mapName))
                        warnings.push_back("Invalid name in " + mapName + ": " + *why);
                if (!map.contains("actions") || !map["actions"].is_array()) continue;
                for (const auto& action : map["actions"])
                {
                    if (!action.is_object()) continue;
                    const Guid actionId = IdOf(action);
                    const std::string actionName = Str(action, "name");
                    if (!actionId.IsValid()) continue;
                    if (const auto why = ValidateName(draft_, actionId, actionName))
                        warnings.push_back("Invalid name in " + mapName + "/" + actionName + ": " + *why);
                }
            }
        if (!preview_) return warnings;
        for (const auto& map : preview_->actionMaps)
            for (const auto& action : map.actions)
                for (const auto& binding : action.bindings)
                {
                    auto check = [&](const std::string& path) {
                        if (!InputActions::IsKnownControlPath(path))
                            warnings.push_back("Unknown control path '" + path + "' in " + map.name + "/" + action.name); };
                    if (binding.composite.empty()) check(binding.path);
                    else for (const auto& part : binding.parts) check(part.path);
                }
        std::set<std::pair<std::string, std::string>> seenPairs;
        for (const auto& c : Conflicts())
        {
            const auto a = c.binding.ToString(), b = c.otherBinding.ToString();
            if (!seenPairs.emplace(std::min(a, b), std::max(a, b)).second) continue;   // one line per pair
            const std::string where = c.group == "*" ? std::string(" in every scheme")
                                    : (c.scheme.find(", ") != std::string::npos ? " in schemes " : " in scheme ") + c.scheme;
            if (c.action == c.otherAction)
                warnings.push_back("Conflicting '" + c.path + "': " + c.mapName + "/" + c.actionName + " binds it twice" + where);
            else
                warnings.push_back("Conflicting '" + c.path + "': " + c.mapName + "/" + c.actionName + " and " +
                                   c.mapName + "/" + c.otherActionName + " share it" + where);
        }
        return warnings;
    }
}
