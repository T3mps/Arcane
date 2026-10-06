#include "Input/EditorActions.hpp"

#include <Arcane/Input/KeyLayout.hpp>

#include <cctype>
#include <cstdlib>
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
}
