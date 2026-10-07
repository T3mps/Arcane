#include <Arcane/Input/KeyLayout.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>

#include <cctype>

namespace Arcane
{
    static_assert(Keys::kReturn == SDLK_RETURN && Keys::kEscape == SDLK_ESCAPE && Keys::kDelete == SDLK_DELETE);
    static_assert(Keys::FKey(2) == SDLK_F2 && Keys::FKey(12) == SDLK_F12 && Keys::kHome == SDLK_HOME);
    static_assert(Keys::kUp == SDLK_UP && Keys::kKeypadEnter == SDLK_KP_ENTER && Keys::kPageDown == SDLK_PAGEDOWN);
    static_assert(Keys::ScanLetter('W') == SDL_SCANCODE_W && Keys::kScanLCtrl == SDL_SCANCODE_LCTRL && Keys::kScanRGui == SDL_SCANCODE_RGUI);

    namespace
    {
        std::int32_t QwertyKeycode(std::uint32_t sc) noexcept
        {
            if (sc >= 4 && sc <= 29) return 'a' + static_cast<std::int32_t>(sc - 4);
            if (sc >= 30 && sc <= 38) return '1' + static_cast<std::int32_t>(sc - 30);
            if (sc == 39) return '0';
            switch (sc)
            {
            case 40: return Keys::kReturn;  case 41: return Keys::kEscape; case 42: return Keys::kBackspace;
            case 43: return Keys::kTab;     case 44: return Keys::kSpace;  case 45: return '-';  case 46: return '=';
            case 47: return '[';            case 48: return ']';           case 49: return '\\'; case 51: return ';';
            case 52: return '\'';           case 53: return '`';           case 54: return ',';  case 55: return '.';
            case 56: return '/';            case 76: return Keys::kDelete; default: break;
            }
            if ((sc >= 58 && sc <= 69) || (sc >= 73 && sc <= 82) || sc == 88 || (sc >= 224 && sc <= 231))
                return Keys::FromScancode(sc);
            return 0;
        }

        // Names for the keys that print no character; letters and punctuation name themselves.
        std::string NamedKey(std::int32_t kc)
        {
            switch (kc)
            {
            case Keys::kReturn: return "Enter";       case Keys::kEscape: return "Escape";     case Keys::kBackspace: return "Backspace";
            case Keys::kTab: return "Tab";            case Keys::kSpace: return "Space";       case Keys::kDelete: return "Delete";
            case Keys::kInsert: return "Insert";      case Keys::kHome: return "Home";         case Keys::kEnd: return "End";
            case Keys::kPageUp: return "PageUp";      case Keys::kPageDown: return "PageDown"; case Keys::kLeft: return "Left";
            case Keys::kRight: return "Right";        case Keys::kUp: return "Up";             case Keys::kDown: return "Down";
            case Keys::kKeypadEnter: return "KeypadEnter";
            default: break;
            }
            for (int n = 1; n <= 12; ++n)
                if (kc == Keys::FKey(n)) return "F" + std::to_string(n);
            return {};
        }

        std::string QwertyName(std::int32_t kc)
        {
            if (std::string named = NamedKey(kc); !named.empty()) return named;
            if (kc >= 'a' && kc <= 'z') return std::string(1, static_cast<char>(std::toupper(kc)));
            if (kc > 0x20 && kc < 0x7F) return std::string(1, static_cast<char>(kc));
            return {};
        }

        class QwertyLayout final : public KeyLayout
        {
        public:
            std::int32_t KeycodeFor(std::uint32_t sc) const override { return QwertyKeycode(sc); }
            std::uint32_t ScancodeFor(std::int32_t kc) const override
            {
                for (std::uint32_t sc = 1; sc < 512; ++sc) if (QwertyKeycode(sc) == kc) return sc;
                return 0;
            }
            std::string KeyName(std::int32_t kc) const override { return QwertyName(kc); }
        };

        class SdlLayout final : public KeyLayout
        {
        public:
            std::int32_t KeycodeFor(std::uint32_t sc) const override
            {
                const SDL_Keycode k = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(sc), SDL_KMOD_NONE, false);
                return k == SDLK_UNKNOWN ? 0 : static_cast<std::int32_t>(k);
            }
            std::uint32_t ScancodeFor(std::int32_t kc) const override
            {
                SDL_Keymod mod = SDL_KMOD_NONE;
                const SDL_Scancode s = SDL_GetScancodeFromKey(static_cast<SDL_Keycode>(kc), &mod);
                return s == SDL_SCANCODE_UNKNOWN ? 0u : static_cast<std::uint32_t>(s);
            }
            std::string KeyName(std::int32_t kc) const override
            {
                if (std::string named = NamedKey(kc); !named.empty()) return named;   // one spelling on every layout
                const char* n = SDL_GetKeyName(static_cast<SDL_Keycode>(kc));
                return (n && *n) ? std::string(n) : QwertyName(kc);
            }
        };
    }

    const KeyLayout& QwertyKeyLayout() { static const QwertyLayout k; return k; }
    const KeyLayout& SystemKeyLayout() { static const SdlLayout k; return k; }
}
