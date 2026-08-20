// NODE-LEVEL PIXEL CORRECTNESS for the NRI frame graph -- [gpu], DESK ONLY.
//
// ===== WHY THIS FILE EXISTS ==================================================
// NRI Phase 5a retired ~2,200 lines across seven [gpu] test files along with
// the NVRHI render passes they drove (MaterialChainGpuTest, MaterialGpuTest,
// OffscreenCanvasTest, PickBufferTest, PostChainGpuTest, SelectionOutlineTest,
// SpriteMaterialGpuTest, plus two ImGui pixel cases) and NOTHING replaced them.
// At that point the entire suite's [gpu] coverage was TWO cases, both in
// NriSubstrateTest.cpp, and both device-WRAP smokes: they prove a native device
// survives the trip into NRI and say nothing whatsoever about pixels.
//
// The 12 frozen golden PNGs cover COMPOSITE output -- one image per
// (host, backend, stage). They are a real floor and they catch a whole-frame
// regression, but they cannot localize one: a golden that moves tells you the
// frame changed, not which node changed it. Node-level cases are what turn a
// pixel regression from a desk session into a test run, and that difference
// compounds with every pass the renderer gains.
//
// ===== WHAT IS ASSERTED EXACTLY, AND WHAT IS ASSERTED STRUCTURALLY ===========
// This distinction is deliberate and load-bearing, so it is stated up front.
//
//   * THE PICK ID PASS IS EXACT. Its output is an R32_UINT hit-proxy id, not a
//     colour: no tonemap, no colour space, no filtering. `k+1 for the k-th
//     drawable, 0 for background` is an integer contract and is asserted as
//     one. This is where the value of the file is concentrated -- it is the
//     coverage PickBufferTest lost, and it is exactly reproducible.
//
//   * COLOUR IS ASSERTED STRUCTURALLY. Everything reaching ReadCapture has been
//     through AddTonemapNode, so the canvas clear ({0.02, 0.02, 0.04, 1.0},
//     Batch2DNode.cpp) does NOT arrive as those bytes and a literal expectation
//     would be pinning the tonemap curve by accident. These cases assert
//     relations that survive any sane curve -- "the drawn rect is far brighter
//     than the background", "red dominates green and blue there", "the pixel
//     outside the rect matches the pixel in the far corner". A case that wants
//     to pin the curve itself belongs in the goldens, which already do.
//
// ===== WHY AN OFFSCREEN VEHICLE ==============================================
// NriGraphContext::CreateOffscreen builds the real graph -- same nodes, same
// barriers, same RenderFrame path -- with NO window and NO swapchain, and hands
// back pixels through ReadCapture(). So these run headless on a machine with a
// GPU, without the windowed-run hazard that makes host runs a desk-only affair
// (see the [gpu] note below). The editor already uses this exact vehicle for
// its viewport, so the code under test is the code that ships.
//
// EVERYTHING GOES THROUGH EXPORTED Arcane CLASSES, never a raw nri* call. This
// exe links its OWN static copy of NRI, so a bare nri* function here would run
// against a different function table than the device it was handed. Both
// NativeDeviceOwner and NriDevice are ARCANE_API, so `Wrap` executes inside
// ArcaneClient.dll and the device that comes back is the DLL's -- which is the
// one CreateOffscreen must be given. NriSubstrateTest.cpp's wrap smoke carries
// the same rule for the same reason.
//
// ===== [gpu] MEANS DESK, AND THESE HAVE NOT BEEN RUN =========================
// The agent gate is `ArcaneTests.exe "~[gpu]"`, so nothing here executes there;
// what the gate DOES prove about this file is that it compiles and links, which
// is not nothing (every API contract below is checked by the compiler). The
// pixel assertions themselves are UNRUN as committed. Run them at the desk:
//
//     cd bin\Debug-windows-x86_64-md\ArcaneTests
//     ArcaneTests.exe "[pixel]"                 # both backends
//     ArcaneTests.exe "[pixel][d3d12]"          # one backend
//
// A first run that fails is far more likely to be a wrong expectation here than
// a renderer defect -- read the failure before believing it, and prefer fixing
// the expectation to re-baselining anything.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>          // kSwapchainFramesInFlight -- the probe latency
#include <Arcane/Render/PickEmit.hpp>             // PickDrawable -- the id pass's input
#include <Arcane/Render/RenderErrorLatch.hpp>     // the shared 0/0 latch every case guards
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdlib>      // std::abs over the integer luma difference
#include <memory>
#include <optional>
#include <span>         // FrameDesc::pickables / ::selectedIds are spans
#include <vector>

