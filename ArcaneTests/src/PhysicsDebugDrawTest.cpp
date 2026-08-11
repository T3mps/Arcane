// Physics M6 P3.6: PhysicsDebugDraw -- GPU smoke test.
//
// Verifies that DrawPhysicsDebug() submits at least some geometry to the
// Batcher2D (lines + circles) and that the NVRHI validation layer stays
// silent (RenderErrorCount() == 0) on both backends.
//
// Scene (MKS, meters): a static AABB floor, two dynamic circles resting on
// it (so a contact pair forms + an island unites them), and one kinematic
// mover above.  After ~60 steps the contact manager has begun pairs and the
// island pass has written m_uf.  DrawPhysicsDebug runs over that state and
// submits primitives; the test asserts at least one quad was emitted (lines
// and circles both become quads under the batcher's hood) and that no
// GPU/NVRHI errors occurred.
//
// Camera: opts.zoom/cameraOffset fold the compact ~1.2 m x 0.68 m scene into
// the 128x128 offscreen canvas at the Sandbox's pixelsPerMeter=100 convention
// (see the derivation comment at the DrawPhysicsDebug call site below), so the
// flipped rich-overlay marker magnitudes (0.05/0.18/0.03 world units) rasterize
// multi-pixel instead of sub-pixel at identity zoom.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Manifold2D/Physics/Body.hpp>
#include <Manifold2D/Physics/PhysicsTypes.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>
#include <Manifold2D/Physics/Shapes.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/PhysicsDebugDraw.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

using namespace Manifold2D::Physics;

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

        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "data/shaders");
        REQUIRE(shaders != nullptr);

        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 128, 128);
        REQUIRE(canvas != nullptr);

        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(batcher != nullptr);

        // ---- build physics scene -------------------------------------------
        //
        // World with gravity so the dynamic circles settle onto the floor
        // and the island/sleep pass runs. gravityY is the MKS ctor default
        // (10 m/s^2) -- this compact scene settles well within the 60-step
        // budget below, so no override is needed.
        WorldDef wdef;
        PhysicsWorld world(wdef);

        // Static floor: AABB 0.6 m half-wide, 0.1 m half-tall, centered at
        // (0, 0.6). Floor top surface is at y = 0.5.
        BodyDef floorDef;
        floorDef.type     = BodyType::Static;
        floorDef.position = Vec2(Real(0), Real(0.6));
        floorDef.shape    = MakeAabb(Real(0.6), Real(0.1));
        world.AddBody(floorDef);

        // Dynamic circle A at (-0.15, 0.35) -- radius 0.15 m, resting exactly
        // on the floor top (bottom = 0.5, zero gap).
        BodyDef circA;
        circA.type     = BodyType::Dynamic;
        circA.position = Vec2(Real(-0.15), Real(0.35));
        circA.shape    = MakeCircle(Real(0.15));
        circA.density  = Real(1);
        world.AddBody(circA);

        // Dynamic circle B at (0.09, 0.35) -- clearly overlapping A (center
        // distance 0.24 m < sum of radii 0.3 m) so a dynamic-dynamic contact
        // is guaranteed from step 1, making the ForEachContact / magenta
        // contact-line draw path reliably fire.
        BodyDef circB;
        circB.type     = BodyType::Dynamic;
        circB.position = Vec2(Real(0.09), Real(0.35));
        circB.shape    = MakeCircle(Real(0.15));
        circB.density  = Real(1);
        world.AddBody(circB);

        // Kinematic mover: capsule hovering above the floor.
        BodyDef moverDef;
        moverDef.type     = BodyType::Kinematic;
        moverDef.position = Vec2(Real(0.3), Real(0.1));
        moverDef.shape    = MakeCapsule(Real(0.2), Real(0.08));
        world.AddBody(moverDef);

        // Step enough times for the dynamic bodies to settle, contacts to
        // register (begun == true) and an island to form. Circles start
        // touching the floor (zero gap), same as the px-era scene, so the
        // unchanged 60-step (1 s @ 60 Hz) budget is re-verified sufficient
        // at the new MKS scale/gravity.
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

        // Zoom fold: mirror the Sandbox's pixelsPerMeter=100 convention so the
        // flipped marker magnitudes (comMarkerSize 0.05 m, orientationTickLen
        // 0.18 m, contactMarkerSize 0.03 m) rasterize multi-pixel instead of
        // sub-pixel at identity zoom. Scene bounding box (floor + circles +
        // mover) is x[-0.6, 0.6] y[0.02, 0.7] (~1.2 m x 0.68 m); its center
        // (0, 0.36) must map to the canvas center (64, 64) at zoom 100 ->
        // offset = (64, 64) - (0, 0.36) * 100 = (64, 28), which fits the whole
        // scene inside the 128x128 canvas with margin on every side.
        Arcane::PhysicsDebugDrawOptions opts;
        opts.zoom         = 100.0f;
        opts.cameraOffset = glm::vec2(64.0f, 28.0f);
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
        // Batch2DStats exposes only { drawCalls, quads } (Batcher2D.hpp:58-62)
        // -- no per-primitive-type breakdown, so this cannot discriminate a
        // marker quad from an outline quad. It CAN discriminate whether the
        // rich-overlay code paths ran at all, via a computed floor that is
        // deterministic (independent of settle/contact/sleep outcome, unlike
        // the old ">0" bound): floor outline 4 + circle A 1 + circle B 1 +
        // capsule outline (2 circles + 2 lines) 4 + 4 orientation ticks (one
        // per alive body, unconditional on type/awake) + 4 COM-marker lines (2
        // dynamic bodies x 2, gated on type but NOT on awake) = 18. This still
        // does not prove the markers rasterize at a VISIBLE size -- that is
        // the pre-existing "no-render-error != pixels visible" gap (no pixel-
        // readback in this test); recorded as a residual, not fixed here.
        Arcane::Batch2DStats stats = batcher->Stats();
        CHECK(stats.quads >= 18);

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
