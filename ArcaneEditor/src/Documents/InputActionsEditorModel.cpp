#include "Documents/InputActionsEditorModel.hpp"

#include <Arcane/Edit/CommandStack.hpp>

#include <fstream>

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
}
