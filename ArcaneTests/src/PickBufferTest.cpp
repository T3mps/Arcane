// Arcane pickable-collection emitter (Task 2 of the entity-id picking design,
// docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md SS3b/3c).
// CPU-only ([pick]), no GPU: exercises CollectPickables (sprite + collider
// emitters) and the id<->entity table (PickEntityForId).

#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>

#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/PickBuffer.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include "Helpers/TestTypeContext.hpp"

using Catch::Approx;

namespace
{
    // Fresh registry with Scene + Physics components registered and a
    // zero-gravity PhysicsResource attached.
    //
    // Cross-DLL note (mirrors EntityPickTest.cpp's MakeSceneRegistry):
    // CollectPickables is compiled into Arcane.dll (Arcane/Render/PickEmit.cpp),
    // so its registry.CreateView<WorldTransform, SpriteRenderer>() resolves
    // component IDs through Arcane.dll's per-module TypeContext slot, not the
    // one main() installs in the test module. Pin that DLL slot to the shared
    // test context once (a throwaway Runtime installs it in Arcane.dll; the
    // slot persists after the Runtime is destroyed) so both modules agree on
    // component IDs -- otherwise [pick] run in isolation would see 0 sprites.
    std::unique_ptr<Astra::Registry> MakePickRegistry()
    {
        // Belt-and-braces: test_main pins Arcane.dll's TypeContext slot once
        // before any test runs, which is the real guarantee (per-type IDs are
        // cached in per-module magic statics and never re-resolve, so a late
        // pin cannot repair an already-cached id). Re-pinning here only keeps
        // the slot pointed at the shared context; never install an unshared
        // one anywhere in this suite.
        Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());

        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        Arcane::RegisterPhysicsComponents(*reg);

        Manifold2D::Physics::WorldDef wd;
        wd.gravityY = 0.0f;
        wd.gravityX = 0.0f;
        reg->SetResource(Arcane::PhysicsResource{
            std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd),
            {}
        });

        return reg;
    }

    // `size` is the DRAWN size in world units. A sprite with no .arcsprite asset
    // draws a 1x1 m quad times the world scale, so the size rides the basis
    // columns of the world matrix (what Transform::ToMatrix bakes scale into).
    Astra::Entity SpawnSprite(Astra::Registry& reg, glm::vec2 pos, glm::vec2 size)
    {
        const Astra::Entity e = reg.CreateEntity();
        Arcane::WorldTransform wt;
        wt.matrix = glm::mat3(1.0f);
        wt.matrix[0] = glm::vec3(size.x, 0.0f, 0.0f); // columns 0/1 = rotation*scale
        wt.matrix[1] = glm::vec3(0.0f, size.y, 0.0f);
        wt.matrix[2] = glm::vec3(pos, 1.0f);          // column 2 = world position
        reg.AddComponent<Arcane::WorldTransform>(e, wt);
        Arcane::SpriteRenderer sp;
        reg.AddComponent<Arcane::SpriteRenderer>(e, sp);
        return e;
    }

    // Mints a kinematic body with a single Aabb fixture at `pos`, via the real
    // PhysicsSystem create pass (stepWorld=false: registers the body and
    // writes it back without stepping, so it stays exactly at `pos`).
    // `scale` is baked into the body's fixtures by the create pass (it sets
    // PhysicsBodyRef::appliedScale = lt.scale) -- used to prove CollectPickables
    // scales the silhouette to match the drawn collider.
    Astra::Entity SpawnAabbBody(Astra::Registry& reg, glm::vec2 pos, float halfW, float halfH,
                                glm::vec2 scale = glm::vec2(1.0f, 1.0f))
    {
        const Astra::Entity e = reg.CreateEntity();

        Arcane::Transform lt;
        lt.position = pos;
        lt.scale    = scale;
        reg.AddComponent<Arcane::Transform>(e, lt);

        Arcane::RigidBody2D rb;
        rb.type = Manifold2D::Physics::BodyType::Kinematic;
        reg.AddComponent<Arcane::RigidBody2D>(e, rb);

        Arcane::Collider2D col;
        Arcane::Fixture fx;
        fx.kind  = Manifold2D::Physics::ShapeKind::Aabb;
        fx.halfW = halfW;
        fx.halfH = halfH;
        col.fixtures.push_back(fx);
        reg.AddComponent<Arcane::Collider2D>(e, col);

        reg.AddComponent<Arcane::PhysicsBodyRef>(e, Arcane::PhysicsBodyRef{});

        Arcane::PhysicsSystem sys(1.0f / 60.0f, /*stepWorld=*/false);
        sys(reg);

        return e;
    }
}

