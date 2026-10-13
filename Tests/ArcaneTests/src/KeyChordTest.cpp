// Settings arc S4 (spec s7.2): chord text, the two key types, layout-aware
// display, reserved chords and the Labelled <-> Physical flip.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActions.hpp"
#include <Arcane/Input/KeyLayout.hpp>

using namespace Arcane::Editor;
namespace Keys = Arcane::Keys;

namespace
{
    // AZERTY's letter swaps: the QWERTY Q/A and W/Z positions trade letters.
    struct AzertyLayout final : Arcane::KeyLayout
    {
        std::int32_t KeycodeFor(std::uint32_t sc) const override
        {
            switch (sc)
            {
            case Keys::ScanLetter('Q'): return 'a';
            case Keys::ScanLetter('A'): return 'q';
            case Keys::ScanLetter('W'): return 'z';
            case Keys::ScanLetter('Z'): return 'w';
            default: return Arcane::QwertyKeyLayout().KeycodeFor(sc);
            }
        }
        std::uint32_t ScancodeFor(std::int32_t kc) const override
        {
            for (std::uint32_t sc = 1; sc < 512; ++sc) if (KeycodeFor(sc) == kc) return sc;
            return 0;
        }
        std::string KeyName(std::int32_t kc) const override { return Arcane::QwertyKeyLayout().KeyName(kc); }
    };

    struct LayoutScope
    {
        explicit LayoutScope(const Arcane::KeyLayout& l) { SetActiveKeyLayout(&l); }
        ~LayoutScope() { SetActiveKeyLayout(nullptr); }
    };
}

TEST_CASE("KeyChord: labelled and physical text parse and format canonically", "[shortcuts]")
{
    const auto undo = ParseKeyChord("ctrl+z");
    REQUIRE(undo.has_value());
    CHECK(undo->ctrl);
    CHECK_FALSE(undo->shift);
    CHECK(undo->type == KeyType::Labelled);
    CHECK(undo->key == 'z');
    CHECK(FormatKeyChord(*undo) == "Ctrl+Z");
    CHECK(FormatKeyChord(*ParseKeyChord("Shift+ctrl+Z")) == "Ctrl+Shift+Z");

    const auto fly = ParseKeyChord("[KeyW]");
    REQUIRE(fly.has_value());
    CHECK(fly->type == KeyType::Physical);
    CHECK(fly->key == static_cast<std::int32_t>(Keys::ScanLetter('W')));
    CHECK(FormatKeyChord(*fly) == "[KeyW]");

    CHECK(ParseKeyChord("F2")->key == Keys::FKey(2));
    CHECK(FormatKeyChord(*ParseKeyChord("Del")) == "Delete");
    CHECK(FormatKeyChord(*ParseKeyChord("Return")) == "Enter");
    CHECK(FormatKeyChord(*ParseKeyChord("Alt+G")) == "Alt+G");
    CHECK(FormatKeyChord(*ParseKeyChord("Ctrl+[KeyS]")) == "Ctrl+[KeyS]");
    CHECK(FormatKeyChord(*ParseKeyChord("[ArrowUp]")) == "[ArrowUp]");

    const auto unbound = ParseKeyChord("");
    REQUIRE(unbound.has_value());
    CHECK_FALSE(unbound->Bound());
    CHECK(FormatKeyChord(*unbound).empty());

    CHECK_FALSE(ParseKeyChord("Ctrl+").has_value());
    CHECK_FALSE(ParseKeyChord("Hyper+Z").has_value());
    CHECK_FALSE(ParseKeyChord("[KeyQQ]").has_value());
    CHECK_FALSE(ParseKeyChord("Ctrl+Bogus").has_value());
}

TEST_CASE("KeyChord: display uses the active layout's labels", "[shortcuts]")
{
    CHECK(DisplayKeyChord(*ParseKeyChord("Ctrl+Z")) == "Ctrl+Z");
    CHECK(DisplayKeyChord(*ParseKeyChord("Delete")) == "Del");
    CHECK(DisplayKeyChord(*ParseKeyChord("Escape")) == "Esc");
    CHECK(DisplayKeyChord(*ParseKeyChord("F2")) == "F2");
    CHECK(DisplayKeyChord(*ParseKeyChord("[KeyW]")) == "W");
    const AzertyLayout azerty;
    const LayoutScope scope(azerty);
    CHECK(DisplayKeyChord(*ParseKeyChord("[KeyW]")) == "Z");   // the fly-forward key reads "Z" on AZERTY
    CHECK(DisplayKeyChord(*ParseKeyChord("Ctrl+Z")) == "Ctrl+Z");
}

TEST_CASE("KeyChord: reserved chords are refused with a reason", "[shortcuts]")
{
    CHECK(ReservedChordReason(*ParseKeyChord("Alt+F4")).has_value());
    CHECK(ReservedChordReason(*ParseKeyChord("Ctrl+Tab")).has_value());
    CHECK(ReservedChordReason(*ParseKeyChord("Alt+Tab")).has_value());
    CHECK(ReservedChordReason(*ParseKeyChord("Super+E")).has_value());
    CHECK(ReservedChordReason(*ParseKeyChord("Ctrl+Alt+Delete")).has_value());
    CHECK(ReservedChordReason(*ParseKeyChord("Alt+[F4]")).has_value());      // physical spelling of the same key
    CHECK_FALSE(ReservedChordReason(*ParseKeyChord("Ctrl+K")).has_value());
    CHECK_FALSE(ReservedChordReason(*ParseKeyChord("F4")).has_value());
}

TEST_CASE("KeyChord: Labelled <-> Physical through the layout, and SameKey across types", "[shortcuts]")
{
    CHECK(FormatKeyChord(ToKeyType(*ParseKeyChord("W"), KeyType::Physical)) == "[KeyW]");
    CHECK(FormatKeyChord(ToKeyType(*ParseKeyChord("[KeyW]"), KeyType::Labelled)) == "W");
    CHECK(SameKey(*ParseKeyChord("W"), *ParseKeyChord("[KeyW]")));
    CHECK_FALSE(SameKey(*ParseKeyChord("Ctrl+W"), *ParseKeyChord("[KeyW]")));
    const AzertyLayout azerty;
    const LayoutScope scope(azerty);
    CHECK(FormatKeyChord(ToKeyType(*ParseKeyChord("Z"), KeyType::Physical)) == "[KeyW]");   // AZERTY's Z sits at QWERTY W
    CHECK_FALSE(SameKey(*ParseKeyChord("W"), *ParseKeyChord("[KeyW]")));
}
