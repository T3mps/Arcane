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
    const std::vector<std::string> kCanonical = {
        "runtime_create", "type_context_install", "gpu_core",
        "project_open", "render_bridge", "input_config",
        "sprite_tables", "plugin_load", "finalize",
    };
    CHECK(Arcane::HostBoot::CoreStageIds() == kCanonical);
}