TEST_CASE("CollectPickables gathers sprites and physics colliders, ordered", "[pick]")
{
    auto reg = MakePickRegistry();

    // Known camera view: offset (100,50) canvas px, 10 px per world-meter.
    const Arcane::PickView view{ {100.0f, 50.0f}, 10.0f };

    // Sprite entity: world pos (2,3), size (4,6) -> world half-extents (2,3).
    const Astra::Entity spriteEntity = SpawnSprite(*reg, {2.0f, 3.0f}, {4.0f, 6.0f});

    // Physics entity: Aabb fixture halfW=0.5 halfH=0.25, body pos (5,-1).
    const Astra::Entity colliderEntity = SpawnAabbBody(*reg, {5.0f, -1.0f}, 0.5f, 0.25f);

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 2);

    // Each entity appears exactly once, sprites-then-colliders order.
    CHECK(out[0].entity.GetValue() == spriteEntity.GetValue());
    CHECK(out[1].entity.GetValue() == colliderEntity.GetValue());

    // Sprite -> Quad. Projected: center = (2,3)*10 + (100,50) = (120,80);
    // half-extents = (2,3)*10 = (20,30) (world half-extents (2,3) from size*0.5).
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Quad);
    CHECK(out[0].center.x == Approx(120.0f));
    CHECK(out[0].center.y == Approx(80.0f));
    CHECK(out[0].halfExtents.x == Approx(20.0f));
    CHECK(out[0].halfExtents.y == Approx(30.0f));

    // Collider -> Box. Projected: center = (5,-1)*10 + (100,50) = (150,40);
    // half-extents = (0.5,0.25)*10 = (5,2.5).
    CHECK(out[1].kind == Arcane::PickDrawable::Kind::Box);
    CHECK(out[1].center.x == Approx(150.0f));
    CHECK(out[1].center.y == Approx(40.0f));
    CHECK(out[1].halfExtents.x == Approx(5.0f));
    CHECK(out[1].halfExtents.y == Approx(2.5f));
}

TEST_CASE("id->entity table maps 1-based, 0 is background", "[pick]")
{
    auto reg = MakePickRegistry();
    const Astra::Entity e0 = reg->CreateEntity();
    const Astra::Entity e1 = reg->CreateEntity();

    std::vector<Arcane::PickDrawable> table;
    Arcane::PickDrawable d0; d0.entity = e0; table.push_back(d0);
    Arcane::PickDrawable d1; d1.entity = e1; table.push_back(d1);

    CHECK_FALSE(Arcane::PickEntityForId(table, 0).IsValid());   // 0 == background
    CHECK(Arcane::PickEntityForId(table, 1).GetValue() == e0.GetValue());
    CHECK(Arcane::PickEntityForId(table, 2).GetValue() == e1.GetValue());
    CHECK_FALSE(Arcane::PickEntityForId(table, 3).IsValid());   // out of range
}

// The silhouette must match the DRAWN collider, which the physics create pass
// bakes at lt.scale (MakeScaledShape) -- so a scaled body has to pick at its
// scaled size, not its authored size. Aabb scales per-axis (halfW*|sx|,
// halfH*|sy|), mirroring PhysicsSystem::MakeScaledShape.
TEST_CASE("CollectPickables scales a collider silhouette by the body's baked scale", "[pick]")
{
    auto reg = MakePickRegistry();

    // 10 px per world-meter, no offset.
    const Arcane::PickView view{ {0.0f, 0.0f}, 10.0f };

    // Aabb halfW=0.5 halfH=0.25 at body pos (1,1), authored scale (2,4). The
    // create pass bakes appliedScale=(2,4); the silhouette half-extents scale to
    //   world half = (0.5*2, 0.25*4) = (1.0, 1.0); canvas = *10 = (10, 10).
    const Astra::Entity e = SpawnAabbBody(*reg, {1.0f, 1.0f}, 0.5f, 0.25f, {2.0f, 4.0f});

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 1);
    CHECK(out[0].entity.GetValue() == e.GetValue());
    CHECK(out[0].kind == Arcane::PickDrawable::Kind::Box);
    CHECK(out[0].center.x == Approx(10.0f));       // (1,1)*10
    CHECK(out[0].center.y == Approx(10.0f));
    CHECK(out[0].halfExtents.x == Approx(10.0f));  // 0.5 * 2 * 10
    CHECK(out[0].halfExtents.y == Approx(10.0f));  // 0.25 * 4 * 10
}

