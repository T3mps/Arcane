#include "Documents/InputActionsDocument.hpp"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace Arcane::Editor
{
    namespace
    {
        nlohmann::json ReadDraft(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) return nullptr;
            const std::string raw(std::istreambuf_iterator<char>{stream}, {});
            auto parsed = nlohmann::json::parse(raw, nullptr, false);
            return parsed.is_discarded() ? nlohmann::json(raw) : parsed;
        }

        Guid DraftGuid(const nlohmann::json& draft, const std::filesystem::path& path)
        {
            if (draft.is_object() && draft.contains("id") && draft["id"].is_string())
                if (const auto id = Guid::FromString(draft["id"].get<std::string>()); id && id->IsValid())
                    return *id;
            return Guid::FromName(Guid::Nil(), path.generic_string());
        }
    }

    InputActionsDocument::InputActionsDocument(std::filesystem::path path,
                                               nlohmann::json draft,
                                               Arcane::CommandStack* commands)
        : path_(std::move(path)), title_(path_.stem().string()),
          guid_(DraftGuid(draft, path_)), model_(std::move(draft), commands)
    {
        windowLabel_ = title_ + " (Input Actions)###inputdoc_" + guid_.ToString();
        RefreshText();
    }

    std::unique_ptr<InputActionsDocument> InputActionsDocument::Open(
        const std::filesystem::path& path, Arcane::CommandStack* commands)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            ARC_WARN("InputActionsDocument: cannot open '{}'", path.generic_string());
            return nullptr;
        }
        stream.close();
        return std::unique_ptr<InputActionsDocument>(
            new InputActionsDocument(path, ReadDraft(path), commands));
    }

    Guid InputActionsDocument::PeekGuid(const std::filesystem::path& path)
    { return DraftGuid(ReadDraft(path), path); }

    void InputActionsDocument::RefreshText()
    {
        const auto text = model_.Draft().is_string()
            ? model_.Draft().get<std::string>() : model_.Draft().dump(2);
        const auto count = std::min(text.size(), text_.size() - 1);
        std::memcpy(text_.data(), text.data(), count);
        text_[count] = '\0';
    }

    void InputActionsDocument::Draw(bool& requestClose)
    {
        bool open = true;
        if (ImGui::Begin(windowLabel_.c_str(), &open))
        {
            focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            if (ImGui::Button("Save") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
                if (!Save()) ARC_WARN("InputActionsDocument: save refused for '{}'", path_.generic_string());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", Dirty() ? "Unsaved changes" : "Saved");
            for (const auto& diagnostic : model_.Diagnostics())
                ImGui::TextWrapped("%s", diagnostic.c_str());
            if (ImGui::InputTextMultiline("##input_actions_json", text_.data(), text_.size(),
                ImGui::GetContentRegionAvail()))
            {
                auto next = nlohmann::json::parse(text_.data(), nullptr, false);
                if (next.is_discarded()) next = std::string(text_.data());
                (void)model_.ApplyEdit("Edit input actions", model_.Draft(), std::move(next));
            }
        }
        else
        {
            focused_ = false;
        }
        ImGui::End();
        requestClose = !open;
    }
}
