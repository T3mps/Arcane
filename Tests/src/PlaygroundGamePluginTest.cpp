// PlaygroundGame across the ABI: the scene runs (orbit + transform propagation) and
// survives a real FreeLibrary/LoadLibrary reload. Headless -- no GPU.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Registry/Registry.hpp>

#include <filesystem>

namespace
{
    glm::vec2 MoonWorldPos(Arcane::Runtime& rt)
    {
        glm::vec2 pos(0.0f);
        auto& reg = rt.Registry();
        if (auto* sr = reg.GetResource<Arcane::SceneRoot>())
        {
            reg.GetRelations(sr->entity).ForEachDescendant([&](Astra::Entity e, size_t depth) {
                if (depth == 2)
                    if (auto* wt = reg.GetComponent<Arcane::WorldTransform>(e))
                        pos = glm::vec2(wt->matrix[2]);   // translation column
            });
        }
        return pos;
    }
}

TEST_CASE("PlaygroundGame runs and survives a reload", "[hotreload]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    // Scene component types are registered by the plugin's Init (ReRegisterComponent);
    // the shared TypeContext makes the test module's TypeID<WorldTransform> agree.

    Arcane::PluginHost host(rt, std::filesystem::path("PlaygroundGame.dll"));
    REQUIRE(host.Load());

    // ABI v2: the plugin reports the current ABI version and the host resolved
    // the new DrawUI entry point (called by the host between ImGui Begin/Render).
    const Arcane::PluginVTable* vt = host.Vtable();
    REQUIRE(vt != nullptr);
    REQUIRE(vt->ABIVersion != nullptr);
    CHECK(vt->ABIVersion() == Arcane::kGamePluginABIVersion);
    CHECK(Arcane::kGamePluginABIVersion == 2u);
    CHECK(vt->DrawUI != nullptr);

    const glm::vec2 before = MoonWorldPos(rt);
    for (int i = 0; i < 30; ++i)
        rt.Loop().Advance(1.0 / 60.0,
            [&](double dt){ host.Vtable()->FixedUpdate(dt); }, [&](double,double){});
    const glm::vec2 afterSteps = MoonWorldPos(rt);
    CHECK(glm::distance(before, afterSteps) > 1.0f);    // orbit + propagation moved the moon

    REQUIRE(host.ForceReload());                        // snapshot round-trip across a real swap
    const glm::vec2 afterReload = MoonWorldPos(rt);
    CHECK(glm::distance(afterSteps, afterReload) < 1.0f);  // state survived the reload
    CHECK(Arcane::RenderErrorCount() == 0);
    host.Unload();
}
