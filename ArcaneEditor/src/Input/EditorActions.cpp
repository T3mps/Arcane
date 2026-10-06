#include "Input/EditorActions.hpp"
#include "Viewport/ViewportInput.hpp"

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/KeyLayout.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    namespace Keys = Arcane::Keys;

    namespace
    {
        const Arcane::KeyLayout* g_layout = nullptr;

        bool EqualsNoCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
            return true;
        }

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
            return s;
        }

        // W3C UI Events `code` names for the physical keys a chord may name.
        std::string PhysicalName(std::uint32_t sc)
        {
            if (sc >= 4 && sc <= 29) return "Key" + std::string(1, static_cast<char>('A' + (sc - 4)));
            if (sc >= 30 && sc <= 38) return "Digit" + std::string(1, static_cast<char>('1' + (sc - 30)));
            if (sc == 39) return "Digit0";
            if (sc >= 58 && sc <= 69) return "F" + std::to_string(sc - 57);
            switch (sc)
            {
            case 40: return "Enter";       case 41: return "Escape";      case 42: return "Backspace";  case 43: return "Tab";
            case 44: return "Space";       case 45: return "Minus";       case 46: return "Equal";      case 47: return "BracketLeft";
            case 48: return "BracketRight";case 49: return "Backslash";   case 51: return "Semicolon";  case 52: return "Quote";
            case 53: return "Backquote";   case 54: return "Comma";       case 55: return "Period";     case 56: return "Slash";
            case 73: return "Insert";      case 74: return "Home";        case 75: return "PageUp";     case 76: return "Delete";
            case 77: return "End";         case 78: return "PageDown";    case 79: return "ArrowRight"; case 80: return "ArrowLeft";
            case 81: return "ArrowDown";   case 82: return "ArrowUp";     case 88: return "NumpadEnter";
            default: return {};
            }
        }

        std::optional<std::uint32_t> ParsePhysical(std::string_view name)
        {
            for (std::uint32_t sc = 1; sc < 512; ++sc)
                if (const std::string n = PhysicalName(sc); !n.empty() && EqualsNoCase(n, name)) return sc;
            return std::nullopt;
        }

        struct NamedKey { std::int32_t key; std::string_view canonical, display; };
        constexpr NamedKey kNamedKeys[] = {
            { Keys::kReturn, "Enter", "Enter" },        { Keys::kEscape, "Escape", "Esc" },      { Keys::kBackspace, "Backspace", "Backspace" },
            { Keys::kTab, "Tab", "Tab" },               { Keys::kSpace, "Space", "Space" },      { Keys::kDelete, "Delete", "Del" },
            { Keys::kInsert, "Insert", "Ins" },         { Keys::kHome, "Home", "Home" },         { Keys::kEnd, "End", "End" },
            { Keys::kPageUp, "PageUp", "PgUp" },        { Keys::kPageDown, "PageDown", "PgDn" }, { Keys::kLeft, "Left", "Left" },
            { Keys::kRight, "Right", "Right" },         { Keys::kUp, "Up", "Up" },               { Keys::kDown, "Down", "Down" },
            { Keys::kKeypadEnter, "KeypadEnter", "Keypad Enter" },
        };
        struct Alias { std::string_view text; std::int32_t key; };
        constexpr Alias kAliases[] = {
            { "Esc", Keys::kEscape }, { "Del", Keys::kDelete }, { "Return", Keys::kReturn }, { "Ins", Keys::kInsert },
            { "PgUp", Keys::kPageUp }, { "PgDn", Keys::kPageDown }, { "ArrowUp", Keys::kUp }, { "ArrowDown", Keys::kDown },
            { "ArrowLeft", Keys::kLeft }, { "ArrowRight", Keys::kRight }, { "Spacebar", Keys::kSpace },
        };

        std::string Utf8(std::int32_t cp)
        {
            std::string out;
            const auto u = static_cast<std::uint32_t>(cp);
            if (u < 0x80) out += static_cast<char>(u);
            else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 0x3F)); }
            else if (u < 0x10000) { out += static_cast<char>(0xE0 | (u >> 12)); out += static_cast<char>(0x80 | ((u >> 6) & 0x3F)); out += static_cast<char>(0x80 | (u & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (u >> 18)); out += static_cast<char>(0x80 | ((u >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((u >> 6) & 0x3F)); out += static_cast<char>(0x80 | (u & 0x3F)); }
            return out;
        }

        // One UTF-8 code point -> its value, or nullopt.
        std::optional<std::int32_t> SingleCodePoint(std::string_view s)
        {
            if (s.empty()) return std::nullopt;
            const auto b0 = static_cast<unsigned char>(s[0]);
            const std::size_t len = b0 < 0x80 ? 1 : (b0 >> 5) == 0x6 ? 2 : (b0 >> 4) == 0xE ? 3 : (b0 >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || s.size() != len) return std::nullopt;
            std::int32_t cp = len == 1 ? b0 : len == 2 ? (b0 & 0x1F) : len == 3 ? (b0 & 0x0F) : (b0 & 0x07);
            for (std::size_t i = 1; i < len; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
            return cp;
        }

        std::optional<std::int32_t> ParseLabelled(std::string_view name)
        {
            for (const NamedKey& k : kNamedKeys) if (EqualsNoCase(k.canonical, name)) return k.key;
            for (const Alias& a : kAliases) if (EqualsNoCase(a.text, name)) return a.key;
            if ((name.size() == 2 || name.size() == 3) && (name[0] == 'F' || name[0] == 'f'))
            {
                const int n = std::atoi(std::string(name.substr(1)).c_str());
                if (n >= 1 && n <= 12 && std::to_string(n) == name.substr(1)) return Keys::FKey(n);
            }
            if (name.size() == 1)
            {
                const auto c = static_cast<unsigned char>(name[0]);
                if (std::isalpha(c)) return std::tolower(c);
                if (c == '[') return std::nullopt;   // "[" alone is the start of a physical name
                if (c > 0x20 && c < 0x7F && c != '+') return c;
            }
            if (const auto cp = SingleCodePoint(name); cp && *cp >= 0x80) return *cp;   // a non-Latin letter
            return std::nullopt;
        }

        std::string LabelledText(std::int32_t key, bool forDisplay)
        {
            for (const NamedKey& k : kNamedKeys)
                if (k.key == key) return std::string(forDisplay ? k.display : k.canonical);
            for (int n = 1; n <= 12; ++n) if (key == Keys::FKey(n)) return "F" + std::to_string(n);
            if (forDisplay) if (std::string n = ActiveKeyLayout().KeyName(key); !n.empty()) return n;
            if (key >= 'a' && key <= 'z') return std::string(1, static_cast<char>(std::toupper(key)));
            return Utf8(key);
        }

        std::string Modifiers(const KeyChord& c)
        {
            std::string out;
            if (c.ctrl) out += "Ctrl+";
            if (c.shift) out += "Shift+";
            if (c.alt) out += "Alt+";
            if (c.super) out += "Super+";
            return out;
        }

        std::int32_t LogicalKey(const KeyChord& c)
        {
            return c.type == KeyType::Labelled ? c.key : ActiveKeyLayout().KeycodeFor(static_cast<std::uint32_t>(c.key));
        }
    }

    void SetActiveKeyLayout(const Arcane::KeyLayout* layout) { g_layout = layout; }
    const Arcane::KeyLayout& ActiveKeyLayout() { return g_layout ? *g_layout : Arcane::QwertyKeyLayout(); }

    std::optional<KeyChord> ParseKeyChord(std::string_view text)
    {
        text = Trim(text);
        KeyChord c;
        if (text.empty()) return c;   // unbound
        std::vector<std::string_view> parts;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= text.size(); ++i)
            if (i == text.size() || text[i] == '+') { parts.push_back(Trim(text.substr(start, i - start))); start = i + 1; }
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        {
            const std::string_view m = parts[i];
            if (EqualsNoCase(m, "ctrl") || EqualsNoCase(m, "control")) c.ctrl = true;
            else if (EqualsNoCase(m, "shift")) c.shift = true;
            else if (EqualsNoCase(m, "alt") || EqualsNoCase(m, "option")) c.alt = true;
            else if (EqualsNoCase(m, "super") || EqualsNoCase(m, "win") || EqualsNoCase(m, "cmd") || EqualsNoCase(m, "meta")) c.super = true;
            else return std::nullopt;
        }
        const std::string_view key = parts.back();
        if (key.empty()) return std::nullopt;
        if (key.front() == '[')
        {
            if (key.size() < 3 || key.back() != ']') return std::nullopt;
            const auto sc = ParsePhysical(key.substr(1, key.size() - 2));
            if (!sc) return std::nullopt;
            c.type = KeyType::Physical;
            c.key = static_cast<std::int32_t>(*sc);
            return c;
        }
        const auto k = ParseLabelled(key);
        if (!k) return std::nullopt;
        c.key = *k;
        return c;
    }

    std::string FormatKeyChord(const KeyChord& c)
    {
        if (!c.Bound()) return {};
        if (c.type == KeyType::Physical) return Modifiers(c) + "[" + PhysicalName(static_cast<std::uint32_t>(c.key)) + "]";
        return Modifiers(c) + LabelledText(c.key, /*forDisplay*/ false);
    }

    std::string DisplayKeyChord(const KeyChord& c)
    {
        if (!c.Bound()) return {};
        if (c.type == KeyType::Physical)
        {
            const std::int32_t produced = ActiveKeyLayout().KeycodeFor(static_cast<std::uint32_t>(c.key));
            return Modifiers(c) + (produced != 0 ? LabelledText(produced, true) : PhysicalName(static_cast<std::uint32_t>(c.key)));
        }
        return Modifiers(c) + LabelledText(c.key, /*forDisplay*/ true);
    }

    std::optional<std::string> ReservedChordReason(const KeyChord& c)
    {
        if (!c.Bound()) return std::nullopt;
        const std::int32_t k = LogicalKey(c);
        if (c.super) return std::string("the Windows key belongs to the OS");
        if (c.alt && !c.ctrl && k == Keys::FKey(4)) return std::string("Alt+F4 closes the window (Windows)");
        if (c.alt && k == Keys::kTab) return std::string("Alt+Tab switches applications (Windows)");
        if (c.ctrl && !c.alt && k == Keys::kTab) return std::string("Ctrl+Tab is Dear ImGui's window switcher");
        if (c.ctrl && c.alt && k == Keys::kDelete) return std::string("Ctrl+Alt+Delete opens the Windows security screen");
        if (c.ctrl && !c.alt && k == Keys::kEscape) return std::string("Ctrl+Esc opens the Start menu (Windows)");
        if (c.alt && !c.ctrl && k == Keys::kSpace) return std::string("Alt+Space opens the window menu (Windows)");
        return std::nullopt;
    }

    KeyChord ToKeyType(const KeyChord& c, KeyType type)
    {
        if (c.type == type || !c.Bound()) return c;
        KeyChord out = c;
        out.type = type;
        if (type == KeyType::Physical)
        {
            const std::uint32_t sc = ActiveKeyLayout().ScancodeFor(c.key);
            if (sc == 0) return c;
            out.key = static_cast<std::int32_t>(sc);
        }
        else
        {
            const std::int32_t kc = ActiveKeyLayout().KeycodeFor(static_cast<std::uint32_t>(c.key));
            if (kc == 0) return c;
            out.key = kc;
        }
        return out;
    }

    bool SameKey(const KeyChord& a, const KeyChord& b)
    {
        if (!a.Bound() || !b.Bound()) return false;
        if (a.ctrl != b.ctrl || a.shift != b.shift || a.alt != b.alt || a.super != b.super) return false;
        if (a.type == b.type) return a.key == b.key;
        return LogicalKey(a) == LogicalKey(b);
    }

    // ---- contexts --------------------------------------------------------
    const char* ActionContextName(ActionContext c) noexcept
    {
        switch (c)
        {
        case ActionContext::Global:        return "Global";
        case ActionContext::Viewport:      return "Viewport";
        case ActionContext::Graph:         return "Graph";
        case ActionContext::AssetBrowser:  return "Asset Browser";
        case ActionContext::Outliner:      return "Outliner";
        case ActionContext::Inspector:     return "Inspector";
        case ActionContext::Text:          return "Text field";
        case ActionContext::Console:       return "Console";
        case ActionContext::InputDocument: return "Input Actions";
        case ActionContext::Document:      return "Document";
        }
        return "?";
    }

    int ContextSpecificity(ActionContext c) noexcept
    {
        switch (c)
        {
        case ActionContext::Global:   return 0;
        case ActionContext::Document: return 1;
        case ActionContext::Text:     return 3;
        default:                      return 2;
        }
    }

    bool ContextsOverlap(ActionContext a, ActionContext b) noexcept
    {
        if (a == ActionContext::Text || b == ActionContext::Text) return a == b;   // fires only while typing; Global never does
        if (a == b || a == ActionContext::Global || b == ActionContext::Global) return true;
        const auto docHosted = [](ActionContext c)
        { return c == ActionContext::Graph || c == ActionContext::Inspector || c == ActionContext::InputDocument; };
        return (a == ActionContext::Document && docHosted(b)) || (b == ActionContext::Document && docHosted(a));
    }

    bool UsesImGuiRoute(ActionContext c) noexcept { return c != ActionContext::Global && c != ActionContext::Viewport; }

    // ---- state -----------------------------------------------------------
    struct EditorActions::State
    {
        std::string id, displayName, defaultText, cvarName, help;
        EditorActionDesc desc{};
        KeyChord defaultChord{};
        KeyChord chord{};
        std::optional<std::string> boundText;   // the cvar text last parsed
        Arcane::CVarHandle handle{};
        std::size_t index = 0;
        std::uint64_t bindSeq = 0;              // "newer binding wins"
        bool warnedBadText = false;
        bool exactPrev = false, pressed = false, down = false;
        double heldFor = 0.0;
        int repeats = 0;
        [[nodiscard]] bool IsDefault() const noexcept { return chord == defaultChord; }
    };

    namespace
    {
        struct Mods { bool ctrl = false, shift = false, alt = false, super = false; };
        Mods HeldMods(const Arcane::InputSnapshot& s)
        {
            return { s.ScancodeDown(Keys::kScanLCtrl) || s.ScancodeDown(Keys::kScanRCtrl),
                     s.ScancodeDown(Keys::kScanLShift) || s.ScancodeDown(Keys::kScanRShift),
                     s.ScancodeDown(Keys::kScanLAlt) || s.ScancodeDown(Keys::kScanRAlt),
                     s.ScancodeDown(Keys::kScanLGui) || s.ScancodeDown(Keys::kScanRGui) };
        }
        bool ExactMods(const KeyChord& c, const Mods& m) { return c.ctrl == m.ctrl && c.shift == m.shift && c.alt == m.alt && c.super == m.super; }
        bool HasMods(const KeyChord& c, const Mods& m) { return (!c.ctrl || m.ctrl) && (!c.shift || m.shift) && (!c.alt || m.alt) && (!c.super || m.super); }

        // The non-Latin fallback applies to a labelled Latin letter whose QWERTY
        // key produces no Latin letter on this layout.
        bool FallbackApplies(const KeyChord& c)
        {
            if (c.type != KeyType::Labelled || c.key < 'a' || c.key > 'z') return false;
            const std::int32_t produced = ActiveKeyLayout().KeycodeFor(4u + static_cast<std::uint32_t>(c.key - 'a'));
            return !(produced >= 'a' && produced <= 'z');
        }

        bool KeyHeld(const KeyChord& c, const Arcane::InputSnapshot& s)
        {
            if (!c.Bound()) return false;
            if (c.type == KeyType::Physical) return s.ScancodeDown(static_cast<std::uint32_t>(c.key));
            if (s.KeycodeDown(static_cast<std::uint32_t>(c.key))) return true;
            if (c.key == Keys::kReturn && s.KeycodeDown(static_cast<std::uint32_t>(Keys::kKeypadEnter))) return true;
            return FallbackApplies(c) && s.ScancodeDown(4u + static_cast<std::uint32_t>(c.key - 'a'));
        }

        // ImGui's CalcTypematicRepeatAmount: repeats in (t0, t1].
        int RepeatTicks(double t0, double t1, double delay, double rate)
        {
            if (t0 >= t1) return 0;
            if (rate <= 0.0) return (t0 < delay && t1 >= delay) ? 1 : 0;
            const int c0 = t0 < delay ? -1 : static_cast<int>((t0 - delay) / rate);
            const int c1 = t1 < delay ? -1 : static_cast<int>((t1 - delay) / rate);
            return c1 - c0;
        }

        ImGuiKey ToImGuiKey(std::int32_t k)
        {
            if (k >= 'a' && k <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (k - 'a'));
            if (k >= '0' && k <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (k - '0'));
            for (int n = 1; n <= 12; ++n) if (k == Keys::FKey(n)) return static_cast<ImGuiKey>(ImGuiKey_F1 + (n - 1));
            switch (k)
            {
            case Keys::kReturn: return ImGuiKey_Enter;      case Keys::kEscape: return ImGuiKey_Escape;
            case Keys::kBackspace: return ImGuiKey_Backspace; case Keys::kTab: return ImGuiKey_Tab;
            case Keys::kSpace: return ImGuiKey_Space;       case Keys::kDelete: return ImGuiKey_Delete;
            case Keys::kInsert: return ImGuiKey_Insert;     case Keys::kHome: return ImGuiKey_Home;
            case Keys::kEnd: return ImGuiKey_End;           case Keys::kPageUp: return ImGuiKey_PageUp;
            case Keys::kPageDown: return ImGuiKey_PageDown; case Keys::kLeft: return ImGuiKey_LeftArrow;
            case Keys::kRight: return ImGuiKey_RightArrow;  case Keys::kUp: return ImGuiKey_UpArrow;
            case Keys::kDown: return ImGuiKey_DownArrow;    case Keys::kKeypadEnter: return ImGuiKey_KeypadEnter;
            case '-': return ImGuiKey_Minus;   case '=': return ImGuiKey_Equal;     case '[': return ImGuiKey_LeftBracket;
            case ']': return ImGuiKey_RightBracket; case '\\': return ImGuiKey_Backslash; case ';': return ImGuiKey_Semicolon;
            case '\'': return ImGuiKey_Apostrophe; case '`': return ImGuiKey_GraveAccent; case ',': return ImGuiKey_Comma;
            case '.': return ImGuiKey_Period;  case '/': return ImGuiKey_Slash;
            default: return ImGuiKey_None;
            }
        }

        bool ImGuiChordPressed(const KeyChord& c, bool repeat)
        {
            if (ImGui::GetCurrentContext() == nullptr) return false;
            const ImGuiKey key = ToImGuiKey(c.key);
            if (key == ImGuiKey_None) return false;
            ImGuiKeyChord mods = 0;
            if (c.ctrl) mods |= ImGuiMod_Ctrl;
            if (c.shift) mods |= ImGuiMod_Shift;
            if (c.alt) mods |= ImGuiMod_Alt;
            if (c.super) mods |= ImGuiMod_Super;
            const ImGuiInputFlags flags = repeat ? ImGuiInputFlags_Repeat : ImGuiInputFlags_None;
            if (ImGui::IsKeyChordPressed(key | mods, flags)) return true;
            return key == ImGuiKey_Enter && ImGui::IsKeyChordPressed(ImGuiKey_KeypadEnter | mods, flags);   // keypad Enter is Enter
        }

        std::string JoinAnd(const std::vector<std::string>& items)
        {
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i > 0) out += (i + 1 == items.size()) ? " and " : ", ";
                out += items[i];
            }
            return out;
        }
    }

    // ---- lifetime and registration -----------------------------------------
    EditorActions& EditorActions::Get()
    {
        static EditorActions instance(Arcane::CVarRegistry::Get());
        static const bool registered = (RegisterEditorActions(instance), true);
        (void)registered;
        return instance;
    }

    EditorActions::EditorActions(Arcane::CVarRegistry& registry) : m_registry(registry) {}
    EditorActions::~EditorActions() = default;

    void EditorActions::Register(const EditorActionDesc& d)
    {
        if (m_index.contains(d.id)) return;   // idempotent: a re-run boot stage, a test
        auto s = std::make_unique<State>();
        s->id = d.id;
        s->displayName = d.displayName;
        s->defaultText = d.defaultChord;
        s->cvarName = "editor.keys." + s->id;
        s->help = std::format("Keyboard shortcut for {} ({}). \"Ctrl+Z\" matches the key labelled Z on your layout; "
                              "\"[KeyZ]\" matches the key in Z's position. Empty = unbound.",
                              s->displayName, ActionContextName(d.context));
        const std::optional<KeyChord> def = ParseKeyChord(s->defaultText);
        ARC_ASSERT(def.has_value(), "EditorActions: an action's default chord does not parse");
        s->defaultChord = def.value_or(KeyChord{});
        s->chord = s->defaultChord;
        s->index = m_states.size();
        s->bindSeq = s->index;
        s->desc = EditorActionDesc{ s->id, s->displayName, d.context, s->defaultText };

        Arcane::CVarDesc cd;
        cd.name = s->cvarName;
        cd.type = Arcane::CVarType::String;
        cd.defaultValue = Arcane::CVarValue::String(s->defaultText);
        cd.flags = Arcane::CVarFlags::Archive;
        cd.help = s->help;
        cd.displayName = s->displayName;
        cd.keywords = "shortcut key binding hotkey chord keyboard";
        cd.widget = "keychord";
        cd.audience = Arcane::Audience::Editor;
        cd.scope = Arcane::SettingScope::PreferencesMachine;
        cd.apply = Arcane::ApplyMode::Live;
        cd.order = static_cast<std::int32_t>(s->index);
        s->handle = m_registry.Register(cd);
        if (s->handle.IsStale()) s->handle = m_registry.Find(s->cvarName);   // a module reload kept it

        m_index.emplace(s->id, s->index);
        m_states.push_back(std::move(s));
    }

    const EditorActions::State* EditorActions::Find(std::string_view id) const
    {
        const auto it = m_index.find(id);
        return it == m_index.end() ? nullptr : m_states[it->second].get();
    }

    void EditorActions::RefreshBindings()
    {
        for (auto& sp : m_states)
        {
            State& s = *sp;
            const std::optional<Arcane::CVarValue> v = s.handle.IsStale() ? std::nullopt : m_registry.Get(s.handle);
            const std::string text = (v && v->type == Arcane::CVarType::String) ? v->AsString() : s.defaultText;
            if (s.boundText && *s.boundText == text) continue;
            s.boundText = text;
            if (const std::optional<KeyChord> c = ParseKeyChord(text)) s.chord = *c;
            else
            {
                s.chord = KeyChord{};
                if (!s.warnedBadText) ARC_WARN("{}: \"{}\" is not a key chord -- the action is unbound", s.cvarName, text);
                s.warnedBadText = true;
            }
            s.bindSeq = s.IsDefault() ? s.index : (std::uint64_t{ 1 } << 32) + (++m_seq);
        }
    }

    // ---- the frame ---------------------------------------------------------
    void EditorActions::BeginFrame(const ActionFrameInput& in)
    {
        m_prevSnap = m_hasFrame ? m_snap : Arcane::InputSnapshot{};
        m_snap = in.snap ? *in.snap : Arcane::InputSnapshot{};
        m_frame = in;
        m_frame.snap = nullptr;
        m_hasFrame = true;
        m_contextsPrev = m_contextsNow;
        m_contextsNow = in.viewportActive ? (1u << static_cast<unsigned>(ActionContext::Viewport)) : 0u;
        RefreshBindings();

        const Mods held = HeldMods(m_snap);
        for (auto& sp : m_states)
        {
            State& s = *sp;
            const bool key = KeyHeld(s.chord, m_snap);
            const bool exact = key && ExactMods(s.chord, held);
            s.down = key && HasMods(s.chord, held);
            s.pressed = exact && !s.exactPrev;
            s.exactPrev = exact;
            if (s.pressed) { s.heldFor = 0.0; s.repeats = 1; }
            else if (exact) { const double t0 = s.heldFor; s.heldFor += in.dt; s.repeats = RepeatTicks(t0, s.heldFor, in.repeatDelay, in.repeatRate); }
            else { s.heldFor = 0.0; s.repeats = 0; }
        }

        m_capture.reset();
        for (std::uint32_t sc = 1; sc < 512; ++sc)
        {
            if (sc >= Keys::kScanLCtrl && sc <= Keys::kScanRGui) continue;   // modifiers alone are not a chord
            if (!m_snap.ScancodeDown(sc) || m_prevSnap.ScancodeDown(sc)) continue;
            KeyChord physical{ held.ctrl, held.shift, held.alt, held.super, KeyType::Physical, static_cast<std::int32_t>(sc) };
            KeyChord labelled = ToKeyType(physical, KeyType::Labelled);
            m_capture = ChordCapture{ labelled, physical };
            break;
        }
    }

    void EditorActions::MarkContextActive(ActionContext c) noexcept { m_contextsNow |= 1u << static_cast<unsigned>(c); }

    bool EditorActions::ContextActive(ActionContext c) const noexcept
    {
        if (c == ActionContext::Global) return true;
        const std::uint32_t bit = 1u << static_cast<unsigned>(c);
        return ((m_contextsNow | m_contextsPrev) & bit) != 0;
    }

    bool EditorActions::Live(const State& s, bool imguiRoute) const
    {
        if (m_listening) return false;
        const ActionContext c = s.desc.context;
        if (c == ActionContext::Text) return true;
        if (c == ActionContext::Global || c == ActionContext::Viewport)
            return EditorShortcutsLive(m_frame.playMode, m_frame.wantCaptureKeyboard, m_frame.captureLive,
                                       c == ActionContext::Viewport, m_frame.viewportActive);
        if (m_frame.captureLive) return false;
        const bool typing = imguiRoute && ImGui::GetCurrentContext() ? ImGui::GetIO().WantTextInput : m_frame.wantTextInput;
        return s.chord.ctrl || s.chord.alt || s.chord.super || !typing;
    }

    bool EditorActions::Shadowed(const State& s) const
    {
        for (const auto& op : m_states)
        {
            const State& o = *op;
            if (&o == &s || !SameKey(o.chord, s.chord)) continue;
            if (!ContextsOverlap(o.desc.context, s.desc.context) || !ContextActive(o.desc.context)) continue;
            if (o.desc.context == s.desc.context && o.IsDefault() && s.IsDefault()) continue;   // designed overlap
            const int so = ContextSpecificity(o.desc.context), ss = ContextSpecificity(s.desc.context);
            if (so > ss || (so == ss && o.bindSeq > s.bindSeq)) return true;
        }
        return false;
    }

    bool EditorActions::Fired(const State& s, bool repeat) const
    {
        if (!s.chord.Bound() || !Live(s, UsesImGuiRoute(s.desc.context))) return false;
        bool edge = false;
        const bool snapshotEdge = repeat ? s.repeats > 0 : s.pressed;
        if (UsesImGuiRoute(s.desc.context) && s.chord.type == KeyType::Labelled)
            edge = ImGuiChordPressed(s.chord, repeat) || (FallbackApplies(s.chord) && snapshotEdge);
        else
            edge = snapshotEdge;
        return edge && !Shadowed(s);
    }

    bool EditorActions::Pressed(std::string_view id) const       { const State* s = Find(id); return s && Fired(*s, false); }
    bool EditorActions::PressedRepeat(std::string_view id) const { const State* s = Find(id); return s && Fired(*s, true); }

    bool EditorActions::Down(std::string_view id) const
    {
        const State* s = Find(id);
        if (!s || m_listening || !s->down) return false;
        return s->desc.context == ActionContext::Text || !m_frame.captureLive;
    }

    // ---- reading the map ---------------------------------------------------
    std::optional<KeyChord> EditorActions::ChordOf(std::string_view id) const        { const State* s = Find(id); return s ? std::optional<KeyChord>(s->chord) : std::nullopt; }
    std::optional<KeyChord> EditorActions::DefaultChordOf(std::string_view id) const { const State* s = Find(id); return s ? std::optional<KeyChord>(s->defaultChord) : std::nullopt; }
    const EditorActionDesc* EditorActions::Desc(std::string_view id) const          { const State* s = Find(id); return s ? &s->desc : nullptr; }
    Arcane::CVarHandle EditorActions::HandleOf(std::string_view id) const            { const State* s = Find(id); return s ? s->handle : Arcane::CVarHandle{}; }

    std::vector<std::string_view> EditorActions::Ids() const
    {
        std::vector<std::string_view> out;
        out.reserve(m_states.size());
        for (const auto& s : m_states) out.push_back(s->id);
        return out;
    }

    std::string EditorActions::MenuShortcut(std::string_view id) const
    {
        const State* s = Find(id);
        return s ? DisplayKeyChord(s->chord) : std::string{};
    }

    std::vector<std::string> EditorActions::ConflictsOf(std::string_view id) const
    {
        std::vector<std::string> out;
        const State* s = Find(id);
        if (!s || !s->chord.Bound()) return out;
        for (const auto& op : m_states)
        {
            const State& o = *op;
            if (&o == s || !SameKey(o.chord, s->chord) || !ContextsOverlap(o.desc.context, s->desc.context)) continue;
            if (o.IsDefault() && s->IsDefault()) continue;   // shipped together on purpose
            out.push_back(o.id);
        }
        return out;
    }

    std::string EditorActions::FiringActionOf(std::string_view id) const
    {
        const State* best = Find(id);
        if (!best) return {};
        for (const std::string& other : ConflictsOf(id))
        {
            const State* o = Find(other);
            const int so = ContextSpecificity(o->desc.context), sb = ContextSpecificity(best->desc.context);
            if (so > sb || (so == sb && o->bindSeq > best->bindSeq)) best = o;
        }
        return best->id;
    }

    std::string EditorActions::ConflictTooltip(std::string_view id) const
    {
        const std::vector<std::string> others = ConflictsOf(id);
        if (others.empty()) return {};
        std::vector<std::string> named;
        for (const std::string& o : others)
        {
            const State* s = Find(o);
            named.push_back(s->displayName + " (" + ActionContextName(s->desc.context) + ")");
        }
        const State* self = Find(id);
        const State* winner = Find(FiringActionOf(id));
        return DisplayKeyChord(self->chord) + " is also bound to " + JoinAnd(named) + ". Where they overlap, "
             + winner->displayName + " (" + ActionContextName(winner->desc.context) + ") fires.";
    }

    void RegisterEditorActions(EditorActions&) {}
}