// The drawable index IS the hit-proxy id (id = index+1), so the collection order
// must be DETERMINISTIC and must not depend on unordered_map hash layout. Pins
// the documented contract: sprites first (back), then colliders (front), stable
// across repeated collections.
TEST_CASE("CollectPickables orders sprites before colliders, deterministically", "[pick]")
{
    auto reg = MakePickRegistry();
    const Arcane::PickView view{ {0.0f, 0.0f}, 1.0f };

    const Astra::Entity sprite    = SpawnSprite(*reg, {0.0f, 0.0f}, {1.0f, 1.0f});
    const Astra::Entity colliderA = SpawnAabbBody(*reg, {1.0f, 0.0f}, 0.5f, 0.5f);
    const Astra::Entity colliderB = SpawnAabbBody(*reg, {2.0f, 0.0f}, 0.5f, 0.5f);

    std::vector<Arcane::PickDrawable> out;
    Arcane::CollectPickables(*reg, view, out);

    REQUIRE(out.size() == 3);
    CHECK(out[0].entity.GetValue() == sprite.GetValue());   // sprite first (back)

    // Both colliders appear exactly once, after the sprite (order between the two
    // is archetype-stable; assert set membership to stay robust to that detail).
    const bool colsPresent =
        (out[1].entity.GetValue() == colliderA.GetValue() && out[2].entity.GetValue() == colliderB.GetValue()) ||
        (out[1].entity.GetValue() == colliderB.GetValue() && out[2].entity.GetValue() == colliderA.GetValue());
    CHECK(colsPresent);

    // Determinism: a second collection yields the identical ordering.
    std::vector<Arcane::PickDrawable> out2;
    Arcane::CollectPickables(*reg, view, out2);
    REQUIRE(out2.size() == 3);
    CHECK(out2[0].entity.GetValue() == out[0].entity.GetValue());
    CHECK(out2[1].entity.GetValue() == out[1].entity.GetValue());
    CHECK(out2[2].entity.GetValue() == out[2].entity.GetValue());
}

// ===========================================================================
// GPU tests ([gpu][pick]) -- DESK-DRIVEN. These need a real device (per-backend,
// like OffscreenCanvasTest) and hit the Parsec GPU-driver hazard headless, so
// the ~[gpu] dev-loop excludes them. Run at the desk:
//   ArcaneTests.exe "[pick][gpu]"
// ===========================================================================

namespace
{
    // Task 1 (impl-time verification point SS8.1): prove R32_UINT render targets
    // + clearTextureUInt + a 1x1 sub-region readback work on this backend with
    // validation SILENT -- the exact primitives the id pass + Pick() readback
    // rely on. Mirrors GpuTestHelpers::CheckOffscreenClear, but integer-format
    // and with a 1x1 TextureSlice copy at a known pixel (the Pick() readback).
    void CheckR32UintClearReadback(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto targetDesc = nvrhi::TextureDesc()
            .setWidth(8).setHeight(8)
            .setFormat(nvrhi::Format::R32_UINT)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true)
            .setDebugName("R32UintTarget");
        nvrhi::TextureHandle target = nv->createTexture(targetDesc);
        REQUIRE(target != nullptr);

        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(1).setHeight(1)
            .setFormat(nvrhi::Format::R32_UINT)
            .setDebugName("R32UintReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->clearTextureUInt(target, nvrhi::AllSubresources, 7u);
        // 1x1 copy of the pixel at (3,5) -- the Pick() readback shape.
        cl->copyTexture(staging, nvrhi::TextureSlice(),
                        target, nvrhi::TextureSlice().setOrigin(3, 5)
                                    .setWidth(1).setHeight(1));
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* px = static_cast<const uint32_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(px != nullptr);
        CHECK(px[0] == 7u);
        nv->unmapStagingTexture(staging);
        nv->runGarbageCollection();

        CHECK(Arcane::RenderErrorCount() == 0);
    }

