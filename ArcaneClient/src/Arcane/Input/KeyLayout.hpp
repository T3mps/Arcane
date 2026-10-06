#pragma once

// KeyLayout (settings arc S4, spec s7.2): the keyboard-layout seam. A KEYCODE
// is what a key produces on the current layout (SDL_Keycode); a SCANCODE is
// its physical position (SDL_Scancode, USB HID). The editor's shortcut
// registry matches and displays through this, never through SDL directly.
// QwertyKeyLayout is pure (tests, and display before SDL video is up);
// SystemKeyLayout asks SDL for the OS's current layout.

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    namespace Keys
    {
        inline constexpr std::int32_t kScancodeMask = 1 << 30;   // SDLK_SCANCODE_MASK
        [[nodiscard]] constexpr std::int32_t FromScancode(std::uint32_t sc) noexcept { return static_cast<std::int32_t>(sc) | kScancodeMask; }
        [[nodiscard]] constexpr std::int32_t FKey(int n) noexcept { return FromScancode(57u + static_cast<std::uint32_t>(n)); }   // F1 = scancode 58
        [[nodiscard]] constexpr std::uint32_t ScanLetter(char upper) noexcept { return 4u + static_cast<std::uint32_t>(upper - 'A'); }

        inline constexpr std::int32_t kReturn = 0x0D, kEscape = 0x1B, kBackspace = 0x08, kTab = 0x09, kSpace = 0x20, kDelete = 0x7F;
        inline constexpr std::int32_t kInsert = FromScancode(73), kHome = FromScancode(74), kPageUp = FromScancode(75);
        inline constexpr std::int32_t kEnd = FromScancode(77), kPageDown = FromScancode(78);
        inline constexpr std::int32_t kRight = FromScancode(79), kLeft = FromScancode(80), kDown = FromScancode(81), kUp = FromScancode(82);
        inline constexpr std::int32_t kKeypadEnter = FromScancode(88);

        inline constexpr std::uint32_t kScanReturn = 40, kScanEscape = 41, kScanBackspace = 42, kScanTab = 43, kScanSpace = 44;
        inline constexpr std::uint32_t kScanF2 = 59, kScanF4 = 61, kScanHome = 74, kScanDelete = 76;
        inline constexpr std::uint32_t kScanRight = 79, kScanLeft = 80, kScanDown = 81, kScanUp = 82;
        inline constexpr std::uint32_t kScanLCtrl = 224, kScanLShift = 225, kScanLAlt = 226, kScanLGui = 227;
        inline constexpr std::uint32_t kScanRCtrl = 228, kScanRShift = 229, kScanRAlt = 230, kScanRGui = 231;
    }

    class ARC_API KeyLayout
    {
    public:
        virtual ~KeyLayout() = default;
        [[nodiscard]] virtual std::int32_t KeycodeFor(std::uint32_t scancode) const = 0;   // 0 = the key produces nothing
        [[nodiscard]] virtual std::uint32_t ScancodeFor(std::int32_t keycode) const = 0;   // 0 = no key produces it
        [[nodiscard]] virtual std::string KeyName(std::int32_t keycode) const = 0;         // "Z", "F2", "Delete"
    };

    [[nodiscard]] ARC_API const KeyLayout& QwertyKeyLayout();
    [[nodiscard]] ARC_API const KeyLayout& SystemKeyLayout();
}
