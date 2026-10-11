// Settings arc S4 (spec s7.2): the editor action registry -- cvar per action,
// both input routes, exact modifiers, gates, precedence, conflicts, capture.
#include <catch2/catch_test_macros.hpp>
#include "Input/EditorActions.hpp"
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/CVarModule.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Input/KeyLayout.hpp>
#include <imgui.h>
#include <initializer_list>

using namespace Arcane::Editor;
namespace Keys = Arcane::Keys;

namespace
{
    Arcane::InputSnapshot Snap(std::initializer_list<std::uint32_t> scancodes, const Arcane::KeyLayout& layout = Arcane::QwertyKeyLayout())
    {
        Arcane::InputSnapshot s;
        for (std::uint32_t sc : scancodes)
        {
            s.SetScancode(sc);
            if (const std::int32_t kc = layout.KeycodeFor(sc)) s.AddKeycode(static_cast<std::uint32_t>(kc));
        }
        return s;
    }

    struct Rig
    {
        Arcane::CVarRegistry reg;
        EditorActions actions{ reg };
        Rig()
        {
            actions.Register({ "t.undo",   "Undo",          ActionContext::Global,   "Ctrl+Z" });
            actions.Register({ "t.frame",  "Frame",         ActionContext::Global,   "F" });
            actions.Register({ "t.gframe", "Frame (graph)", ActionContext::Graph,    "F" });
            actions.Register({ "t.tool",   "Move Tool",     ActionContext::Viewport, "[KeyW]" });
            actions.Register({ "t.fly",    "Fly Forward",   ActionContext::Viewport, "[KeyW]" });
            actions.Register({ "t.close",  "Close",         ActionContext::Global,   "Ctrl+W" });
            actions.Register({ "t.copy",   "Copy",          ActionContext::Global,   "Ctrl+C" });
        }
        void Frame(const Arcane::InputSnapshot& s, bool viewportActive = true, bool typing = false, double dt = 1.0 / 60.0)
        {
            ActionFrameInput in;
            in.snap = &s; in.viewportActive = viewportActive; in.wantCaptureKeyboard = typing; in.wantTextInput = typing; in.dt = dt;
            actions.BeginFrame(in);
        }
        void Bind(std::string_view id, std::string_view chord)
        {
            reg.Set(actions.HandleOf(id), Arcane::CVarValue::String(std::string(chord)), Arcane::SetBy::EditorUser, "editor", Arcane::CVarContext::Editor);
            reg.Publish();
            actions.RefreshBindings();
        }
    };

    constexpr std::uint32_t kCtrl = Keys::kScanLCtrl, kShift = Keys::kScanLShift;
}

TEST_CASE("EditorActions: Register makes editor.keys.<id>, a String cvar holding the default chord; re-register is a no-op", "[shortcuts]")
{
    Rig r;
    const Arcane::CVarHandle h = r.reg.Find("editor.keys.t.undo");
    REQUIRE_FALSE(h.IsStale());
    r.reg.PublishImmediate();
    const auto v = r.reg.Get(h);
    REQUIRE(v.has_value());
    CHECK(v->type == Arcane::CVarType::String);
    CHECK(v->AsString() == "Ctrl+Z");
    r.actions.Register({ "t.undo", "Undo", ActionContext::Global, "Ctrl+Y" });
    CHECK(FormatKeyChord(*r.actions.ChordOf("t.undo")) == "Ctrl+Z");
    CHECK(r.actions.MenuShortcut("t.undo") == "Ctrl+Z");
    // S4-GATE: the declaring module, as ARC_CVAR records it (this binary here,
    // ArcaneEditor in the editor) -- never ArcaneCore's CurrentModule().
    const std::optional<Arcane::CVarDescInfo> info = r.reg.Describe("editor.keys.t.undo");
    REQUIRE(info.has_value());
    CHECK(info->module == Arcane::Detail::CallerModule());
    CHECK(info->module != "ArcaneCore");
}

