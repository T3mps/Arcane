#include "Documents/InputActionsDocument.hpp"

#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>

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
          guid_(DraftGuid(draft, path_)), model_(std::move(draft), commands),
          page_(model_, path_.filename().string(), path_.generic_string(),
                { [this](const Guid& id) { BeginRebind(id); }, &state_, &preview_ })
    {
        windowLabel_ = title_ + " (Input Actions)###inputdoc_" + guid_.ToString();
        diagKey_ = "input:" + guid_.ToString();
        SelectFirstMapAndAction();
    }

    InspectorPage* InputActionsDocument::PageFor(std::string_view key)
    {
        InputSelection sel;
        if (!key.empty())
        {
            std::array<Guid*, 4> slots{ &sel.map, &sel.action, &sel.binding, &sel.part };
            std::size_t start = 0;
            for (std::size_t level = 0; level < 4; ++level)
            {
                const std::size_t slash = key.find('/', start);
                const std::string_view seg = key.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
                if (level < 3 && slash == std::string_view::npos) return nullptr;
                if (!seg.empty())
                {
                    const auto id = Guid::FromString(std::string(seg));
                    if (!id || !id->IsValid()) return nullptr;
                    *slots[level] = *id;
                }
                if (slash == std::string_view::npos) break;
                start = slash + 1;
            }
            // Every named level must still exist (a pinned page for a deleted binding is "gone").
            auto exists = [&](const Guid& id) { return !id.IsValid() || model_.FindNode(id) != nullptr; };
            if (!exists(sel.map) || !exists(sel.action) || !exists(sel.binding) || !exists(sel.part)) return nullptr;
        }
        page_.SetSelection(sel);
        return &page_;   // an empty key IS a page: the asset page (spec A s3.1, container fallback)
    }

    // A freshly opened document shows its first map and that map's first
    // action selected, as Unity's Input Actions editor does: the hierarchy and
    // the inspector are the document, and an empty "Select an action map."
    // pane on open hides both. A malformed draft (no maps, no ids) selects
    // nothing, which the widgets already tolerate.
    void InputActionsDocument::SelectFirstMapAndAction()
    {
        const auto& draft = model_.Draft();
        if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()
            || draft["actionMaps"].empty())
            return;
        const auto& map = draft["actionMaps"][0];
        if (!map.is_object() || !map.contains("id") || !map["id"].is_string())
            return;
        const auto mapId = Guid::FromString(map["id"].get<std::string>());
        if (!mapId) return;
        model_.SelectMap(*mapId);
        if (map.contains("actions") && map["actions"].is_array() && !map["actions"].empty())
        {
            const auto& action = map["actions"][0];
            if (action.is_object() && action.contains("id") && action["id"].is_string())
                if (const auto actionId = Guid::FromString(action["id"].get<std::string>()))
                    model_.SelectAction(*actionId);
        }
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

    InputSnapshot InputActionsDocument::SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive)
    {
        InputSnapshot s = raw;
        s.wantCaptureMouse = false;
        s.wantCaptureKeyboard = anyItemActive;
        return s;
    }

    void InputActionsDocument::BeginRebind(const Guid& target)
    {
        if (!target.IsValid()) return;
        captureTarget_ = target;
        capture_.Begin(target, std::nullopt, 10.0f, previewSnapshot_);   // any device; the initiating control is not a capture (existing rule)
    }

    void InputActionsDocument::TickCapture(bool bodyDrawn)
    {
        if (!captureTarget_.IsValid()) return;
        captureSwallowFrame_ = ImGui::GetFrameCount();   // every frame the capture is live, INCLUDING the completing/cancelling one
        // A capture is bound to the focused, visible document (UE's
        // SInputKeySelector ends selection on focus loss): a hidden tab, a
        // collapsed window or a click into the Viewport/Inspector cancels it, so
        // a key typed elsewhere can never land in a binding and the timeout
        // cannot freeze while the tab is hidden.
        //
        // Focus alone cannot see the click that moves it: ImGui applies
        // click-to-focus on a window background in EndFrame, AFTER this frame's
        // document draw, and the capture snapshot was sampled at frame start --
        // so on the click frame focused_ is still true and Observe would bind
        // the very button that clicked away (<Mouse>/leftButton). A click of
        // ANY button this frame outside the document window cancels instead.
        // The column children are NoInputs while a capture is live, so the
        // hover falls through to the document root: a click INSIDE the
        // document still binds (right-click over a column reads Right Button).
        bool clickedAway = false;
        if (bodyDrawn && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            for (int button = 0; button < ImGuiMouseButton_COUNT; ++button)
                if (ImGui::IsMouseClicked(button)) { clickedAway = true; break; }
        if (!bodyDrawn || !focused_ || clickedAway) capture_.Cancel();
        else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) capture_.Cancel();
        else capture_.Observe(SnapshotForCapture(previewSnapshot_, ImGui::IsAnyItemActive()),
                              ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f);
        const auto& result = capture_.Result();
        if (result.state == InputRebindState::Completed)
        {
            (void)model_.SetField(captureTarget_, "path", result.replacementPath);   // ONE undoable edit
            captureTarget_ = {};
        }
        else if (result.state == InputRebindState::Canceled || result.state == InputRebindState::TimedOut)
            captureTarget_ = {};
    }

    void InputActionsDocument::Draw(bool& requestClose)
    {
        bool open = true;
        // Tab dot = polled model state each frame (MeshDocument/SpriteDocument/
        // ShaderEditorDocument pattern): undo back to the saved revision clears
        // it with no bookkeeping.
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        const bool bodyDrawn = ImGui::Begin(windowLabel_.c_str(), &open, flags);
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);   // valid on both branches
        TickCapture(bodyDrawn);                                                      // BEFORE the shortcut: the swallow stamp is written here
        if (bodyDrawn)
        {
            if (!InputSwallowed() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S))
                if (!Save()) ARC_WARN("InputActionsDocument: save refused for '{}'", path_.generic_string());
            preview_.Sync(model_);
            if (state_.previewArmed)
            {
                InputSnapshot raw = previewSnapshot_;
                raw.wantCaptureKeyboard = false;
                raw.wantCaptureMouse = false;
                preview_.Update(raw);
            }
            if (!model_.Diagnostics().empty())
            {
                // The draft is not a valid asset: say so above the columns (the
                // columns draw what they can; a missing actionMaps draws nothing).
                ImGui::PushStyleColor(ImGuiCol_Text, Arcane::Editor::Theme::kAmber);
                ImGui::TextUnformatted(ICON_LC_TRIANGLE_ALERT);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextWrapped("%s -- fix it in a text editor (Assets > Open as text), then reopen.",
                                   model_.Diagnostics().front().c_str());
                ImGui::Separator();
            }
            InputActionsDocumentWidgets::Services services;
            services.beginRebind     = [this](const Guid& id) { BeginRebind(id); };
            services.isRebinding     = [this](const Guid& id) { return captureTarget_ == id; };
            services.rebindRemaining = [this] { return capture_.Remaining(); };
            services.glow            = [this](const Guid& id) { return state_.previewArmed ? preview_.BindingValue(id) : 0.0f; };
            services.inputSwallowed  = [this] { return InputSwallowed(); };
            widgets_.Draw(model_, state_, services);
            PublishWarnings();
        }
        ImGui::End();
        requestClose = !open;
    }

    void InputActionsDocument::PublishWarnings()
    {
        std::vector<std::string> warnings = model_.Warnings();
        if (warnings == publishedWarnings_) return;
        publishedWarnings_ = std::move(warnings);
        std::vector<Arcane::Diagnostic> diags;
        for (const auto& w : publishedWarnings_)
        {
            Arcane::Diagnostic d;
            d.severity = Arcane::DiagSeverity::Warning;
            d.scope = Arcane::DiagScope::Assets;
            // The model's three warning families (Task 6): unknown control
            // paths, invalid/duplicate names, binding conflicts.
            d.code = w.rfind("Unknown control path", 0) == 0 ? "input.path.unknown"
                   : w.rfind("Invalid name", 0) == 0         ? "input.name.invalid"
                                                             : "input.binding.conflict";
            d.message = w;
            d.detail = title_ + ".arcinput";
            d.locator = Arcane::DiagLocator::Asset(guid_);
            diags.push_back(std::move(d));
        }
        Arcane::Diagnostics::Publish(diagKey_, diags);
    }
}
