// Arcane/Plugin/GameComponents.hpp: the per-module component registrar a
// game module's ARCANE_COMPONENT(T) lines feed and its GamePlugin_Init drains
// ONCE through Arcane::Game::RegisterComponents. This is what makes a
// wizard-made component live after one Rebuild with no hand edit to Init --
// the long-term shape chosen 2026-09-13 for the New C++ Class step. This test
// exe IS a module for the registrar's purposes (the list is module-local by
// construction: inline function-local statics never cross a DLL boundary),
// so the two components below are its whole roster.

#include <catch2/catch_test_macros.hpp>

#include "Helpers/TestTypeContext.hpp"   // Arcane::Test::SharedTypeContext (the TypeContext rule)

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Plugin/GameComponents.hpp>

#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

#include <string>
#include <vector>

namespace Arcane::GameComponentsTest
{
    struct Health { int current = 100; int max = 100; };
    ASTRA_REFLECT_TYPE(Health)
        ASTRA_REFLECT_FIELD(Health, current)
        ASTRA_REFLECT_FIELD(Health, max)
    ASTRA_END_REFLECT_TYPE()

    struct Loot { float value = 0.0f; };
    ASTRA_REFLECT_TYPE(Loot)
        ASTRA_REFLECT_FIELD(Loot, value)
    ASTRA_END_REFLECT_TYPE()
}

// The two lines a Component template's .cpp carries. Namespace-qualified on
// purpose: the macro must accept a qualified name (no token pasting on T).
ARCANE_COMPONENT(Arcane::GameComponentsTest::Health)
ARCANE_COMPONENT(Arcane::GameComponentsTest::Loot)

namespace
{
    std::vector<std::string> RegisteredNames(const Astra::Registry& reg)
    {
        std::vector<std::string> names;
        if (const Astra::ComponentRegistry* creg = reg.GetComponentRegistry())
            creg->ForEachComponent([&](Astra::ComponentID, const Astra::ComponentDescriptor& d)
            {
                names.emplace_back(d.name);
            });
        return names;
    }

    bool Contains(const std::vector<std::string>& v, const char* s)
    {
        for (const std::string& n : v)
            if (n == s) return true;
        return false;
    }
}

TEST_CASE("Arcane::Game::RegisterComponents registers every ARCANE_COMPONENT of this module, once", "[plugin]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    Astra::Registry& reg = rt.Registry();

    // Before: the engine's own roster, none of ours.
    CHECK_FALSE(Contains(RegisteredNames(reg), "Arcane::GameComponentsTest::Health"));
    CHECK_FALSE(Contains(RegisteredNames(reg), "Arcane::GameComponentsTest::Loot"));

    // The module bootstrap a game's Init does: open its ComponentModule, drain
    // the registrar into it.
    Astra::ComponentModule module = Astra::ComponentModule::Open(rt.Components(), "GameComponentsTest");
    REQUIRE(static_cast<bool>(module));
    const std::size_t n = Arcane::Game::RegisterComponents(module);

    CHECK(n == 2);   // exactly this TU's two ARCANE_COMPONENT lines
    const std::vector<std::string> after = RegisteredNames(reg);
    CHECK(Contains(after, "Arcane::GameComponentsTest::Health"));
    CHECK(Contains(after, "Arcane::GameComponentsTest::Loot"));
}

TEST_CASE("Arcane::Game::ComponentRegistrars lists the ARCANE_COMPONENT type names, for diagnostics", "[plugin]")
{
    // What a host could print ("module X brought 2 component(s): ...") --
    // the registrar carries the stringified type name alongside the thunk.
    std::vector<std::string> names;
    for (const Arcane::Game::ComponentRegistrar* r = Arcane::Game::ComponentRegistrars(); r; r = r->next)
        names.emplace_back(r->typeName);
    CHECK(names.size() == 2);
    CHECK(Contains(names, "Arcane::GameComponentsTest::Health"));
    CHECK(Contains(names, "Arcane::GameComponentsTest::Loot"));
}