TEST_CASE("EditorActions: a press is one edge, with exact modifiers", "[shortcuts]")
{
    Rig r;
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('Z') }));
    CHECK(r.actions.Pressed("t.undo"));
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('Z') }));
    CHECK_FALSE(r.actions.Pressed("t.undo"));              // held: no second edge
    r.Frame(Snap({}));
    r.Frame(Snap({ kCtrl, kShift, Keys::ScanLetter('Z') }));
    CHECK_FALSE(r.actions.Pressed("t.undo"));              // Ctrl+Shift+Z is another chord
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('W') }));
    CHECK(r.actions.Pressed("t.close"));
    CHECK_FALSE(r.actions.Pressed("t.tool"));              // Ctrl+W never switches to the bare-W tool
}

TEST_CASE("EditorActions: gates -- typing, Play, rebind capture, viewport focus; Down ignores extra modifiers", "[shortcuts]")
{
    Rig r;
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('Z') }), true, /*typing*/ true);
    CHECK_FALSE(r.actions.Pressed("t.undo"));
    r.Frame(Snap({}));
    r.Frame(Snap({ Keys::ScanLetter('W') }), /*viewportActive*/ false);
    CHECK_FALSE(r.actions.Pressed("t.tool"));
    r.Frame(Snap({}));
    r.Frame(Snap({ Keys::ScanLetter('W') }));
    CHECK(r.actions.Pressed("t.tool"));                    // designed overlap: both default, same context
    CHECK(r.actions.Pressed("t.fly"));
    r.Frame(Snap({ kShift, Keys::ScanLetter('W') }));
    CHECK(r.actions.Down("t.fly"));                        // Shift is the boost, not a different chord
    ActionFrameInput in; Arcane::InputSnapshot s = Snap({}); in.snap = &s; in.playMode = true; r.actions.BeginFrame(in);
    s = Snap({ kCtrl, Keys::ScanLetter('Z') }); r.actions.BeginFrame(in);
    CHECK_FALSE(r.actions.Pressed("t.undo"));              // Play
}

TEST_CASE("EditorActions: a more specific active context shadows Global on a shared chord", "[shortcuts]")
{
    Rig r;
    r.Frame(Snap({ Keys::ScanLetter('F') }));
    CHECK(r.actions.Pressed("t.frame"));                   // no graph focused
    r.Frame(Snap({}));
    r.actions.MarkContextActive(ActionContext::Graph);     // a graph canvas held focus this frame
    r.Frame(Snap({ Keys::ScanLetter('F') }));
    CHECK_FALSE(r.actions.Pressed("t.frame"));             // ... so F belongs to the graph
    CHECK(r.actions.ConflictsOf("t.frame").empty());       // both at their defaults: not a conflict
}

TEST_CASE("EditorActions: a rebinding that collides is a conflict on every row; newer wins within a context", "[shortcuts]")
{
    Rig r;
    r.Bind("t.copy", "F");
    const auto conflicts = r.actions.ConflictsOf("t.copy");
    CHECK(conflicts.size() == 2);                          // t.frame (Global) and t.gframe (Graph)
    CHECK(r.actions.ConflictsOf("t.frame").size() == 1);
    CHECK(r.actions.ConflictsOf("t.gframe").size() == 1);
    const std::string tip = r.actions.ConflictTooltip("t.copy");
    CHECK(tip.find("Frame (Global)") != std::string::npos);
    CHECK(tip.find("Frame (graph) (Graph)") != std::string::npos);
    CHECK(tip.find("fires") != std::string::npos);
    CHECK(r.actions.FiringActionOf("t.copy") == "t.gframe");   // the more specific context
    r.Frame(Snap({}));
    r.Frame(Snap({ Keys::ScanLetter('F') }));
    CHECK(r.actions.Pressed("t.copy"));                    // Global vs Global: the newer binding
    CHECK_FALSE(r.actions.Pressed("t.frame"));
    r.Bind("t.copy", "Ctrl+C");
    CHECK(r.actions.ConflictsOf("t.copy").empty());
}

