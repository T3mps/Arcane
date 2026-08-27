// NODE-LEVEL PIXEL CORRECTNESS for the NRI frame graph -- [gpu], DESK ONLY.
//
// ===== WHY THIS FILE EXISTS ==================================================
// Without it the suite's entire [gpu] coverage is TWO cases, both in
// NriSubstrateTest.cpp, and both device-WRAP smokes: they prove a native device
// survives the trip into NRI and say nothing whatsoever about pixels.
//
// A whole-frame image comparison could only say THAT the frame changed, never
// WHICH node changed it. Node-level cases are what turn a pixel regression from
// a desk session into a test run, and that difference compounds with every pass
// the renderer gains.
//
// ===== WHAT IS ASSERTED EXACTLY, AND WHAT IS ASSERTED STRUCTURALLY ===========
// This distinction is deliberate and load-bearing, so it is stated up front.
//
//   * THE PICK ID PASS IS EXACT. Its output is an R32_UINT hit-proxy id, not a
//     colour: no tonemap, no colour space, no filtering. `k+1 for the k-th
//     drawable, 0 for background` is an integer contract and is asserted as
//     one. This is where the value of the file is concentrated, and it is
//     exactly reproducible.
//
//   * COLOUR IS ASSERTED STRUCTURALLY. Everything reaching ReadCapture has been
//     through AddTonemapNode, so the canvas clear ({0.02, 0.02, 0.04, 1.0},
//     Batch2DNode.cpp) does NOT arrive as those bytes and a literal expectation
//     would be pinning the tonemap curve by accident. These cases assert
//     relations that survive any sane curve -- "the drawn rect is far brighter
//     than the background", "red dominates green and blue there", "the pixel
//     outside the rect matches the pixel in the far corner". Pinning the curve
//     itself is a different job and does not belong in these cases.
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
// a renderer defect -- read the failure before believing it.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>          // kSwapchainFramesInFlight -- the probe latency
#include <Arcane/Render/PickEmit.hpp>             // PickDrawable -- the id pass's input
#include <Arcane/Render/RenderErrorLatch.hpp>     // the shared 0/0 latch every case guards
#include <Arcane/Render/MeshBuilder.hpp>          // BuildCube -- the opaque pass's geometry
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>   // MeshInstance / MeshSceneDesc
#include <Arcane/Scene/SceneCamera.hpp>           // PerspectiveProjection -- the camera under test

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>           // lookAtRH, translate

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
#if defined(ARCANE_DEBUG)
        // Mirror OffscreenVehicle::Create / NriGraphContext.cpp's windowed
        // creation half EXACTLY (same three flags, same Debug-only gate).
        // These [gpu][pixel] cases are node-level frame-graph tests -- they
        // exercise hand- and graph-derived barrier placement more directly
        // than anything else in the tree, which is precisely the defect
        // class Vulkan sync validation catches and core validation does not
        // (RenderDeviceDesc.hpp). Leaving this vehicle at RenderDeviceDesc's
        // defaults would make the best-placed cases in the suite for finding
        // barrier hazards the ones running with the least validation.
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;   // VK-only; see RenderDeviceDesc.hpp
#endif
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
//    contract that keeps a capture consumer from silently reading a stale
//    buffer -- the accessor's own comment states it, and nothing pinned it.
// ---------------------------------------------------------------------------
namespace
{
    void CheckCaptureRefusesWithoutACaptureFrame(Arcane::GraphicsBackend backend)
    {
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
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
// 7. THE SAME DECLARED FRAME IS BYTE-DETERMINISTIC. Two vehicles, same content,
//    same nodes -> identical pixels. Cheap, and it is the invariant every other
//    case in this file silently depends on: a comparison is only meaningful if
//    a re-run of the same frame reproduces itself exactly.
//
//    IT ALSO PINS THE FLAG SURFACE. `post`, `gameUi` and `imgui` are null here,
//    which is how a caller asks for the frame WITHOUT those passes (see
//    FrameDesc). If nulling them ever stopped meaning "omit the node", this
//    frame would grow passes and stop matching itself.
// ---------------------------------------------------------------------------
namespace
{
    void CheckFrameIsByteDeterministic(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        auto capture = [&](std::uint32_t& w, std::uint32_t& h)
        {
            PixelVehicle v = MakeVehicle(backend);
            auto batcher = BatchOneRect(glm::vec2(30.0f, 20.0f), glm::vec2(50.0f, 30.0f),
                                        glm::vec4(0.2f, 0.8f, 0.3f, 1.0f));
            Arcane::NriGraphContext::FrameDesc frame;
            frame.capture = true;
            frame.batch   = batcher.get();
            // post / gameUi / imgui all left null: no chain, no game HUD, no
            // host chrome. That IS the "batch slice" now.
            RenderOne(*v.ctx, frame);
            std::vector<unsigned char> rgba;
            REQUIRE(v.ctx->ReadCapture(w, h, rgba));
            return rgba;
        };

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
        const std::vector<unsigned char> first  = capture(w0, h0);
        const std::vector<unsigned char> second = capture(w1, h1);

        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);
        // EXACT, not tolerant: two runs of the same nodes over the same content
        // on the same device are deterministic. A tolerance here would hide the
        // very drift the case exists to catch.
        CHECK(first == second);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("pixel: the same declared frame reproduces itself byte-for-byte (d3d12)",
          "[gpu][pixel][nri][d3d12]")
{
    CheckFrameIsByteDeterministic(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("pixel: the same declared frame reproduces itself byte-for-byte (vulkan)",
          "[gpu][pixel][nri][vulkan]")
{
    CheckFrameIsByteDeterministic(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 8. THE OPAQUE MESH PASS (NRI Phase 4, Task 7) -- the first 3D content this
//    suite has ever put a pixel assertion on, and the ONLY place the DEPTH
//    TEST can be proven at all.
//
//    Everything below is structural, per the file header: the cubes are pure
//    single-channel colours under a white light, so "red dominates green" and
//    "the lit sample is far brighter than the ambient-only one" survive any
//    sane tonemap curve where a literal byte would be pinning it.
//
//    THE CAMERA is built here rather than through a Registry because
//    ActivePerspectiveSceneCamera needs one and these cases have no scene.
//    SceneCamera::PerspectiveProjection is an UNVALIDATED pure function (a zero
//    fov, aspectRatio <= 0 or nearZ >= farZ all silently produce NaN), so the
//    three preconditions it does not check are REQUIREd here before it is
//    called -- which is the same guard ActivePerspectiveSceneCamera applies,
//    stated at the only other call site in the tree.
// ---------------------------------------------------------------------------
namespace
{
    // Eye on +Z looking at the origin down -Z with +Y up -- the engine's
    // right-handed convention (SceneCamera.hpp, Task 5), and the vantage
    // MeshBuilder's CCW-from-outside winding is stated against.
    constexpr float kEyeZ        = 4.0f;
    constexpr float kFovYDegrees = 60.0f;
    constexpr float kNearZ       = 0.1f;
    constexpr float kFarZ        = 100.0f;

    // A flat ambient term big enough that an UNLIT surface is still clearly
    // above the canvas clear -- which is what lets the light's contribution be
    // asserted as a difference rather than as "the cube appeared".
    constexpr float kAmbient = 0.08f;

    void FillCamera(Arcane::MeshSceneDesc& scene)
    {
        const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
        REQUIRE(aspect > 0.0f);
        REQUIRE(kNearZ > 0.0f);
        REQUIRE(kFarZ > kNearZ);
        REQUIRE(kFovYDegrees > 0.0f);

        scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, kEyeZ),
                                   glm::vec3(0.0f, 0.0f, 0.0f),
                                   glm::vec3(0.0f, 1.0f, 0.0f));
        scene.projection = Arcane::PerspectiveProjection(kFovYDegrees, aspect, kNearZ, kFarZ);

        // Pointing TOWARD the camera, so a cube's +Z face -- the one facing the
        // lens -- has N.L == 1 exactly. That makes the lit sample's value a
        // property of the lighting model rather than of the cube's orientation.
        scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
        scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
        scene.ambient        = glm::vec3(kAmbient);
    }

    std::vector<unsigned char> CaptureMesh(Arcane::GraphicsBackend backend,
                                           const Arcane::MeshSceneDesc& scene,
                                           std::uint32_t& w, std::uint32_t& h)
    {
        PixelVehicle v = MakeVehicle(backend);

        Arcane::NriGraphContext::FrameDesc frame;
        frame.capture = true;
        frame.mesh    = &scene;
        RenderOne(*v.ctx, frame);

        std::vector<unsigned char> rgba;
        REQUIRE(v.ctx->ReadCapture(w, h, rgba));
        return rgba;
    }

    // ---- 8a: a lit cube covers the centre and the corners stay background ---
    void CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        // 2 m on a side at the origin, seen from 4 m away through a 60-degree
        // lens: the visible height at the origin plane is 2*tan(30)*4 = 4.6 m,
        // so the cube covers the middle ~43% of the frame and leaves every
        // corner untouched.
        const Arcane::MeshData cube = Arcane::BuildCube(2.0f);

        Arcane::MeshInstance one;
        one.mesh      = &cube;
        one.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);   // pure red, so channels separate cleanly
        const Arcane::MeshInstance instances[] = { one };

        Arcane::MeshSceneDesc scene;
        scene.instances = instances;
        FillCamera(scene);

        std::uint32_t w = 0, h = 0;
        const std::vector<unsigned char> lit = CaptureMesh(backend, scene, w, h);

        // The SAME scene with the directional light switched off. Only
        // `lightColor` differs, so every difference below IS the light.
        Arcane::MeshSceneDesc unlitScene = scene;
        unlitScene.lightColor = glm::vec3(0.0f);
        std::uint32_t uw = 0, uh = 0;
        const std::vector<unsigned char> unlit = CaptureMesh(backend, unlitScene, uw, uh);
        REQUIRE(w == uw);
        REQUIRE(h == uh);

        const Rgba centre = At(lit,   w, w / 2u, h / 2u);
        const Rgba unlitC = At(unlit, w, w / 2u, h / 2u);
        const Rgba corner = At(lit,   w, 10u, 10u);
        // NOT named `far`: <windows.h> still #defines that (and `near`) to
        // nothing, so a local by either name is a syntax error here.
        const Rgba opposite = At(lit, w, w - 10u, h - 10u);

        // THE CUBE IS THERE, AND IT IS RED. Asserting the channel separation
        // as well as the brightness is what rules out "the whole canvas got
        // brighter" -- a clear-colour change would move all three together.
        CHECK(centre.r > centre.g + 60);
        CHECK(centre.r > centre.b + 60);
        CHECK(centre.r > corner.r + 120);
        CHECK(Luma(centre) > Luma(corner));

        // THE CORNERS ARE BACKGROUND, both of them -- the canvas clear, which
        // is near-black and blue-leaning (Batch2DNode's kCanvasClear).
        CHECK(corner.r < 96);
        CHECK(opposite.r < 96);
        CHECK(std::abs(Luma(corner) - Luma(opposite)) < 24);

        // THE LIGHT ACTUALLY LIT IT. Ambient alone still shows the cube (so
        // this is not "the mesh only renders when lit"), and the directional
        // term is a large addition on top -- which is the whole of the Lambert
        // model this pass implements.
        CHECK(unlitC.r > corner.r + 20);
        CHECK(centre.r > unlitC.r + 40);

        CHECK(Arcane::RenderErrorCount() == before);
    }

    // ---- 8b: THE DEPTH TEST -------------------------------------------------
    // The assertion nothing else in this suite can make. Two cubes, one NEARER
    // the camera and smaller on screen, one FARTHER and larger, in two
    // different submission orders:
    //
    //   * the centre pixel is the NEAR cube's colour in BOTH orders. Without a
    //     depth test the last-submitted instance wins, so one of the two orders
    //     would come back the far cube's colour -- which is exactly the failure
    //     this case exists to catch, and exactly what painter's order cannot
    //     be talked out of;
    //   * a pixel outside the near cube's silhouette but inside the far one's
    //     is the FAR cube's colour, which rules out the degenerate pass where
    //     the far cube simply never drew.
    //
    // Back-face culling cannot produce this result on its own: both cubes are
    // separate closed opaque solids, so culling says nothing about which of
    // the two owns a shared pixel.
    void CheckNearMeshOccludesFar(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        // Geometry chosen so the two silhouettes are nested with room to spare
        // at 160x96 (worked out against the projection above, in canvas px):
        //   near cube front face at z = +1.3 (2.7 m out) -> x in [71, 89]
        //   far  cube front face at z =  0.0 (4.0 m out) -> x in [49, 111]
        // so x = 96 on the centre row is unambiguously far-only, 7 px clear of
        // the near cube's edge and 15 px clear of the far cube's.
        const Arcane::MeshData nearCube = Arcane::BuildCube(0.6f);
        const Arcane::MeshData farCube  = Arcane::BuildCube(3.0f);

        Arcane::MeshInstance nearInstance;
        nearInstance.mesh      = &nearCube;
        nearInstance.model     = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        nearInstance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);   // RED = near

        Arcane::MeshInstance farInstance;
        farInstance.mesh      = &farCube;
        farInstance.model     = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.5f));
        farInstance.baseColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);    // GREEN = far