    // Task 1: the PickBuffer skeleton builds its id target + staging and resizes
    // cleanly, with zero NVRHI validation errors.
    void CheckPickBufferSkeleton(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend,
                                                     "data/shaders");
        REQUIRE(shaders != nullptr);

        auto pick = Arcane::PickBuffer::Create(device->Nvrhi(), *shaders, 64, 64);
        REQUIRE(pick != nullptr);
        CHECK(pick->Width() == 64);
        CHECK(pick->Height() == 64);

        pick->Resize(128, 72);
        CHECK(pick->Width() == 128);
        CHECK(pick->Height() == 72);

        // Idempotent no-ops: unchanged size and a zero dimension both leave the
        // target untouched.
        pick->Resize(128, 72);
        pick->Resize(0, 0);
        CHECK(pick->Width() == 128);
        CHECK(pick->Height() == 72);

        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: R32_UINT clear + 1x1 readback is validation-clean",
          "[gpu][pick][d3d12]")
{
    CheckR32UintClearReadback(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: R32_UINT clear + 1x1 readback is validation-clean",
          "[gpu][pick][vulkan]")
{
    CheckR32UintClearReadback(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("d3d12: PickBuffer builds and resizes its id target",
          "[gpu][pick][d3d12]")
{
    CheckPickBufferSkeleton(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: PickBuffer builds and resizes its id target",
          "[gpu][pick][vulkan]")
{
    CheckPickBufferSkeleton(Arcane::GraphicsBackend::Vulkan);
}

namespace
{
    // Copy the full R32_UINT id target into a row-major vector<uint32_t>,
    // handling the staging row pitch.
    std::vector<uint32_t> ReadIdTarget(nvrhi::IDevice* nv, nvrhi::ITexture* target,
                                       uint32_t size)
    {
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(size).setHeight(size)
            .setFormat(nvrhi::Format::R32_UINT)
            .setDebugName("IdReadback");
        nvrhi::StagingTextureHandle staging =
            nv->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        REQUIRE(staging != nullptr);

        nvrhi::CommandListHandle cl = nv->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(), target, nvrhi::TextureSlice());
        cl->close();
        nv->executeCommandList(cl);
        nv->waitForIdle();

        size_t rowPitch = 0;
        const auto* bytes = static_cast<const uint8_t*>(nv->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        REQUIRE(bytes != nullptr);
        std::vector<uint32_t> out(static_cast<size_t>(size) * size);
        for (uint32_t y = 0; y < size; ++y)
            std::memcpy(&out[static_cast<size_t>(y) * size],
                        bytes + static_cast<size_t>(y) * rowPitch,
                        static_cast<size_t>(size) * sizeof(uint32_t));
        nv->unmapStagingTexture(staging);
        return out;
    }

    // Task 3: the entity_id pass rasterizes each pickable silhouette tagged with
    // its 1-based id, background stays 0, and overlaps resolve front-most. Reads
    // back the FULL target and samples known pixels. nvrhi normalizes the render-
    // target Y orientation across backends, so target[y][x] == canvas pixel (x,y).
    void CheckIdPass(Arcane::GraphicsBackend backend)
    {
        constexpr uint32_t kSize = 128;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        nvrhi::IDevice* nv = device->Nvrhi();

        auto shaders = Arcane::ShaderLibrary::Create(nv, backend, "data/shaders");
        REQUIRE(shaders != nullptr);
        auto pick = Arcane::PickBuffer::Create(nv, *shaders, kSize, kSize);
        REQUIRE(pick != nullptr);

        // 10 px per world-meter, no offset.
        const Arcane::PickView view{ {0.0f, 0.0f}, 10.0f };

        // --- non-overlapping: two Aabb colliders at distinct canvas pixels -----
        {
            auto reg = MakePickRegistry();
            const Astra::Entity a = SpawnAabbBody(*reg, {3.0f, 4.0f}, 0.5f, 0.5f); // ->(30,40)
            const Astra::Entity b = SpawnAabbBody(*reg, {9.0f, 9.0f}, 0.5f, 0.5f); // ->(90,90)
            (void)a; (void)b;

            pick->RenderIdPass(*reg, view);
            const std::vector<uint32_t> px = ReadIdTarget(nv, pick->IdTarget(), kSize);
            auto At = [&](uint32_t x, uint32_t y) { return px[static_cast<size_t>(y) * kSize + x]; };

            // No sprites; two colliders in creation order -> ids 1 and 2.
            CHECK(At(30, 40) == 1u);   // body A center
            CHECK(At(90, 90) == 2u);   // body B center
            CHECK(At(60, 60) == 0u);   // gap between them -> background
        }

        // --- overlapping: front-most (later-drawn) id wins the pixel -----------
        {
            auto reg = MakePickRegistry();
            const Astra::Entity back  = SpawnAabbBody(*reg, {6.0f, 6.0f}, 1.0f, 1.0f);
            const Astra::Entity front = SpawnAabbBody(*reg, {6.0f, 6.0f}, 1.0f, 1.0f);
            (void)back; (void)front;

            pick->RenderIdPass(*reg, view);
            const std::vector<uint32_t> px = ReadIdTarget(nv, pick->IdTarget(), kSize);
            // `front` is collected second -> id 2 -> drawn last -> wins (60,60).
            CHECK(px[static_cast<size_t>(60) * kSize + 60] == 2u);
        }

        nv->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: entity_id pass writes front-most ids to covered pixels",
          "[gpu][pick][d3d12]")
{
    CheckIdPass(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: entity_id pass writes front-most ids to covered pixels",
          "[gpu][pick][vulkan]")
{
    CheckIdPass(Arcane::GraphicsBackend::Vulkan);
}

namespace
{
    // Task 4: the public Pick() API -- render the id pass + read back the 1x1
    // pixel under the cursor + map the id to its entity. Background and
    // out-of-range clicks return an invalid entity; overlaps pick the front-most.
    void CheckPick(Arcane::GraphicsBackend backend)
    {
        constexpr uint32_t kSize = 128;
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "data/shaders");
        REQUIRE(shaders != nullptr);
        auto pick = Arcane::PickBuffer::Create(device->Nvrhi(), *shaders, kSize, kSize);
        REQUIRE(pick != nullptr);

        const Arcane::PickView view{ {0.0f, 0.0f}, 10.0f };

        // Two Aabb colliders at distinct canvas pixels.
        auto reg = MakePickRegistry();
        const Astra::Entity a = SpawnAabbBody(*reg, {3.0f, 4.0f}, 0.5f, 0.5f); // ->(30,40)
        const Astra::Entity b = SpawnAabbBody(*reg, {9.0f, 9.0f}, 0.5f, 0.5f); // ->(90,90)

        CHECK(pick->Pick(*reg, view, {30.0f, 40.0f}).GetValue() == a.GetValue());
        CHECK(pick->Pick(*reg, view, {90.0f, 90.0f}).GetValue() == b.GetValue());
        CHECK_FALSE(pick->Pick(*reg, view, {60.0f, 60.0f}).IsValid());     // background
        CHECK_FALSE(pick->Pick(*reg, view, {999.0f, 999.0f}).IsValid());   // out of range

        // Overlap: the front-most (later-drawn) entity is returned.
        auto reg2 = MakePickRegistry();
        const Astra::Entity back  = SpawnAabbBody(*reg2, {6.0f, 6.0f}, 1.0f, 1.0f);
        const Astra::Entity front = SpawnAabbBody(*reg2, {6.0f, 6.0f}, 1.0f, 1.0f);
        (void)back;
        CHECK(pick->Pick(*reg2, view, {60.0f, 60.0f}).GetValue() == front.GetValue());

        device->Nvrhi()->runGarbageCollection();
        CHECK(Arcane::RenderErrorCount() == 0);
    }
}

TEST_CASE("d3d12: Pick returns the entity under a pixel, invalid on background",
          "[gpu][pick][d3d12]")
{
    CheckPick(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: Pick returns the entity under a pixel, invalid on background",
          "[gpu][pick][vulkan]")
{
    CheckPick(Arcane::GraphicsBackend::Vulkan);
}