TEST_CASE("EditorActions: AZERTY and a non-Latin layout", "[shortcuts]")
{
    struct Azerty final : Arcane::KeyLayout
    {
        std::int32_t KeycodeFor(std::uint32_t sc) const override
        { return sc == Keys::ScanLetter('W') ? 'z' : sc == Keys::ScanLetter('Z') ? 'w' : Arcane::QwertyKeyLayout().KeycodeFor(sc); }
        std::uint32_t ScancodeFor(std::int32_t kc) const override
        { for (std::uint32_t sc = 1; sc < 512; ++sc) if (KeycodeFor(sc) == kc) return sc; return 0; }
        std::string KeyName(std::int32_t kc) const override { return Arcane::QwertyKeyLayout().KeyName(kc); }
    } azerty;
    struct Cyrillic final : Arcane::KeyLayout
    {
        std::int32_t KeycodeFor(std::uint32_t sc) const override
        { return (sc >= 4 && sc <= 29) ? 0x430 + static_cast<std::int32_t>(sc - 4) : Arcane::QwertyKeyLayout().KeycodeFor(sc); }
        std::uint32_t ScancodeFor(std::int32_t kc) const override
        { for (std::uint32_t sc = 1; sc < 512; ++sc) if (KeycodeFor(sc) == kc) return sc; return 0; }
        std::string KeyName(std::int32_t kc) const override { return Arcane::QwertyKeyLayout().KeyName(kc); }
    } cyrillic;

    Rig r;
    SetActiveKeyLayout(&azerty);
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('W') }, azerty));   // the key LABELLED Z on AZERTY
    CHECK(r.actions.Pressed("t.undo"));
    r.Frame(Snap({}, azerty));
    r.Frame(Snap({ Keys::ScanLetter('W') }, azerty));          // the physical fly/tool key: same place
    CHECK(r.actions.Pressed("t.tool"));
    SetActiveKeyLayout(&cyrillic);
    r.Frame(Snap({}, cyrillic));
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('C') }, cyrillic)); // produces U+0442, no Latin C anywhere
    CHECK(r.actions.Pressed("t.copy"));                        // falls back to the QWERTY position
    SetActiveKeyLayout(nullptr);
}

TEST_CASE("EditorActions: the ImGui route reads ImGui's key events for a labelled chord", "[shortcuts]")
{
    Rig r;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600); io.IniFilename = nullptr;
    unsigned char* px = nullptr; int w = 0, h = 0; io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    const auto frame = [&](bool& fired) { io.DeltaTime = 1.0f / 60.0f; ImGui::NewFrame(); fired = r.actions.Pressed("t.gframe"); ImGui::Render(); };
    bool fired = false;
    frame(fired);
    io.AddKeyEvent(ImGuiKey_F, true); frame(fired);
    CHECK(fired);
    io.AddKeyEvent(ImGuiKey_F, false); frame(fired);
    CHECK_FALSE(fired);
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
}

TEST_CASE("EditorActions: listening silences every action and captures the next chord in both forms", "[shortcuts]")
{
    Rig r;
    r.actions.SetListening(true);
    r.Frame(Snap({}));
    r.Frame(Snap({ kCtrl }));
    CHECK_FALSE(r.actions.CapturedChord().has_value());       // a modifier alone is not a chord
    r.Frame(Snap({ kCtrl, Keys::ScanLetter('Z') }));
    CHECK_FALSE(r.actions.Pressed("t.undo"));
    const auto cap = r.actions.CapturedChord();
    REQUIRE(cap.has_value());
    CHECK(FormatKeyChord(cap->labelled) == "Ctrl+Z");
    CHECK(FormatKeyChord(cap->physical) == "Ctrl+[KeyZ]");
    r.actions.SetListening(false);
}

TEST_CASE("EditorActions: PressedRepeat follows ImGui's typematic timing on the snapshot route", "[shortcuts]")
{
    Rig r;
    r.actions.Register({ "t.next", "Next", ActionContext::Global, "Down" });
    int fires = 0;
    r.Frame(Snap({}));
    for (int i = 0; i < 10; ++i)   // 10 frames x 50 ms held: press at 0, repeats at 0.3, 0.35, 0.40, 0.45
    {
        r.Frame(Snap({ Keys::kScanDown }), true, false, 0.05);
        if (r.actions.PressedRepeat("t.next")) ++fires;
    }
    CHECK(fires == 5);
}
