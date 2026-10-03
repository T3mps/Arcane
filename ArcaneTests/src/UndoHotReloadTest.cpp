// Spec 2026-09-30 s3.3(f): does a ComponentEditCommand survive a game-module
// release + re-register? Three checks on a ComponentModule fixture.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <memory>

namespace ReloadV1 { struct ReloadProbe { float a = 0.0f; float b = 0.0f; }; }
// The "rebuilt" layout: same size/alignment/triviality, fields swapped.
namespace ReloadV2 { struct ReloadProbe { float b = 0.0f; float a = 0.0f; }; }

TEST_CASE("hot reload: a module component's undo step across release + re-register", "[edit][undo][reload]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    auto first = Astra::ComponentModule::Open(components, "ReloadFixture");
    REQUIRE(first);
    first.Register<ReloadV1::ReloadProbe>();
    Astra::Registry reg(components);

    const Astra::ComponentID id = Astra::TypeID<ReloadV1::ReloadProbe>::Value();
    const Astra::ComponentDescriptor* desc = components->GetComponentDescriptor(id);
    REQUIRE(desc != nullptr);
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<ReloadV1::ReloadProbe>(e, ReloadV1::ReloadProbe{1.0f, 2.0f});
    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(reg, e, desc);
    reg.GetComponent<ReloadV1::ReloadProbe>(e)->a = 9.0f;
    reg.GetComponent<ReloadV1::ReloadProbe>(e)->b = 8.0f;
    std::vector<std::byte> after = Arcane::ComponentEditCommand::Snapshot(reg, e, desc);
    Arcane::ComponentEditCommand cmd([&reg]() -> Astra::Registry& { return reg; }, e, desc,
                                     std::move(before), std::move(after), "Edit Probe");
    const auto live = [&] { return *reg.GetComponent<ReloadV1::ReloadProbe>(e); };

    first.Reset();   // the unload: ReleaseModule clears the slot (no shadow entry)

    SECTION("check 1: the same type returns to the same id and slot")
    {
        auto rebuilt = Astra::ComponentModule::Open(components, "ReloadFixture");
        rebuilt.Register<ReloadV1::ReloadProbe>();
        CHECK(components->GetComponentDescriptor(id) == desc);   // same address, live again
        cmd.Undo();
        CHECK(live().a == 1.0f);
        CHECK(live().b == 2.0f);
    }
    SECTION("check 2: a type the rebuild removed leaves Restore a guarded no-op")
    {
        REQUIRE(components->GetComponentDescriptor(id) == nullptr);
        cmd.Undo();
        CHECK(live().a == 9.0f);
        CHECK(live().b == 8.0f);
    }
    SECTION("check 3: a layout change does not deserialize the old blob")
    {
        // What RegisterOneChecked does for a rebuilt image whose type keeps
        // its mangled name: the SAME id, a descriptor built from the NEW layout.
        Astra::ComponentDescriptor v2 = Astra::ComponentRegistry::MakeDescriptor<ReloadV2::ReloadProbe>(id, nullptr);
        v2.hash = Astra::TypeID<ReloadV1::ReloadProbe>::Hash();
        v2.name = "ReloadProbe";
        const std::uint32_t owner = components->OpenModuleId("ReloadFixture");
        REQUIRE(components->InstallOwned(id, owner, v2, nullptr) == Astra::InstallResult::Installed);
        REQUIRE(components->GetComponentDescriptor(id) == desc);
        cmd.Undo();
        // VERDICT (spec s3.3f): CONFIRMED. The stale blob IS deserialized into
        // the rebuilt layout -- why the editor clears history on module reload.
        CHECK(live().a == 1.0f);
        CHECK(live().b == 2.0f);
    }
}