namespace
{
    // The capture target. Small on purpose: every assertion below is about a
    // handful of named texels, and a smaller target makes a desk run cheap.
    // Not square, so a transposed x/y anywhere in the chain shows up as an
    // out-of-range read rather than a plausible-looking wrong pixel.
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;

    // The whole vehicle, in destruction order. Declaration order IS teardown
    // order here and it matters: the graph context borrows the NriDevice, and
    // the NriDevice wraps the native one (contract item 15 -- NRI device first,
    // native second), so the members are declared in the order they are built
    // and the compiler-generated destructor unwinds them correctly.
    struct PixelVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner>  native;
        std::unique_ptr<Arcane::NriDevice>          nri;
        std::unique_ptr<Arcane::NriGraphContext>    ctx;

        [[nodiscard]] bool Ok() const noexcept { return ctx != nullptr; }
    };

    // Builds device -> NRI wrap -> offscreen graph context. REQUIREs rather
    // than returning a failure, because every case below is meaningless without
    // one and a null-check cascade would just move the failure further from its
    // cause. A machine with no usable adapter fails here, loudly, at the first
    // step -- which is the correct place for "this desk cannot run [gpu]".
    PixelVehicle MakeVehicle(Arcane::GraphicsBackend backend,
                             const Arcane::NriGraphContext::NodeSet& nodes = {})
    {
        PixelVehicle v;

        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);

        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = backend;
        // vsync is meaningless with nothing to present to; CreateOffscreen
        // ignores it and the surface-owned knobs. Left at its default rather
        // than set, so this does not imply otherwise.

        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kW, kH, nodes);
        REQUIRE(v.ctx != nullptr);
        REQUIRE(v.ctx->IsOffscreen());
        REQUIRE(v.ctx->SurfaceWidth()  == kW);
        REQUIRE(v.ctx->SurfaceHeight() == kH);
        return v;
    }

    struct Rgba
    {
        std::uint8_t r = 0, g = 0, b = 0, a = 0;
    };

    // ReadCapture hands back TIGHT RGBA8 (it swizzles from BGRA itself when the
    // target resolved that way), so the row stride is exactly width*4 and the
    // channel order is display-referred RGBA whatever NRI picked.
    Rgba At(const std::vector<unsigned char>& rgba, std::uint32_t w,
            std::uint32_t x, std::uint32_t y)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        REQUIRE(i + 3u < rgba.size());
        return Rgba{ rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3] };
    }

    [[nodiscard]] int Luma(const Rgba& p) noexcept
    {
        // Deliberately crude and integer: this is only ever used for "much
        // brighter than", never for a colour-accurate comparison.
        return static_cast<int>(p.r) + static_cast<int>(p.g) + static_cast<int>(p.b);
    }

    // Renders one frame and REQUIREs it actually rendered. Skipped is a real
    // outcome for a collapsed panel, but this vehicle is created at a fixed
    // non-zero extent and never resized, so Skipped here is a defect and must
    // not be tolerated into a pixel assertion.
    void RenderOne(Arcane::NriGraphContext& ctx,
                   const Arcane::NriGraphContext::FrameDesc& frame)
    {
        const auto outcome = ctx.RenderFrameOffscreen(frame);
        REQUIRE(outcome == Arcane::NriGraphContext::FrameOutcome::Presented);
    }

    // A batcher carrying one axis-aligned rect, ready to hand to FrameDesc.
    // Begin() opens the batch against the canvas extent; the graph's Batch2DNode
    // is what Drains it, so this must NOT drain it here.
    std::unique_ptr<Arcane::Batcher2D> BatchOneRect(glm::vec2 pos, glm::vec2 size,
                                                    glm::vec4 color)
    {
        auto batcher = Arcane::Batcher2D::Create();
        REQUIRE(batcher != nullptr);
        batcher->Begin(kW, kH);
        batcher->SetLayer(0, 0);
        batcher->Rect(pos, size, color);
        return batcher;
    }
}