        const Arcane::MeshInstance nearFirst[] = { nearInstance, farInstance };
        const Arcane::MeshInstance farFirst[]  = { farInstance, nearInstance };

        Arcane::MeshSceneDesc nearFirstScene;
        nearFirstScene.instances = nearFirst;
        FillCamera(nearFirstScene);

        Arcane::MeshSceneDesc farFirstScene;
        farFirstScene.instances = farFirst;
        FillCamera(farFirstScene);

        std::uint32_t w0 = 0, h0 = 0, w1 = 0, h1 = 0;
        const std::vector<unsigned char> nearFirstPixels =
            CaptureMesh(backend, nearFirstScene, w0, h0);
        const std::vector<unsigned char> farFirstPixels =
            CaptureMesh(backend, farFirstScene, w1, h1);
        REQUIRE(w0 == w1);
        REQUIRE(h0 == h1);

        for (const std::vector<unsigned char>* pixels : { &nearFirstPixels, &farFirstPixels })
        {
            const Rgba centre = At(*pixels, w0, w0 / 2u, h0 / 2u);
            const Rgba onlyFar = At(*pixels, w0, 96u, h0 / 2u);
            const Rgba corner  = At(*pixels, w0, 10u, 10u);

            // THE OCCLUSION. Red wins the centre whichever order the two were
            // submitted in -- which is depth, and only depth.
            CHECK(centre.r > centre.g + 60);
            CHECK(centre.r > centre.b + 60);

            // ...and the far cube DID draw, outside the near one's silhouette.
            CHECK(onlyFar.g > onlyFar.r + 60);
            CHECK(onlyFar.g > onlyFar.b + 60);

            // ...and neither of them reached the corner.
            CHECK(corner.r < 96);
            CHECK(corner.g < 96);
        }

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("mesh: a lit cube covers the centre pixel and the corners stay background (d3d12)",
          "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: a lit cube covers the centre pixel and the corners stay background (vulkan)",
          "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckMeshCubeCoversTheCentre(Arcane::GraphicsBackend::Vulkan);
}

TEST_CASE("mesh: the nearer cube occludes the farther one whichever order they are submitted in "
          "(d3d12)", "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckNearMeshOccludesFar(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: the nearer cube occludes the farther one whichever order they are submitted in "
          "(vulkan)", "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckNearMeshOccludesFar(Arcane::GraphicsBackend::Vulkan);
}

// ---------------------------------------------------------------------------
// 9. THE NORMAL MATRIX (NRI Phase 4 Task 8, F2a) -- a non-uniformly-scaled
//    instance's normals must go through the INVERSE TRANSPOSE, not the upper
//    3x3 mesh.hlsl's vs_main applied before this task. MeshNodeTest.cpp pins
//    NormalMatrixFor's analytic answer device-lessly; this is the one place that
//    can prove the SHADER HALF actually consumes it.
//
//    A plain cube cannot exercise this at all: every one of its face normals
//    is AXIS-ALIGNED in local space, and an axis-aligned vector's DIRECTION
//    survives a diagonal scale identically whether or not you invert it first
//    (scaling a vector along its own single nonzero axis never turns it, only
//    stretches it) -- the naive upper-3x3 and the correct inverse transpose
//    agree on every cube face under a purely diagonal `model`. So this case
//    hand-builds a single flat quad with a deliberately OBLIQUE authored
//    normal (not aligned with the quad's own geometric plane) -- physically
//    unusual, but a MeshVertex's `normal` is just an attribute the shader
//    transforms and shades with; nothing checks it against the triangle's
//    winding, and isolating the shader's NORMAL-MATRIX MATH from its geometry
//    is exactly the point.
//
//    THE COMPARISON IS STRUCTURAL, per the file header: an UNSCALED reference
//    cube's front face (axis-aligned normal (0,0,1), so its shading is
//    correct under EITHER formula) lit by light=(0,0,1) gives a KNOWN-GOOD
//    N.L=1 rendering, independent of anything this task changed. The oblique
//    quad, scaled 8x non-uniformly along X, is lit by light = the CPU's own
//    NormalMatrixFor(model) applied to its authored normal -- i.e. the
//    direction the shader SHOULD compute if it is correct. If it is, N.L=1
//    there too and the two centre pixels should closely match. If the shader
//    had regressed to the upper-3x3 (this task's exact defect), the ACTUAL
//    shader normal would be a very different direction (dot product with the
//    intended one ~=0.25 at this scale ratio -- worked out below), so N.L
//    would drop to a fraction of 1 and the two centre pixels would visibly
//    diverge.
// ---------------------------------------------------------------------------
namespace
{
    // A flat quad in the Z=0 plane, 2m per side, CCW as seen from +Z (front-
    // facing toward this file's camera, which sits on +Z looking down -Z --
    // MeshBuilder.hpp's WINDING contract, worked out by hand for these four
    // vertices rather than inherited from a generator, because no generator
    // in MeshBuilder.hpp authors a normal independent of its geometry).
    //
    // THE NORMAL IS DELIBERATELY OBLIQUE: normalize(1,0,1), not the quad's
    // true geometric normal (0,0,1). See the section comment above for why an
    // axis-aligned normal (every cube face; this quad's own true geometric
    // one) cannot exercise this defect under a diagonal `model` at all.
    Arcane::MeshData BuildObliqueNormalQuad()
    {
        const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
        Arcane::MeshData quad;
        quad.vertices = {
            { glm::vec3(-1.0f, -1.0f, 0.0f), n, glm::vec2(0.0f, 0.0f) },
            { glm::vec3( 1.0f, -1.0f, 0.0f), n, glm::vec2(1.0f, 0.0f) },
            { glm::vec3( 1.0f,  1.0f, 0.0f), n, glm::vec2(1.0f, 1.0f) },
            { glm::vec3(-1.0f,  1.0f, 0.0f), n, glm::vec2(0.0f, 1.0f) },
        };
        quad.indices = { 0, 1, 2, 0, 2, 3 };
        return quad;
    }

