#include "Documents/InputActionsDocument.hpp"
#include "Input/EditorActions.hpp"
#include "Settings/EditorDocumentUiSettings.hpp"

#include "Documents/InputActionsJson.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Widgets/IconsLucide.h"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/Settings.hpp>

#include <imgui.h>
#include <imgui_internal.h>

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

        // The action whose bindings (or a composite's parts) carry `target`; nullptr when none.
        const nlohmann::json* OwnerActionOfBinding(const nlohmann::json& draft, const Guid& target)
        {
            if (!draft.is_object() || !draft.contains("actionMaps") || !draft["actionMaps"].is_array()) return nullptr;
            for (const auto& m : draft["actionMaps"])
            {
                if (!m.is_object() || !m.contains("actions") || !m["actions"].is_array()) continue;
                for (const auto& a : m["actions"])
                    if (a.is_object() && a.contains("bindings") && FindById(a["bindings"], target)) return &a;
            }
            return nullptr;
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
                                               UndoResolver undo)
        : path_(std::move(path)), title_(path_.stem().string()),
          guid_(DraftGuid(draft, path_)), model_(std::move(draft), std::move(undo)),
          page_(model_, path_.filename().string(), path_.generic_string(),
                { [this](const Guid& id) { BeginRebindFromPage(id); }, &state_, &preview_ })
    {
        windowLabel_ = title_ + " (Input Actions)###inputdoc_" + guid_.ToString();
        diagKey_ = "input:" + guid_.ToString();
        SelectFirstMapAndAction();
    }

    void InputActionsDocument::NoteMoved(const std::filesystem::path& p)
    {
        path_ = p;
        title_ = path_.stem().string();
        windowLabel_ = title_ + " (Input Actions)###inputdoc_" + guid_.ToString();
        page_.SetAssetLocation(path_.filename().string(), path_.generic_string());
    }

    InspectorPage* InputActionsDocument::PageFor(std::string_view key)
    {
        InputSelection sel;
        if (!key.empty())
        {
            const auto ids = model_.ResolveKey(key);   // the model's one grammar + existence
            if (!ids) return nullptr;                   // a pinned page for a deleted binding is "gone"
            sel = { (*ids)[0], (*ids)[1], (*ids)[2], (*ids)[3] };
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
        const std::filesystem::path& path, UndoResolver undo)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            ARC_WARN("InputActionsDocument: cannot open '{}'", path.generic_string());
            return nullptr;
        }
        stream.close();
        return std::unique_ptr<InputActionsDocument>(
            new InputActionsDocument(path, ReadDraft(path), std::move(undo)));
    }

    Guid InputActionsDocument::PeekGuid(const std::filesystem::path& path)
    { return DraftGuid(ReadDraft(path), path); }

    InputSnapshot InputActionsDocument::SnapshotForCapture(const InputSnapshot& raw, bool anyItemActive, bool pointerOnChrome)
    {
        InputSnapshot s = raw;
        s.wantCaptureMouse = pointerOnChrome;   // the document owns the pointer inside its content; its title bar, borders and grips belong to the window
        s.wantCaptureKeyboard = anyItemActive;
        return s;
    }

    bool InputActionsDocument::PressOnChrome(ImVec2 p, ImVec2 lo, ImVec2 hi, float pad) noexcept
    { return p.x < lo.x + pad || p.y < lo.y + pad || p.x > hi.x - pad || p.y > hi.y - pad; }

    void InputActionsDocument::BeginRebind(const Guid& target)
    {
        if (!target.IsValid() || pending_) return;   // one capture at a time: the page's Rebind... waits for the pending add
        captureTarget_ = target;
        capture_.Begin(target, std::nullopt, Arcane::Settings<InputEditorSettings>().rebindTimeoutSeconds, previewSnapshot_);   // any device; the initiating control is not a capture (existing rule)
    }

    void InputActionsDocument::BeginRebindFromPage(const Guid& target)
    {
        if (!target.IsValid()) return;
        focusRequest_ = true;                  // consumed before the next Begin: the cancel rule below stays intact
        state_.scrollRowToId = target;         // the countdown row, which a PINNED page's target need not be the selection of
        // A collapsed owner would hide the countdown row: expand it (view state, not a selection event).
        if (const auto* owner = OwnerActionOfBinding(model_.Draft(), target)) state_.collapsedActions.erase(IdOf(*owner).ToString());
        BeginRebind(target);
    }

    void InputActionsDocument::BeginPending(PendingAdd add)
    {
        if (captureTarget_.IsValid() || add.roles.empty()) return;
        if (add.action.IsValid()) state_.collapsedActions.erase(add.action.ToString());   // the ghost rows draw under the action
        pending_ = std::move(add);
        StartPendingCapture();
    }

    void InputActionsDocument::StartPendingCapture()
    {
        // Any non-nil id (InputRebindOperation.cpp:48); never a row's, so no row
        // shows the rebind countdown. InputSwallowed() covers every step.
        captureTarget_ = Guid::Generate();
        capture_.Begin(captureTarget_, std::nullopt, Arcane::Settings<InputEditorSettings>().rebindTimeoutSeconds, previewSnapshot_);
    }

    void InputActionsDocument::FinishPending()
    {
        PendingAdd done = std::move(*pending_);
        pending_.reset();
        captureTarget_ = {};
        if (done.captured.empty()) return;                 // Esc on the first part: nothing to add
        if (CommitPending(model_, done)) state_.scrollRowToSelection = true;
        else ARC_WARN("input: the pending add was refused (its action or composite is gone, or nothing changed); nothing added");
    }

    void InputActionsDocument::FlushGesture()
    {
        if (!pending_) return;
        capture_.Cancel();                                 // inert: FinishPending clears captureTarget_, so the next TickCapture early-outs
        FinishPending();
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
        // A press that began on this window's own chrome (title bar, close/
        // collapse buttons, resize border or grip) is the WINDOW's: claim it,
        // so it neither binds nor is heard later (Observe latches the held bit
        // into previous_ on the claimed frame). Docked, the tab belongs to the
        // host window and the clickedAway rule above already cancels.
        bool onChrome = false;
        if (bodyDrawn)
        {
            const ImRect inner = ImGui::GetCurrentWindow()->InnerRect;
            for (int b = 0; b < ImGuiMouseButton_COUNT; ++b)
                if (ImGui::IsMouseDown(b) && PressOnChrome(ImGui::GetIO().MouseClickedPos[b], inner.Min, inner.Max, ImGui::GetStyle().WindowBorderHoverPadding))
                    onChrome = true;
            onChrome = onChrome || ImGui::IsAnyItemActive();   // the grip corner sits inside InnerRect; the columns are NoInputs, so an active item here is window decoration
        }
        if (!bodyDrawn || !focused_ || clickedAway) capture_.Cancel();
        else if (EditorActions::Get().Pressed("ui.cancel")) capture_.Cancel();
        else capture_.Observe(SnapshotForCapture(previewSnapshot_, ImGui::IsAnyItemActive(), onChrome),
                              ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f);
        const auto& result = capture_.Result();
        if (result.state == InputRebindState::Completed)
        {
            if (pending_)
            {
                pending_->captured.push_back(result.replacementPath);
                if (pending_->Done()) FinishPending();
                else StartPendingCapture();                // the next role; the completing control is held at Begin, so it is ignored
            }
            else
            {
                (void)model_.SetField(captureTarget_, "path", result.replacementPath);   // ONE undoable edit
                captureTarget_ = {};
            }
        }
        else if (result.state == InputRebindState::Canceled || result.state == InputRebindState::TimedOut)
        {
            if (pending_) FinishPending();                 // commits the parts heard so far (a partial composite is legal)
            else captureTarget_ = {};
        }
        // The capture ended on a frame the body is not drawn: DrawActions (which
        // owns the one-shot's clear) does not run, so drop the page Rebind's
        // scroll-to-row here, or it would scroll a row later with no capture live.
        if (!bodyDrawn && !captureTarget_.IsValid()) state_.scrollRowToId = {};
    }

    void InputActionsDocument::Draw(bool& requestClose)
    {
        bool open = true;
        // Tab dot = polled model state each frame (MeshDocument/SpriteDocument/
        // ShaderEditorDocument pattern): undo back to the saved revision clears
        // it with no bookkeeping.
        const ImGuiWindowFlags flags = Dirty() ? ImGuiWindowFlags_UnsavedDocument : 0;
        if (focusRequest_) { ImGui::SetNextWindowFocus(); focusRequest_ = false; }   // the page's Rebind... (BeginRebindFromPage)
        const bool bodyDrawn = ImGui::Begin(windowLabel_.c_str(), &open, flags);
        focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);   // valid on both branches
        if (focused_) EditorActions::Get().MarkContextActive(ActionContext::Document);
        TickCapture(bodyDrawn);                                                      // BEFORE the shortcut: the swallow stamp is written here
        if (bodyDrawn)
        {
            if (!InputSwallowed() && focused_ && EditorActions::Get().Pressed("document.save"))
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
            services.beginAdd        = [this](PendingAdd add) { BeginPending(std::move(add)); };
            services.pending         = [this]() -> const PendingAdd* { return Pending(); };
            widgets_.Draw(model_, state_, services);
        }
        ImGui::End();
        requestClose = !open;
    }

    void InputActionsDocument::PublishWarnings()
    {
        if (model_.DraftRevision() == publishedRevision_) return;   // Warnings() runs once per draft revision, not per frame
        publishedRevision_ = model_.DraftRevision();
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