// ---------------------------------------------------------------------------
// 1. THE VEHICLE ITSELF: a frame renders, captures, and comes back the size it
//    was asked for. Everything below depends on this, so it is its own case --
//    a failure here localises to the harness rather than to a node.
// ---------------------------------------------------------------------------
namespace
{
    void CheckCaptureRoundTrip(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.stage   = Arcane::GoldenStage::Batch;   // no post chain, no HUD
        frame.capture = true;
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        CHECK(w == kW);
        CHECK(h == kH);
        // TIGHT rgba8: exactly four bytes per texel, no row padding.
        CHECK(rgba.size() == static_cast<std::size_t>(kW) * kH * 4u);

        // The canvas clear is opaque ({..., 1.0}), so alpha must survive the
        // chain. A zero alpha here means the capture read an untouched buffer.
        const Rgba corner = At(rgba, w, 0, 0);
        CHECK(corner.a == 255);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: an offscreen graph frame captures at the requested extent (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckCaptureRoundTrip(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: an offscreen graph frame captures at the requested extent (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckCaptureRoundTrip(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 2. ReadCapture REFUSES A FRAME THAT DID NOT ASK TO BE CAPTURED. This is the
//    contract that keeps a golden run from silently comparing a stale buffer --
//    the accessor's own comment states it, and nothing pinned it.
// ---------------------------------------------------------------------------
namespace
{
    void CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend backend)
    {
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.stage   = Arcane::GoldenStage::Batch;
        frame.capture = false;                       // the whole point
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // False, already logged. NOT a run failure -- the same exit-3 "capture
        // failed" class the caller is told not to escalate.
        CHECK_FALSE(v.ctx->ReadCapture(w, h, rgba));
    }
}

TEST_CASE("pixel: ReadCapture refuses when no capture node was recorded (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: ReadCapture refuses when no capture node was recorded (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 3. SPRITE/BATCHER PIXEL CORRECTNESS -- the coverage SpriteMaterialGpuTest and
//    MaterialGpuTest lost. A batched rect must land INSIDE its own canvas
//    rectangle and nowhere else.
//
//    Structural, not literal (see the file header): the rect is pure red at
//    full intensity against a near-black clear, so "red dominates" and "far
//    brighter than the corner" hold under any tonemap curve, while an exact
//    byte would be pinning the curve.
// ---------------------------------------------------------------------------
namespace
{
    void CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();
        PixelVehicle v = MakeVehicle(backend);

        // A rect in the upper-left quadrant, inset from every edge so that
        // "inside" and "outside" samples are both unambiguous and no assertion
        // sits on the boundary texel (where filtering/rounding is a coin flip).
        constexpr float kX = 20.0f, kY = 16.0f, kWide = 40.0f, kTall = 24.0f;
        auto batcher = BatchOneRect(glm::vec2(kX, kY), glm::vec2(kWide, kTall),
                                    glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));

        Arcane::NriGraphContext::FrameDesc frame;
        frame.stage   = Arcane::GoldenStage::Batch;   // isolate the batcher: no post chain, no HUD
        frame.capture = true;
        frame.batch   = batcher.get();
        RenderOne(*v.ctx, frame);

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));

        // Dead centre of the rect.
        const Rgba inside = At(rgba, w,
                               static_cast<std::uint32_t>(kX + kWide  * 0.5f),
                               static_cast<std::uint32_t>(kY + kTall * 0.5f));
        // Well outside it, and far from every edge of the canvas.
        const Rgba outside = At(rgba, w, kW - 10u, kH - 10u);

        // THE COLOUR: red dominates both other channels by a wide margin.
        CHECK(inside.r > inside.g + 60);
        CHECK(inside.r > inside.b + 60);
        // THE PLACEMENT: the rect is far brighter than the untouched clear, and
        // the outside sample is NOT -- which together are what "landed in its
        // own rectangle" means. Asserting only the first would pass a shader
        // that filled the whole canvas red.
        CHECK(Luma(inside) > Luma(outside) + 120);
        CHECK(outside.r < 96);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: a batched rect lands inside its own canvas rectangle (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: a batched rect lands inside its own canvas rectangle (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckBatchedRectLandsInItsRectangle(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 4. THE PICK ID PASS -- the largest single loss (PickBufferTest), and the one
//    place in this file where the contract is an exact integer.
//
//    THE CONVENTION UNDER TEST: the k-th entry of FrameDesc::pickables IS
//    hit-proxy id k+1, and 0 is background. `PickDrawable::entity` is NOT read
//    by the id pass -- it is the host's own reverse-mapping label -- so these
//    cases need no registry and leave it default-constructed deliberately.
//
//    THE LATENCY IS PART OF THE CONTRACT: the readback rides
//    kSwapchainFramesInFlight frames behind, so ProbeId() is nullopt until that
//    many frames have gone by. Rendering exactly kSwapchainFramesInFlight + 1
//    frames and requiring a value on the last is what pins the latency at its
//    documented depth -- a regression that made it deeper would fail here
//    rather than silently return a stale id to a host.
// ---------------------------------------------------------------------------
namespace
{
    // Renders the same pick frame repeatedly until the readback has had time to
    // land, then returns what the probe read.
    std::optional<std::uint32_t> ProbeAt(PixelVehicle& v,
                                         std::span<const Arcane::PickDrawable> drawables,
                                         glm::ivec2 pixel,
                                         std::uint64_t ticket = 0)
    {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.stage       = Arcane::GoldenStage::Batch;
        frame.pickOutline = true;          // declares the pick node + readback + JFA chain
        frame.pickables   = drawables;
        frame.pickPixel   = pixel;
        frame.pickTicket  = ticket;

        for (std::uint32_t i = 0; i < Arcane::kSwapchainFramesInFlight; ++i)
        {
            RenderOne(*v.ctx, frame);
            // Nothing has come back yet, by construction. Asserted rather than
            // assumed: this IS the latency contract.
            CHECK_FALSE(v.ctx->ProbeId().has_value());
        }
        RenderOne(*v.ctx, frame);
        return v.ctx->ProbeId();
    }

    void CheckPickIdPass(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        // Two well-separated quads. Index 0 -> id 1, index 1 -> id 2.
        Arcane::PickDrawable a;
        a.kind        = Arcane::PickDrawable::Kind::Quad;
        a.center      = glm::vec2(40.0f, 24.0f);
        a.halfExtents = glm::vec2(16.0f, 10.0f);

        Arcane::PickDrawable b;
        b.kind        = Arcane::PickDrawable::Kind::Quad;
        b.center      = glm::vec2(118.0f, 68.0f);
        b.halfExtents = glm::vec2(16.0f, 10.0f);

        const Arcane::PickDrawable drawables[] = { a, b };

        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(40, 24));
            REQUIRE(id.has_value());
            CHECK(*id == 1u);            // the 0th drawable IS id 1
        }
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(118, 68));
            REQUIRE(id.has_value());
            CHECK(*id == 2u);            // the 1st drawable IS id 2
        }
        {
            // Background. 0 is a legitimate VALUE, not a missing readback --
            // the accessor is explicit that the caller decides 0 means miss --
            // so this must come back engaged AND zero, and asserting both is
            // what separates "read background" from "read nothing".
            PixelVehicle v = MakeVehicle(backend, nodes);
            const auto id = ProbeAt(v, drawables, glm::ivec2(5, 90));
            REQUIRE(id.has_value());
            CHECK(*id == 0u);
        }

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pick: the id pass reads k+1 for the k-th drawable and 0 for background (d3d12)",
          "[gpu][pixel][pick][nri][d3d12]")
{
    CheckPickIdPass(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pick: the id pass reads k+1 for the k-th drawable and 0 for background (vulkan)",
          "[gpu][pixel][pick][nri][vulkan]")
{
    CheckPickIdPass(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 5. THE PROBE TICKET LABELS THE READBACK. A host that probes a different pixel
//    every frame cannot otherwise tell which request an answer belongs to, and
//    the editor's deferred pick depends on exactly this.
// ---------------------------------------------------------------------------
namespace
{
    void CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend backend)
    {
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        Arcane::PickDrawable a;
        a.kind        = Arcane::PickDrawable::Kind::Quad;
        a.center      = glm::vec2(40.0f, 24.0f);
        a.halfExtents = glm::vec2(16.0f, 10.0f);
        const Arcane::PickDrawable drawables[] = { a };

        PixelVehicle v = MakeVehicle(backend, nodes);

        constexpr std::uint64_t kTicket = 4242u;
        const auto id = ProbeAt(v, drawables, glm::ivec2(40, 24), kTicket);
        REQUIRE(id.has_value());
        CHECK(*id == 1u);

        const auto result = v.ctx->ProbeResult();
        REQUIRE(result.has_value());
        CHECK(result->id     == 1u);
        CHECK(result->ticket == kTicket);   // the label the copy carried

        // IT DOES NOT SELF-CLEAR: reading twice gives the same answer, which is
        // the documented behaviour and the reason acting on it exactly once is
        // the host's job rather than the vehicle's.
        const auto again = v.ctx->ProbeResult();
        REQUIRE(again.has_value());
        CHECK(again->id     == 1u);
        CHECK(again->ticket == kTicket);
    }
}

TEST_CASE("pick: the readback carries the ticket its frame was armed with (d3d12)",
          "[gpu][pixel][pick][nri][d3d12]")
{
    CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pick: the readback carries the ticket its frame was armed with (vulkan)",
          "[gpu][pixel][pick][nri][vulkan]")
{
    CheckProbeTicketRidesWithTheId(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 6. THE SELECTION-OUTLINE JFA -- SelectionOutlineTest's twelve cases were the
//    single largest block retired, and this is the property they existed for:
//    a SELECTED silhouette grows an outline, and an EMPTY selection grows none.
//
//    The two halves must be one case. "Outline pixels appeared" alone would
//    pass a chain that outlines everything; "no outline pixels" alone would
//    pass a chain that is simply switched off. Rendering the same scene twice,
//    differing ONLY in selectedIds, is what makes the difference attributable.
// ---------------------------------------------------------------------------
namespace
{
    // Renders one capture frame with the pick/outline chain armed and returns
    // the pixels. `selected` is the union of hit-proxy ids the outline traces.
    std::vector<unsigned char> CaptureWithOutline(
        PixelVehicle& v,
        std::span<const Arcane::PickDrawable> drawables,
        std::span<const std::uint32_t> selected,
        std::uint32_t& w, std::uint32_t& h)
    {
        Arcane::NriGraphContext::FrameDesc frame;
        frame.stage       = Arcane::GoldenStage::Batch;
        frame.capture     = true;
        frame.pickOutline = true;
        frame.pickables   = drawables;
        frame.selectedIds = selected;
        // No hover: (-1,-1) is the "no hover" convention the outline seed
        // understands, so the ONLY thing that can produce an outline in these
        // frames is `selected`. Left unset would arm the config's probe pixel
        // and give the seed a cursor, which is precisely the confound.
        frame.hoverPixel  = glm::ivec2(-1, -1);
        RenderOne(*v.ctx, frame);

        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    // Counts texels that differ meaningfully between two captures of the same
    // scene. With only selectedIds varying, every difference IS outline.
    std::size_t DifferingTexels(const std::vector<unsigned char>& a,
                                const std::vector<unsigned char>& b,
                                std::uint32_t w, std::uint32_t h)
    {
        REQUIRE(a.size() == b.size());
        std::size_t n = 0;
        for (std::uint32_t y = 0; y < h; ++y)
        {
            for (std::uint32_t x = 0; x < w; ++x)
            {
                const Rgba pa = At(a, w, x, y);
                const Rgba pb = At(b, w, x, y);
                // A generous threshold: this counts OUTLINE, and an outline is
                // a strong colour, not a rounding difference between two runs.
                if (std::abs(Luma(pa) - Luma(pb)) > 40) ++n;
            }
        }
        return n;
    }

    void CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        Arcane::NriGraphContext::NodeSet nodes;
        nodes.pickOutline = true;

        Arcane::PickDrawable a;
        a.kind        = Arcane::PickDrawable::Kind::Quad;
        a.center      = glm::vec2(60.0f, 40.0f);
        a.halfExtents = glm::vec2(20.0f, 14.0f);
        const Arcane::PickDrawable drawables[] = { a };

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;

        // (a) NOTHING SELECTED: the chain is declared and runs, and discards
        //     every pixel. This is the Play-mode arming.
        std::vector<unsigned char> none;
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            none = CaptureWithOutline(v, drawables, {}, w0, h0);
        }

        // (b) THE ONE SILHOUETTE SELECTED (hit-proxy id 1 == drawables[0]).
        std::vector<unsigned char> one;
        {
            PixelVehicle v = MakeVehicle(backend, nodes);
            const std::uint32_t selected[] = { 1u };
            one = CaptureWithOutline(v, drawables, selected, w1, h1);
        }

        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);

        const std::size_t changed = DifferingTexels(none, one, w0, h0);

        // AN OUTLINE APPEARED. A border around a 40x28 silhouette is on the
        // order of a few hundred texels; the floor is deliberately far below
        // that so a thickness change does not fail the case -- the property is
        // "the selection is traced", not "the trace is N texels wide".
        CHECK(changed > 40u);

        // AND IT IS AN OUTLINE, NOT A FILL OR A FULL-SCREEN EFFECT. The
        // silhouette is 40x28 = 1120 texels of a 160x96 = 15360 canvas; an
        // outline plus its falloff must stay well under a fill of the whole
        // thing. Without this bound, a chain that tinted the entire canvas on
        // selection would pass the floor above.
        CHECK(changed < (static_cast<std::size_t>(w0) * h0) / 4u);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("outline: the JFA traces the selected silhouette and nothing when the selection is empty (d3d12)",
          "[gpu][pixel][outline][nri][d3d12]")
{
    CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("outline: the JFA traces the selected silhouette and nothing when the selection is empty (vulkan)",
          "[gpu][pixel][outline][nri][vulkan]")
{
    CheckSelectionOutlineTracesOnlyTheSelection(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 7. STAGE GATING IS REAL AT THE PIXEL LEVEL. `batch` drops the post chain and
//    `full` is the only stage that draws a HUD -- the same gates RuntimeApp
//    applies, and the reason a batch golden and a full golden of one scene are
//    different images. PostChainGpuTest covered the post half; nothing does now.
//
//    With no post chain and no HUD supplied, batch and full must agree EXACTLY:
//    same nodes reached, same pixels. That is a cheap, strong invariant -- it
//    catches a stage gate that accidentally changes the frame's shape when
//    there is no stage-gated content to add.
// ---------------------------------------------------------------------------
namespace
{
    void CheckStagesAgreeWithNoStageGatedContent(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        auto capture = [&](Arcane::GoldenStage stage, std::uint32_t& w, std::uint32_t& h)
        {
            PixelVehicle v = MakeVehicle(backend);
            auto batcher = BatchOneRect(glm::vec2(30.0f, 20.0f), glm::vec2(50.0f, 30.0f),
                                        glm::vec4(0.2f, 0.8f, 0.3f, 1.0f));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.stage   = stage;
            frame.capture = true;
            frame.batch   = batcher.get();
            // post = null ("no chain"), imgui = null, gameUi = null: nothing
            // that either stage would gate differently.
            RenderOne(*v.ctx, frame);
            std::vector<unsigned char> rgba;
            REQUIRE(v.ctx->ReadCapture(w, h, rgba));
            return rgba;
        };

        std::uint32_t wb = 0, hb = 0, wf = 0, hf = 0;
        const std::vector<unsigned char> batch = capture(Arcane::GoldenStage::Batch, wb, hb);
        const std::vector<unsigned char> full  = capture(Arcane::GoldenStage::Full,  wf, hf);

        REQUIRE(wb == wf);
        REQUIRE(hb == hf);
        // EXACT, not tolerant: two runs of the same nodes over the same content
        // on the same device are deterministic. A tolerance here would hide the
        // very drift the case exists to catch.
        CHECK(batch == full);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: batch and full stages agree when no stage-gated content is supplied (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckStagesAgreeWithNoStageGatedContent(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: batch and full stages agree when no stage-gated content is supplied (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckStagesAgreeWithNoStageGatedContent(Arcane::GraphicsBackend::Vulkan);
}