    void CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t before = Arcane::RenderErrorCount();

        // ---- THE KNOWN-GOOD REFERENCE: an unscaled cube, axis-aligned
        //      normal, light dead-on. Correct under EITHER normal-transform
        //      formula, so it needs nothing from this task to be
        //      trustworthy. ----
        const Arcane::MeshData referenceCube = Arcane::BuildCube(2.0f);
        Arcane::MeshInstance referenceInstance;
        referenceInstance.mesh      = &referenceCube;
        referenceInstance.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);   // white: pure N.L*light+ambient
        const Arcane::MeshInstance referenceInstances[] = { referenceInstance };

        Arcane::MeshSceneDesc referenceScene;
        referenceScene.instances = referenceInstances;
        FillCamera(referenceScene);   // lightDirection defaults to (0,0,1) -- exactly this face's normal

        std::uint32_t rw = 0, rh = 0;
        const std::vector<unsigned char> referencePixels = CaptureMesh(backend, referenceScene, rw, rh);
        const Rgba referenceCentre = At(referencePixels, rw, rw / 2u, rh / 2u);

        // ---- THE OBLIQUE, NON-UNIFORMLY-SCALED CASE. `model` scales X by 8x
        //      and leaves Y/Z alone -- extreme on purpose, so a regression to
        //      the upper-3x3 misses by a WIDE margin rather than a subtle
        //      one (worked out below, not just asserted). ----
        const Arcane::MeshData obliqueQuad = BuildObliqueNormalQuad();
        const glm::vec3 localNormal = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f));
        const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(8.0f, 1.0f, 1.0f));

        // THE LIGHT IS SET FROM THE SAME FUNCTION THE SHADER MUST MATCH. This
        // is deliberate, not circular: NormalMatrixFor is pinned analytically,
        // device-lessly, against hand-worked numbers in MeshNodeTest.cpp. What
        // THIS case adds is proof the shader's OWN computation (mesh.hlsl's
        // vs_main, run on a real GPU) agrees with the CPU's -- which no
        // device-less test can show.
        //
        //   correct = inverseTranspose(diag(8,1,1)) * normalize(1,0,1)
        //           = diag(1/8,1,1) * (0.7071, 0, 0.7071) = (0.0884, 0, 0.7071)
        //           normalized ~= (0.124, 0, 0.992)
        //   naive (upper 3x3) = diag(8,1,1) * (0.7071, 0, 0.7071) = (5.657, 0, 0.7071)
        //           normalized ~= (0.992, 0, 0.124)
        //   dot(correct, naive) ~= 0.246 -- so a shader still using the naive
        //   formula would land N.L ~= 0.25 here, not 1.0: a wide, unmistakable
        //   miss, not a rounding-sized one.
        const glm::vec3 correctNormal = glm::normalize(Arcane::NormalMatrixFor(model) * localNormal);

        Arcane::MeshInstance obliqueInstance;
        obliqueInstance.mesh      = &obliqueQuad;
        obliqueInstance.model     = model;
        obliqueInstance.baseColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        const Arcane::MeshInstance obliqueInstances[] = { obliqueInstance };

        Arcane::MeshSceneDesc obliqueScene;
        obliqueScene.instances      = obliqueInstances;
        FillCamera(obliqueScene);
        obliqueScene.lightDirection = correctNormal;   // aim the light at the CORRECT answer

        std::uint32_t ow = 0, oh = 0;
        const std::vector<unsigned char> obliquePixels = CaptureMesh(backend, obliqueScene, ow, oh);
        const Rgba obliqueCentre = At(obliquePixels, ow, ow / 2u, oh / 2u);

        // ---- THE SAME OBLIQUE SCENE, AMBIENT ONLY -- proves the light is
        //      actually contributing something, so "matches the reference"
        //      cannot be explained away by "everything saturates to white
        //      regardless of the light". Mirrors CheckMeshCubeCoversTheCentre's
        //      own lit-vs-unlit idiom. ----
        Arcane::MeshSceneDesc obliqueUnlitScene = obliqueScene;
        obliqueUnlitScene.lightColor = glm::vec3(0.0f);
        std::uint32_t uw = 0, uh = 0;
        const std::vector<unsigned char> obliqueUnlitPixels =
            CaptureMesh(backend, obliqueUnlitScene, uw, uh);
        REQUIRE(uw == ow);
        REQUIRE(uh == oh);
        const Rgba obliqueUnlitCentre = At(obliqueUnlitPixels, uw, uw / 2u, uh / 2u);

        REQUIRE(rw == ow);
        REQUIRE(rh == oh);

        // THE PROPERTY: the oblique instance's normal-matrix-corrected
        // shading matches the known-good reference's, closely -- both are
        // N.L=1 under the SAME ambient/light-color/albedo, so a CORRECT
        // shader lands these BIT-IDENTICAL (both N.L=1 -> linear 1.08 -> the
        // same tonemapped byte), making the true delta 0 and this margin free
        // to tighten. 12 still comfortably covers CPU/GPU float noise while
        // catching failures far short of the upper-3x3 regression this case
        // was designed for (N.L~=0.25, nowhere near this margin): an
        // all-identity normal matrix reaching the shader (e.g. Finding 2's
        // singular guard misfiring, or the push constants never arriving)
        // gives N.L=cos(angle between correctNormal and local +Z)~=0.789 ->
        // linear 0.869, whose tonemapped delta from the reference sits at or
        // just above 24 depending on the curve -- a margin of 24 could miss
        // that failure; 12 does not.
        CHECK(std::abs(Luma(referenceCentre) - Luma(obliqueCentre)) < 12);

        // AND THE LIGHT DEMONSTRABLY MATTERED: the lit oblique centre is far
        // brighter than its own ambient-only twin.
        CHECK(Luma(obliqueCentre) > Luma(obliqueUnlitCentre) + 60);

        CHECK(Arcane::RenderErrorCount() == before);
    }
}

TEST_CASE("mesh: a non-uniformly-scaled instance's lit-face brightness matches the analytic "
          "Lambert term (d3d12)", "[gpu][pixel][mesh][nri][d3d12]")
{
    CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("mesh: a non-uniformly-scaled instance's lit-face brightness matches the analytic "
          "Lambert term (vulkan)", "[gpu][pixel][mesh][nri][vulkan]")
{
    CheckNonUniformScaleNormalMatchesAnalyticLambert(Arcane::GraphicsBackend::Vulkan);
}
