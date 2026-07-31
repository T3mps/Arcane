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

TEST_CASE("EditorStages runs editor_fonts/editor_shell ahead of the plugin", "[boot]")
{
    // Change-detector BY DESIGN, same idiom as "CoreStages is exactly the
    // canonical list" above -- pins REGISTRATION ORDER, not just presence,
    // because BootSequence picks the lowest-INDEXED ready Main stage each
    // iteration (BootSequence.cpp), so id/dependsOn membership alone (every
    // other test in this file) cannot catch a reorder that puts these ids
    // back after plugin_load.
    //
    // Unaffected by Task 8c (2026-07-30, "the splash carries the loading UI,
    // not the editor window") -- this is a DIFFERENT invariant from
    // splash_ready's own ordering (see the next TEST_CASE below), for a
    // different reason, and it does not move: PluginHost::Load -> the
    // plugin's Init is free to call ImGui::SetCurrentContext(...) and never
    // restore it (Sandbox.cpp:102). editor_fonts/editor_shell install the
    // editor's font atlas, theme, ImGuiConfigFlags_DockingEnable, and
    // settings handlers -- ALL of which must land on the EDITOR's ImGui
    // context, not whatever a plugin last set GImGui to. If plugin_load's
    // index is ever lower again, those calls silently reconfigure the game
    // context instead: the editor keeps the stock ImGui font (every
    // ICON_LC_* glyph a missing-glyph box), reveals in stock ImGui blue,
    // drops its persisted imgui.ini layout, and -- because the editor
    // context's own docking flag was never set before its first NewFrame --
    // crashes in ImGui::DockBuilderAddNode (EXCEPTION_ACCESS_VIOLATION,
    // imgui.cpp:20823) the first time EndDockSpace tries to rebuild the dock
    // layout.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> ids = Arcane::HostBoot::EditorStageIdsForTest(ctx);

    const auto indexOf = [&](const std::string& id) -> std::ptrdiff_t
    {
        const auto it = std::find(ids.begin(), ids.end(), id);
        REQUIRE(it != ids.end());
        return it - ids.begin();
    };

    const auto fonts      = indexOf("editor_fonts");
    const auto shell      = indexOf("editor_shell");
    const auto pluginLoad = indexOf("plugin_load");

    CHECK(fonts < pluginLoad);
    CHECK(shell < pluginLoad);
    // The dependency chain itself, restated as index order: editor_fonts
    // must precede editor_shell (already enforced by dependsOn + the
    // scheduler).
    CHECK(fonts < shell);
}

TEST_CASE("EditorStages reveals the window only after finalize", "[boot]")
{
    // Change-detector BY DESIGN (Task 8c, 2026-07-30 correction: "the splash
    // carries the loading UI, not the editor window" -- see
    // docs/superpowers/specs/2026-07-29-async-boot-loading-screen-design.md).
    // splash_ready used to be pinned ahead of plugin_load/render_bridge
    // (revealing the window early, to show a loading bar INSIDE it); that
    // invariant is now backwards. UnrealEdGlobals.cpp:215-236 is the shape we
    // copy: "Hide the splash screen now that everything is ready to go" ->
    // Hide() -> "Do final set up on the editor frame and show it" ->
    // CreateDefaultMainFrame -- the main window does not exist (in UE's
    // model) / is not revealed (in ours) until loading has actually
    // finished. BootProgress is now rendered by the pre-device splash for
    // the whole boot (Arcane::BootSplashPresenter) instead of an in-window
    // bar, so there is no more reason to reveal early.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<std::string> ids = Arcane::HostBoot::EditorStageIdsForTest(ctx);

    const auto indexOf = [&](const std::string& id) -> std::ptrdiff_t
    {
        const auto it = std::find(ids.begin(), ids.end(), id);
        REQUIRE(it != ids.end());
        return it - ids.begin();
    };

    CHECK(indexOf("finalize") < indexOf("splash_ready"));
}

TEST_CASE("plugin_load policy: Optional for the editor, Fatal for the runtime", "[boot]")
{
    // 2026-07-30 human ruling (boot-corestages Task 9b): CoreStages declares
    // plugin_load Fatal by default (ProjectBoot.cpp), and RuntimeStages does
    // not touch it -- a game host with nothing to run has nothing useful to
    // fall back to. EditorStages overrides it to Optional: the editor is the
    // one place a developer can go to FIX a game module that fails to load,
    // and EditorApp::StagePluginLoad's own comment (EditorApp.cpp) already
    // establishes why that is safe -- "every m_plugin-> use in MainLoop is
    // optional-guarded, so a disengaged plugin is safe". Before this ruling,
    // a Fatal plugin_load made EditorApp::Run() return 1 (boot.ok == false)
    // with no window ever opened, which is exactly the "cannot get in to fix
    // it" failure this override exists to close.
    //
    // Unlike the other TEST_CASEs in this file, this one reads BootStage's
    // own .policy field, so it goes through EditorStages/RuntimeStages
    // directly rather than the *StageIdsForTest helpers -- those return ids
    // only (see their declarations in ProjectBoot.hpp), which is exactly
    // right for the id-parity tests above but cannot see policy at all.
    Arcane::HostBoot::BootContext ctx{};

    const auto policyOf = [](const std::vector<Arcane::BootStage>& stages) -> Arcane::BootPolicy
    {
        const auto it = std::find_if(stages.begin(), stages.end(),
            [](const Arcane::BootStage& s) { return s.id == "plugin_load"; });
        REQUIRE(it != stages.end());
        return it->policy;
    };

    CHECK(policyOf(Arcane::HostBoot::EditorStages(ctx))  == Arcane::BootPolicy::Optional);
    CHECK(policyOf(Arcane::HostBoot::RuntimeStages(ctx)) == Arcane::BootPolicy::Fatal);
}

TEST_CASE("splash_ready structurally depends on finalize", "[boot]")
{
    // The index-order check above (mirroring this file's usual idiom) proves
    // splash_ready's REGISTRATION position; this proves the DAG EDGE that
    // makes that position load-bearing rather than accidental -- BootSequence
    // is a real topological scheduler (BootSequence.cpp), so a genuine
    // dependsOn edge on "finalize" is what actually prevents splash_ready
    // from running early, independent of where it happens to sit in the
    // vector.
    Arcane::HostBoot::BootContext ctx{};
    const std::vector<Arcane::BootStage> stages = Arcane::HostBoot::EditorStages(ctx);
    const auto it = std::find_if(stages.begin(), stages.end(),
        [](const Arcane::BootStage& s) { return s.id == "splash_ready"; });
    REQUIRE(it != stages.end());
    CHECK(std::find(it->dependsOn.begin(), it->dependsOn.end(), "finalize") != it->dependsOn.end());
}
