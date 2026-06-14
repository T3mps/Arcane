// Physics M6 P3.6: PhysicsDebugDraw -- GPU smoke test.
//
// Verifies that DrawPhysicsDebug() submits at least some geometry to the
// Batcher2D (lines + circles) and that the NVRHI validation layer stays
// silent (RenderErrorCount() == 0) on both backends.
//
// Scene: a static AABB floor, two dynamic circles resting on it (so a contact
// pair forms + an island unites them), and one kinematic mover above.  After
// ~60 steps the contact manager has begun pairs and the island pass has written
// m_uf.  DrawPhysicsDebug runs over that state and submits primitives; the test
// asserts at least one quad was emitted (lines and circles both become quads
// under the batcher's hood) and that no GPU/NVRHI errors occurred.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Physics/Body.hpp>
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

using namespace Arcane::Physics;

namespace
{
    // Build and step a small physics scene, then render the debug overlay
    // into a 128x128 offscreen canvas.  Asserts draws were submitted and
    // validation is clean.
    void RunDebugDraw(Arcane::GraphicsBackend backend)
    {
        // ---- GPU device + offscreen canvas + batcher -----------------------
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        REQUIRE(shaders != nullptr);

        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 128, 128);
        REQUIRE(canvas != nullptr);

        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(batcher != nullptr);

        // ---- build physics scene -------------------------------------------
        //
        // World with gravity so the dynamic circles settle onto the floor
        // and the island/sleep pass runs.
        WorldDef wdef;
        wdef.gravityY = Real(400);   // pixels / s^2 (same as other physics tests)

        PhysicsWorld world(wdef);

        // Static floor: AABB 200 half-wide, 10 half-tall, centered at (64, 110).
        // Floor top surface is at y = 100.
        BodyDef floorDef;
        floorDef.type     = BodyType::Static;
        floorDef.position = Vec2(Real(64), Real(110));
        floorDef.shape    = MakeAabb(Real(200), Real(10));
        world.AddBody(floorDef);

        // Dynamic circle A at (40, 90) -- radius 10, just above the floor.
        BodyDef circA;
        circA.type     = BodyType::Dynamic;
        circA.position = Vec2(Real(40), Real(90));
        circA.shape    = MakeCircle(Real(10));
        circA.density  = Real(1);
        world.AddBody(circA);

        // Dynamic circle B at (57, 90) -- clearly overlapping A (center distance 17 <
        // sum of radii 20) so a dynamic-dynamic contact is guaranteed from step 1,
        // making the ForEachContact / magenta contact-line draw path reliably fire.
        BodyDef circB;
        circB.type     = BodyType::Dynamic;
        circB.position = Vec2(Real(57), Real(90));
        circB.shape    = MakeCircle(Real(10));
        circB.density  = Real(1);
        world.AddBody(circB);

        // Kinematic mover: capsule hovering above the floor.
        BodyDef moverDef;
        moverDef.type     = BodyType::Kinematic;
        moverDef.position = Vec2(Real(100), Real(70));
        moverDef.shape    = MakeCapsule(Real(15), Real(8));
        world.AddBody(moverDef);

        // Step enough times for the dynamic bodies to settle, contacts to
        // register (begun == true) and an island to form.
        constexpr Real kDt = Real(1) / Real(60);
        for (int i = 0; i < 60; ++i)
            world.Step(kDt);

        // ---- record the debug-draw pass ------------------------------------
        nvrhi::CommandListHandle cl = device->Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                              nvrhi::Color(0, 0, 0, 1));

        batcher->Begin(cl, canvas->Framebuffer(),
                       canvas->Width(), canvas->Height());

        Arcane::PhysicsDebugDrawOptions opts;
        opts.drawContacts = true;
        opts.drawAabbs    = false;   // optional; off for a clean minimal test
        Arcane::DrawPhysicsDebug(world, *batcher, opts);

        batcher->End();
        cl->close();
        device->Nvrhi()->executeCommandList(cl);
        device->Nvrhi()->waitForIdle();

        // ---- asserts -------------------------------------------------------
        //
        // Every primitive (line, circle, rect) becomes quads in the batcher.
        // We have at least: 3 shape outlines (floor rect uses 4 lines = 4 quads;
        // each circle = 1 quad; capsule = 2 circles + 2 lines = 4 quads) -> well
        // above zero.  Use a conservative lower bound of 1.
        Arcane::Batch2DStats stats = batcher->Stats();
        CHECK(stats.quads > 0);

        // GPU / NVRHI validation must stay silent.
        CHECK(Arcane::RenderErrorCount() == 0);

        device->Nvrhi()->runGarbageCollection();
    }
} // namespace

TEST_CASE("d3d12: physics debug draw submits geometry with no validation errors",
          "[gpu][physics][d3d12]")
{
    RunDebugDraw(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: physics debug draw submits geometry with no validation errors",
          "[gpu][physics][vulkan]")
{
    RunDebugDraw(Arcane::GraphicsBackend::Vulkan);
}
