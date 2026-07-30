// THE BUG-CLASS GATE. Three shipped bugs came from EditorApp::Init performing a
// step RuntimeApp did not. This asserts both hosts take every core stage.
//
// Honest limit: this proves both hosts RUN the same stages, not that a stage's
// body behaves identically in both modules. The Astra TypeContext bug was a
// per-module static slot, which is why type_context_install also carries a
// runtime VerifySharedTypeContext check. The two cover different halves.

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/BootSequence.hpp>
#include <Arcane/Host/ProjectBoot.hpp>

TEST_CASE("CoreStages ids are unique", "[boot]")
{
    std::vector<std::string> ids = Arcane::HostBoot::CoreStageIds();
    std::vector<std::string> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    CHECK_FALSE(ids.empty());
}

TEST_CASE("CoreStages declares no dependency it does not contain", "[boot]")
{
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<Arcane::BootStage> core = Arcane::HostBoot::CoreStages(ctx);
    std::vector<std::string> ids;
    for (const Arcane::BootStage& s : core) ids.push_back(s.id);

    for (const Arcane::BootStage& s : core)
        for (const std::string& d : s.dependsOn)
            CHECK(std::find(ids.begin(), ids.end(), d) != ids.end());
}

TEST_CASE("both hosts contain every core stage", "[boot]")
{
    // Built the same way the hosts build theirs: CoreStages, then appends.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> core = Arcane::HostBoot::CoreStageIds();

    const std::vector<std::string> editorIds  = Arcane::HostBoot::EditorStageIdsForTest(ctx);
    const std::vector<std::string> runtimeIds = Arcane::HostBoot::RuntimeStageIdsForTest(ctx);

    for (const std::string& id : core)
    {
        INFO("core stage missing from a host: " << id);
        CHECK(std::find(editorIds.begin(),  editorIds.end(),  id) != editorIds.end());
        CHECK(std::find(runtimeIds.begin(), runtimeIds.end(), id) != runtimeIds.end());
    }
}

TEST_CASE("an appended host stage cannot shadow a core id", "[boot]")
{
    // A duplicate id would silently replace a core stage. BootSequence refuses
    // duplicates at Run(), but catch it here where the message is clearer.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> core = Arcane::HostBoot::CoreStageIds();

    for (const std::vector<std::string>& hostIds :
         { Arcane::HostBoot::EditorStageIdsForTest(ctx),
           Arcane::HostBoot::RuntimeStageIdsForTest(ctx) })
    {
        std::vector<std::string> sorted = hostIds;
        std::sort(sorted.begin(), sorted.end());
        CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    }
}

TEST_CASE("the runtime host appends nothing today", "[boot]")
{
    // If this ever legitimately changes, change it deliberately HERE -- do not
    // weaken it in passing.
    Arcane::HostBoot::BootContext ctx{};
    CHECK(Arcane::HostBoot::RuntimeStageIdsForTest(ctx) == Arcane::HostBoot::CoreStageIds());
}

TEST_CASE("CoreStages is exactly the canonical list", "[boot]")
{
    // Change-detector BY DESIGN. Adding an engine-wide boot step is a
    // DELIBERATE act -- edit this list in the same commit. Do not weaken
    // it in passing. Three shipped bugs came from a step going missing.
    //
    // "edit_core" added 2026-07-30 (review Fix 5): split out of the old
    // monolithic "sprite_tables" -- see ProjectBoot.cpp's edit_core comment.
    const std::vector<std::string> kCanonical = {
        "runtime_create", "edit_core", "type_context_install", "gpu_core",
        "project_open", "render_bridge", "input_config",
        "sprite_tables", "plugin_load", "finalize",
    };
    CHECK(Arcane::HostBoot::CoreStageIds() == kCanonical);
}

TEST_CASE("EditorStages runs editor_fonts/editor_shell/splash_ready ahead of the plugin", "[boot]")
{
    // Change-detector BY DESIGN, same idiom as "CoreStages is exactly the
    // canonical list" above -- pins REGISTRATION ORDER, not just presence,
    // because BootSequence picks the lowest-INDEXED ready Main stage each
    // iteration (BootSequence.cpp), so id/dependsOn membership alone (every
    // other test in this file) cannot catch a reorder that puts these ids
    // back after plugin_load.
    //
    // Why this specific order is load-bearing (2026-07-30 review rounds 2-3):
    // PluginHost::Load -> the plugin's Init is free to call
    // ImGui::SetCurrentContext(...) and never restore it (Sandbox.cpp:102).
    // editor_fonts/editor_shell install the editor's font atlas, theme,
    // ImGuiConfigFlags_DockingEnable, and settings handlers -- ALL of which
    // must land on the EDITOR's ImGui context, not whatever a plugin last
    // set GImGui to. If plugin_load's index is ever lower again, those calls
    // silently reconfigure the game context instead: the editor keeps the
    // stock ImGui font (every ICON_LC_* glyph a missing-glyph box), reveals
    // in stock ImGui blue, drops its persisted imgui.ini layout, and --
    // because the editor context's own docking flag was never set before its
    // first NewFrame -- crashes in ImGui::DockBuilderAddNode
    // (EXCEPTION_ACCESS_VIOLATION, imgui.cpp:20823) the first time
    // EndDockSpace tries to rebuild the dock layout. splash_ready (which
    // calls Window::Show()) has the same requirement for a DIFFERENT reason:
    // if it runs after the slow tail (render_bridge/input_config/
    // sprite_tables/plugin_load/finalize) instead of right after
    // editor_shell, the loading screen stays behind a hidden window until
    // ~98% of the boot's weight is already done -- see ProjectBoot.cpp's
    // splash_ready insertion comment for the exact weight accounting.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> ids = Arcane::HostBoot::EditorStageIdsForTest(ctx);

    const auto indexOf = [&](const std::string& id) -> std::ptrdiff_t
    {
        const auto it = std::find(ids.begin(), ids.end(), id);
        REQUIRE(it != ids.end());
        return it - ids.begin();
    };

    const auto fonts        = indexOf("editor_fonts");
    const auto shell        = indexOf("editor_shell");
    const auto splashReady  = indexOf("splash_ready");
    const auto renderBridge = indexOf("render_bridge");
    const auto pluginLoad   = indexOf("plugin_load");

    CHECK(fonts       < pluginLoad);
    CHECK(shell        < pluginLoad);
    CHECK(splashReady < pluginLoad);
    // splash_ready must reveal the window before the slow tail runs, not
    // merely before plugin_load specifically -- render_bridge is the
    // earliest of that tail and has no dependency on splash_ready, so
    // nothing else forces this ordering.
    CHECK(splashReady < renderBridge);
    // The dependency chain itself, restated as index order: editor_fonts
    // must precede editor_shell (already enforced by dependsOn + the
    // scheduler), and splash_ready must follow both.
    CHECK(fonts       < shell);
    CHECK(shell        < splashReady);
}
