#pragma once

#include "Documents/InputActionsEditorModel.hpp"
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/InputRebindOperation.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace Arcane::Editor
{
    // Per-document ImGui editing state. The asset model remains independent of ImGui.
    class InputActionsDocumentWidgets
    {
    public:
        void Draw(InputActionsEditorModel& model, const InputSnapshot& snapshot);

    private:
        bool TextField(const char* label, const std::string& value,
                       const std::function<void(std::string)>& commit);
        std::unordered_map<unsigned int, std::string> fieldDrafts_;
        char schemeName_[64] = "Gamepad";
        char schemeGroup_[64] = "Gamepad";
        std::unique_ptr<InputActions> preview_;
        nlohmann::json previewSource_;
        InputRebindOperation capture_;
        Guid captureTarget_;
        int captureDevice_ = 0;
    };
}
