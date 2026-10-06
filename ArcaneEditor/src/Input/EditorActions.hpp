#pragma once

// Editor actions (settings arc S4, spec s7.2): every editor keyboard command is
// a named action whose chord is the String cvar editor.keys.<id> (widget
// "keychord", PreferencesMachine). "Ctrl+Z" is LABELLED (the key that prints Z
// on the user's layout, the default); "[KeyW]" is PHYSICAL (the key in W's
// QWERTY position: the fly camera, the QWER tool row). Display always uses the
// current layout's labels.

#include <Arcane/Config/CVarHandle.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class KeyLayout; class CVarRegistry; }

namespace Arcane::Editor
{
    enum class KeyType : std::uint8_t { Labelled, Physical };

    struct KeyChord
    {
        bool ctrl = false, shift = false, alt = false, super = false;
        KeyType type = KeyType::Labelled;
        std::int32_t key = 0;   // Labelled: an SDL keycode. Physical: an SDL scancode. 0 = unbound.
        bool operator==(const KeyChord&) const = default;
        [[nodiscard]] bool Bound() const noexcept { return key != 0; }
    };

    [[nodiscard]] std::optional<KeyChord> ParseKeyChord(std::string_view text);   // "" -> an unbound chord; nullopt = not a chord
    [[nodiscard]] std::string FormatKeyChord(const KeyChord& chord);              // the canonical cvar text
    [[nodiscard]] std::string DisplayKeyChord(const KeyChord& chord);             // the active layout's labels

    void SetActiveKeyLayout(const Arcane::KeyLayout* layout);   // null = QWERTY (headless tests)
    [[nodiscard]] const Arcane::KeyLayout& ActiveKeyLayout();

    [[nodiscard]] std::optional<std::string> ReservedChordReason(const KeyChord& chord);   // why the OS/ImGui owns it
    [[nodiscard]] KeyChord ToKeyType(const KeyChord& chord, KeyType type);                  // unchanged when no key maps
    [[nodiscard]] bool SameKey(const KeyChord& a, const KeyChord& b);                       // same modifiers, same key on this layout

    // ---- the action registry (S4-7) ----
    enum class ActionContext : std::uint8_t
    {
        Global, Viewport, Graph, AssetBrowser, Outliner, Inspector, Text, Console, InputDocument,
        Document,   // S4: a focused asset document (Ctrl+S saves IT, not the scene)
    };
    [[nodiscard]] const char* ActionContextName(ActionContext c) noexcept;
    [[nodiscard]] bool ContextsOverlap(ActionContext a, ActionContext b) noexcept;
    [[nodiscard]] int ContextSpecificity(ActionContext c) noexcept;    // Global 0 < Document 1 < panels 2 < Text 3
    [[nodiscard]] bool UsesImGuiRoute(ActionContext c) noexcept;       // all but Global and Viewport

    struct EditorActionDesc
    {
        std::string_view id, displayName;
        ActionContext context;
        std::string_view defaultChord;
    };

    struct ActionFrameInput
    {
        const Arcane::InputSnapshot* snap = nullptr;   // this frame's SDL sample (never kept)
        bool playMode = false;
        bool wantCaptureKeyboard = false;   // the editor ImGui context's
        bool wantTextInput = false;
        bool captureLive = false;           // an Input Actions rebind capture owns the keyboard
        bool viewportActive = false;
        double dt = 0.0;
        float repeatDelay = 0.275f;         // ImGui io defaults
        float repeatRate = 0.050f;
    };

    struct ChordCapture { KeyChord labelled; KeyChord physical; };

    class EditorActions
    {
    public:
        static EditorActions& Get();   // bound to CVarRegistry::Get(); registers the editor's table once
        explicit EditorActions(Arcane::CVarRegistry& registry);
        ~EditorActions();
        EditorActions(const EditorActions&) = delete;
        EditorActions& operator=(const EditorActions&) = delete;

        void Register(const EditorActionDesc& desc);
        [[nodiscard]] bool Pressed(std::string_view id) const;
        [[nodiscard]] bool PressedRepeat(std::string_view id) const;
        [[nodiscard]] bool Down(std::string_view id) const;
        [[nodiscard]] std::vector<std::string> ConflictsOf(std::string_view id) const;
        [[nodiscard]] std::string MenuShortcut(std::string_view id) const;

        void BeginFrame(const ActionFrameInput& in);
        void RefreshBindings();
        void MarkContextActive(ActionContext c) noexcept;
        void SetListening(bool listening) noexcept { m_listening = listening; }
        [[nodiscard]] bool Listening() const noexcept { return m_listening; }
        [[nodiscard]] std::optional<ChordCapture> CapturedChord() const noexcept { return m_capture; }

        [[nodiscard]] std::optional<KeyChord> ChordOf(std::string_view id) const;
        [[nodiscard]] std::optional<KeyChord> DefaultChordOf(std::string_view id) const;
        [[nodiscard]] const EditorActionDesc* Desc(std::string_view id) const;
        [[nodiscard]] std::vector<std::string_view> Ids() const;   // registration order
        [[nodiscard]] Arcane::CVarHandle HandleOf(std::string_view id) const;
        [[nodiscard]] Arcane::CVarRegistry& Registry() const noexcept { return m_registry; }
        [[nodiscard]] std::string ConflictTooltip(std::string_view id) const;   // "" when no conflict
        [[nodiscard]] std::string FiringActionOf(std::string_view id) const;    // who wins among id + its conflicts

    private:
        struct State;
        [[nodiscard]] const State* Find(std::string_view id) const;
        [[nodiscard]] bool Live(const State& s, bool imguiRoute) const;
        [[nodiscard]] bool Shadowed(const State& s) const;
        [[nodiscard]] bool ContextActive(ActionContext c) const noexcept;
        [[nodiscard]] bool Fired(const State& s, bool repeat) const;

        Arcane::CVarRegistry& m_registry;
        std::vector<std::unique_ptr<State>> m_states;
        std::map<std::string, std::size_t, std::less<>> m_index;
        ActionFrameInput m_frame{};
        Arcane::InputSnapshot m_snap{}, m_prevSnap{};
        bool m_hasFrame = false;
        bool m_listening = false;
        std::optional<ChordCapture> m_capture;
        std::uint32_t m_contextsNow = 0, m_contextsPrev = 0;
        std::uint64_t m_seq = 0;
    };

    void RegisterEditorActions(EditorActions& actions);   // the editor's table (S4-8)
}
