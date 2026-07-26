// Outliner slice 1: EntityInfo/Hidden ride every persistence seam. JSON
// (scene files), BINARY (Play snapshots -- the path that exercises
// EntityInfo::Serialize, the first non-trivially-copyable component), and
// the RenderSubmissionSystem Not<Hidden> skip (recording mock batcher,
// SpriteRotationTest's harness).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Guid.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>
#include <string>

namespace
{
    std::unique_ptr<Astra::Registry> FreshReg(std::shared_ptr<Astra::ComponentRegistry>& outCreg)
    {
        outCreg = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(outCreg);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    // The recording-mock shape from SpriteRotationTest.cpp, counting only.
    struct CountingBatcher final : Arcane::Batcher2D
    {
        int rects = 0;
        void Begin(nvrhi::ICommandList*, nvrhi::IFramebuffer*, uint32_t, uint32_t) override {}
        void SetLayer(uint16_t, uint16_t) override {}
        void Quad(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                  glm::vec4, float) override { ++rects; }
        void Glyph(glm::vec2, glm::vec2, nvrhi::ITexture*, glm::vec2, glm::vec2,
                   glm::vec4) override {}
        void Rect(glm::vec2, glm::vec2, glm::vec4, float) override { ++rects; }
        void Line(glm::vec2, glm::vec2, float, glm::vec4) override {}
        void Circle(glm::vec2, float, glm::vec4) override {}
        void Triangle(glm::vec2, glm::vec2, glm::vec2, glm::vec4) override {}
        void End() override {}
        void RemoveTexture(nvrhi::ITexture*) override {}
        Arcane::Batch2DStats Stats() const override { return {}; }
    };
}

// Regression guard for the SceneSerializer zero-field-component save fix
// (commit d0142324): EntityInfo/Hidden are the zero/near-zero-field
// components that exposed the bug. If this test is ever rewritten, that
// coverage must move with it, not vanish.
TEST_CASE("EntityInfo + Hidden round-trip scene JSON", "[outliner][json]")
{
    const Arcane::Guid stableId = Arcane::Guid::Generate();
    nlohmann::json doc;
    {
        std::shared_ptr<Astra::ComponentRegistry> creg;
        auto reg = FreshReg(creg);
        Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
        reg->AddComponent<Arcane::EntityInfo>(e, Arcane::EntityInfo{ stableId, "Player Spawn" });
        reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
        // SaveJson only walks the SceneRoot subtree (SceneJsonTest.cpp's
        // established pattern) -- without this the entities array is empty.
        reg->SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ e });
        doc = Arcane::Scene::SaveJson(*reg);
    }
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    REQUIRE(Arcane::Scene::LoadJson(*reg, doc));

    int found = 0;
    reg->CreateView<Arcane::EntityInfo>().ForEach(
        [&](Astra::Entity e2, Arcane::EntityInfo& info)
    {
        ++found;
        CHECK(info.id == stableId);
        CHECK(info.name == "Player Spawn");
        CHECK(reg->GetComponent<Arcane::Hidden>(e2) != nullptr);
    });
    CHECK(found == 1);
}

TEST_CASE("EntityInfo survives the binary snapshot path (Play round-trip)", "[outliner]")
{
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::EntityInfo>(
        e, Arcane::EntityInfo{ Arcane::Guid{ 1, 2 }, "A name long enough to defeat SSO ................" });
    reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});

    auto bytes = reg->Save();
    REQUIRE(bytes.IsOk());
    auto loaded = Astra::Registry::Load(std::span<const std::byte>(*bytes), creg);
    REQUIRE(loaded.IsOk());

    Arcane::EntityInfo* info = (*loaded)->GetComponent<Arcane::EntityInfo>(e);   // SAME entity id
    REQUIRE(info != nullptr);
    CHECK(info->id == (Arcane::Guid{ 1, 2 }));
    CHECK(info->name == "A name long enough to defeat SSO ................");
    CHECK((*loaded)->GetComponent<Arcane::Hidden>(e) != nullptr);
}

TEST_CASE("RenderSubmissionSystem skips Hidden entities", "[outliner][render]")
{
    std::shared_ptr<Astra::ComponentRegistry> creg;
    auto reg = FreshReg(creg);
    CountingBatcher batcher;
    reg->SetResource<Arcane::RenderContext2D>(
        Arcane::RenderContext2D{ &batcher, glm::vec2(0.0f, 0.0f), 1.0f });

    auto sprite = [&](bool hidden)
    {
        Astra::Entity e = reg->CreateEntity();
        reg->AddComponent<Arcane::Transform>(e, Arcane::Transform{});
        reg->AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
        if (hidden) reg->AddComponent<Arcane::Hidden>(e, Arcane::Hidden{});
        return e;
    };
    sprite(false);
    sprite(true);
    sprite(false);

    Arcane::RenderSubmissionSystem{}(*reg);
    CHECK(batcher.rects == 2);   // the Hidden one never reached the batcher
}
