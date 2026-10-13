// Settings arc S4 (spec s7.2): the viewport/global SDL route resolves through
// the action map. Includes the user's Ctrl+W request: it never moves the gizmo.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActionTable.hpp"
#include "Input/EditorActions.hpp"
#include "Viewport/ViewportActions.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/KeyLayout.hpp>
#include <initializer_list>

using namespace Arcane::Editor;
namespace Keys = Arcane::Keys;

namespace
{
    struct Rig
    {
        Arcane::CVarRegistry reg;
        EditorActions actions{ reg };
        Rig() { RegisterEditorActions(actions); }
        void Frame(std::initializer_list<std::uint32_t> scancodes, bool viewportActive = true)
        {
            Arcane::InputSnapshot s;
            for (std::uint32_t sc : scancodes)
            {
                s.SetScancode(sc);
                if (const std::int32_t kc = Arcane::QwertyKeyLayout().KeycodeFor(sc)) s.AddKeycode(static_cast<std::uint32_t>(kc));
            }
            ActionFrameInput in;
            in.snap = &s;
            in.viewportActive = viewportActive;
            in.dt = 1.0 / 60.0;
            actions.BeginFrame(in);
        }
    };
}

TEST_CASE("SDL route: Ctrl+W closes the document and never switches the gizmo to Translate", "[shortcuts]")
{
    Rig r;
    r.Frame({});
    r.Frame({ Keys::kScanLCtrl, Keys::ScanLetter('W') });
    CHECK(ReadGlobalShortcuts(r.actions).closeDocument);
    CHECK(ReadViewportTool(r.actions, /*rmbHeld*/ false) == ViewportTool::None);
    r.Frame({});
    r.Frame({ Keys::ScanLetter('W') });
    CHECK(ReadViewportTool(r.actions, false) == ViewportTool::Translate);
    CHECK_FALSE(ReadGlobalShortcuts(r.actions).closeDocument);
}

TEST_CASE("SDL route: undo, both redo chords, the scene and clipboard keys", "[shortcuts]")
{
    Rig r;
    const auto press = [&](std::initializer_list<std::uint32_t> keys) { r.Frame({}); r.Frame(keys); return ReadGlobalShortcuts(r.actions); };
    CHECK(press({ Keys::kScanLCtrl, Keys::ScanLetter('Z') }).undo);
    CHECK_FALSE(press({ Keys::kScanLCtrl, Keys::ScanLetter('Z') }).redo);
    CHECK(press({ Keys::kScanLCtrl, Keys::kScanLShift, Keys::ScanLetter('Z') }).redo);
    CHECK(press({ Keys::kScanRCtrl, Keys::ScanLetter('Y') }).redo);
    CHECK(press({ Keys::kScanLCtrl, Keys::ScanLetter('S') }).saveScene);
    CHECK(press({ Keys::kScanLCtrl, Keys::ScanLetter('D') }).duplicate);
    CHECK(press({ Keys::kScanLAlt, Keys::ScanLetter('G') }).perspective);
    CHECK(press({ Keys::kScanLAlt, Keys::ScanLetter('J') }).ortho2D);
    CHECK(press({ Keys::ScanLetter('F') }).frameSelected);
    CHECK(press({ Keys::kScanHome }).frameAll);
}

TEST_CASE("SDL route: tools need viewport focus and stand down under RMB; fly keys hold with Shift", "[shortcuts]")
{
    Rig r;
    r.Frame({});
    r.Frame({ Keys::ScanLetter('E') }, /*viewportActive*/ false);
    CHECK(ReadViewportTool(r.actions, false) == ViewportTool::None);
    r.Frame({});
    r.Frame({ Keys::ScanLetter('E') });
    CHECK(ReadViewportTool(r.actions, /*rmbHeld*/ true) == ViewportTool::None);
    r.Frame({});
    r.Frame({ Keys::ScanLetter('R') });
    CHECK(ReadViewportTool(r.actions, false) == ViewportTool::Scale);
    r.Frame({ Keys::kScanLShift, Keys::ScanLetter('W'), Keys::ScanLetter('D'), Keys::ScanLetter('Q') });
    const glm::vec3 axis = ReadFlyAxis(r.actions);
    CHECK(axis.z == 1.0f);
    CHECK(axis.x == 1.0f);
    CHECK(axis.y == -1.0f);
}

TEST_CASE("SDL route: a rebound action follows its cvar", "[shortcuts]")
{
    Rig r;
    r.reg.Set(r.actions.HandleOf("editor.view.frameAll"), Arcane::CVarValue::String("Ctrl+H"),
              Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
    r.reg.Publish();
    r.Frame({});
    r.Frame({ Keys::kScanHome });
    CHECK_FALSE(ReadGlobalShortcuts(r.actions).frameAll);
    r.Frame({});
    r.Frame({ Keys::kScanLCtrl, Keys::ScanLetter('H') });
    CHECK(ReadGlobalShortcuts(r.actions).frameAll);
}
