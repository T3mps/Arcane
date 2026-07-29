#pragma once

// GraphGridPass: the shader-graph canvas backdrop, rendered by the engine
// rather than by ImGui line-spam. One fullscreen triangle (graph_grid.hlsl)
// into an editor-owned RGBA8 target that the caller blits under the node
// editor's content with ImDrawList::AddImage.
//
// WHY A SHADER AT ALL: the vendored node editor's own grid is a fixed 32 px
// line pair with no fade and no styling beyond one color
// (imgui_node_editor.cpp:1498-1519 -- spacing is a hardcoded local, not a
// StyleVar), and an ImGui-side replacement means hundreds of AddLine calls per
// frame at every zoom level. A quad costs one draw and gets the anti-aliasing,
// the octave crossfade and the sublinear zoom curve for free. The vendored grid
// is switched off through its public style seam (StyleColor_Grid alpha 0).
//
// THE PHASE IS STATE, not a function of the view: see UpdatePhase below. That
// is what makes a zoom scale the pattern about the point the editor zoomed
// about rather than about the region's top-left corner.
//
// IDLE COST IS ZERO: Update() compares the phase+scale it just derived, plus
// the palette, against the last rendered ones and returns the existing texture
// untouched when nothing moved, so a still canvas records no command list at
// all. The target is
// over-allocated to a quantum so ordinary panel resizes do not churn GPU
// memory (see kAllocQuantum) -- a real reallocation costs a waitForIdle, the
// same trade OffscreenCanvas::Resize makes (OffscreenCanvas.cpp:131-147).
//
// NVRHI boundary: no ICommandList wrapper, no manual barriers. The target is
// created with setKeepInitialState(true) so NVRHI auto-transitions
// RenderTarget <-> ShaderResource, exactly as the node-preview thumbnails do
// (ShaderEditorDocument.cpp:3477-3487).

#include <Arcane/Render/ShaderLibrary.hpp>

#include <nvrhi/nvrhi.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Arcane::Editor
{
    // The canvas transform, as the node editor reports it. This is RAW VIEW
    // STATE -- the grid's own phase is derived from the HISTORY of these, not
    // from any single one (see UpdatePhase).
    //
    // SCALE IS THE VISUAL SCALE (1 = 100%, 2 = zoomed in 2x). Note that
    // ed::GetCurrentZoom() returns the RECIPROCAL of this -- it hands back
    // CanvasView::InvScale, i.e. canvas units per screen pixel
    // (imgui_node_editor_api.cpp:665-668, imgui_canvas.h:70). Callers must
    // invert it before filling this in.
    //
    // originX/originY are the CANVAS-space coordinate under the region's
    // top-left corner (ed::ScreenToCanvas of that corner), unscaled.
    struct GraphGridView
    {
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        float originX = 0.0f;
        float originY = 0.0f;
        float scale   = 1.0f;
    };

    // Display-referred RGBA (ImGui draws post-tonemap, imgui.hlsl:1-5). The
    // minor/major alphas are the peak strength of each octave, not opacity of
    // the image -- the backdrop is always written opaque.
    struct GraphGridColors
    {
        float canvas[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float minor[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };
        float major[4]  = { 0.0f, 0.0f, 0.0f, 1.0f };

        bool operator==(const GraphGridColors& o) const noexcept
        {
            return std::memcmp(this, &o, sizeof(GraphGridColors)) == 0;
        }
    };

    class GraphGridPass
    {
    public:
        static std::unique_ptr<GraphGridPass> Create(nvrhi::IDevice* device,
                                                     Arcane::ShaderLibrary* shaders)
        {
            if (!device || !shaders)
                return nullptr;
            std::unique_ptr<GraphGridPass> pass(new GraphGridPass(device, shaders));
            if (!pass->Init())
                return nullptr;
            return pass;
        }

        // Renders only when something actually changed. Returns the backdrop
        // texture (null while the shader artifacts are missing -- the caller
        // simply draws no backdrop and the canvas falls back to the panel's
        // own background).
        nvrhi::ITexture* Update(const GraphGridView& view, const GraphGridColors& colors)
        {
            if (view.width == 0 || view.height == 0)
                return nullptr;
            if (!EnsureTarget(view.width, view.height))
                return nullptr;

            // Phase first: it is the thing the render actually depends on, and
            // it is a function of the view HISTORY, not of `view` alone.
            UpdatePhase(view);

            const float viewW = static_cast<float>(view.width);
            const float viewH = static_cast<float>(view.height);

            const std::uint64_t generation = m_shaders->Generation();
            const RenderKey key{ m_phaseX, m_phaseY, view.scale, viewW, viewH };
            if (m_rendered && m_lastKey == key && m_lastColors == colors &&
                m_lastGeneration == generation)
                return m_tex.Get();   // idle: nothing recorded

            nvrhi::IGraphicsPipeline* pipeline = GetPipeline(generation);
            if (!pipeline)
                return nullptr;

            // Mirror of `cbuffer GraphGridCB` in graph_grid.hlsl.
            GridCB cb{};
            cb.phaseX = m_phaseX;
            cb.phaseY = m_phaseY;
            cb.zoom   = view.scale;
            cb.pad0   = 0.0f;
            cb.viewW  = viewW;
            cb.viewH  = viewH;
            cb.pad1   = 0.0f;
            cb.pad2   = 0.0f;
            std::memcpy(cb.canvasColor, colors.canvas, sizeof(cb.canvasColor));
            std::memcpy(cb.minorColor,  colors.minor,  sizeof(cb.minorColor));
            std::memcpy(cb.majorColor,  colors.major,  sizeof(cb.majorColor));

            m_cl->open();
            m_cl->writeBuffer(m_cb, &cb, sizeof(cb));
            const nvrhi::FramebufferInfoEx& fbInfo = m_fb->getFramebufferInfo();
            auto state = nvrhi::GraphicsState()
                .setPipeline(pipeline)
                .setFramebuffer(m_fb)
                .addBindingSet(m_bindingSet);
            state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
            m_cl->setGraphicsState(state);
            m_cl->draw(nvrhi::DrawArguments().setVertexCount(3));
            m_cl->close();
            m_device->executeCommandList(m_cl);

            m_lastKey        = key;
            m_lastColors     = colors;
            m_lastGeneration = generation;
            m_rendered       = true;
            return m_tex.Get();
        }

        // The allocated target size -- the caller needs it for the blit UVs,
        // because the target is over-allocated (see kAllocQuantum) and only its
        // top-left width x height corner belongs to the canvas region.
        std::uint32_t AllocatedWidth()  const noexcept { return m_allocW; }
        std::uint32_t AllocatedHeight() const noexcept { return m_allocH; }

    private:
        GraphGridPass(nvrhi::IDevice* device, Arcane::ShaderLibrary* shaders)
            : m_device(device), m_shaders(shaders)
        {
        }

        struct GridCB
        {
            float phaseX, phaseY, zoom, pad0;
            // Visible region size (the RT is over-allocated), for the vignette.
            // pad1/pad2 mirror the shader's gPad1: HLSL will not let the next
            // float4 straddle offset 32, so the row is padded out either way.
            float viewW, viewH, pad1, pad2;
            float canvasColor[4];
            float minorColor[4];
            float majorColor[4];
        };
        static_assert(sizeof(GridCB) == 80, "GridCB must match graph_grid.hlsl");

        // What the shader's output actually depends on. Deliberately NOT the
        // raw view: two different views that evolve to the same phase render
        // the same image, and the same view arriving twice must not re-render.
        struct RenderKey
        {
            float phaseX = 0.0f, phaseY = 0.0f, scale = 0.0f;
            // The vignette does NOT follow from the phase -- it is fitted to the
            // region -- so the size belongs in the key or a pure resize would
            // serve a stale ellipse from the cache.
            float viewW = 0.0f, viewH = 0.0f;
            bool operator==(const RenderKey&) const = default;
        };

        // Targets are rounded up to this so dragging a dock splitter does not
        // reallocate (and waitForIdle) on every pixel of travel.
        static constexpr std::uint32_t kAllocQuantum = 256;

        // ---------------------------------------------------------------
        // MIRRORED CONSTANTS. These four must stay equal to graph_grid.hlsl's
        // kZoomExponent / kBaseSpacingPx / kMinorTargetPx / kMajorEvery. They
        // are duplicated here rather than passed in because the phase update
        // has to know the grid's own screen scale and its coarsest period, and
        // both are the shader's arithmetic; same mirrored-constant arrangement
        // msdf.hlsl and TextSystem.cpp use for kPxRange/kAtlasSize.
        // ---------------------------------------------------------------
        static constexpr float kZoomExponent  = 0.7f;   // see the shader's rationale block
        static constexpr float kBaseSpacingPx = 20.0f;
        static constexpr float kMinorTargetPx = 22.0f;
        static constexpr float kMajorEvery    = 8.0f;

        // A view scale change below this (relative) is treated as no change at
        // all, which routes the update through the pure-pan branch. The zoom
        // branch divides by (scaleOld - scaleNew), so this is also what keeps
        // that division away from zero.
        static constexpr float kScaleEpsilon = 1e-4f;

        // The grid's own screen scale -- the sublinear answer to view zoom.
        static float GridScale(float viewScale) noexcept
        {
            return std::pow(viewScale > 1e-4f ? viewScale : 1e-4f, kZoomExponent);
        }

        // The snapped minor period, in screen pixels. Mirror of the LOD block
        // in graph_grid.hlsl's ps_main; always lands in (kMinorTargetPx/2, kMinorTargetPx].
        static float MinorPeriod(float gridScale) noexcept
        {
            const float basePeriod = kBaseSpacingPx * gridScale;
            const float level = std::log2(kMinorTargetPx /
                                          (basePeriod > 1e-4f ? basePeriod : 1e-4f));
            return basePeriod * std::exp2(std::floor(level));
        }

        // THE FIX for corner-anchored zoom, and the reason the phase is state.
        //
        // Because the screen period is pow(scale, k) with k < 1, the lattice
        // matches no fixed canvas-space grid -- so nothing about the view tells
        // us where it "should" sit, and computing the phase straight from the
        // view (as the first version did) silently pins the scale change at the
        // RT's top-left corner. Instead the phase is carried forward:
        //
        //   (a) PURE PAN. The content slides by -(dOrigin * scale); the phase is
        //       a screen-space point, so it slides by the identical amount and
        //       the grid tracks the nodes 1:1.
        //
        //   (b) ZOOM. Any step that changes the scale is, in screen space, a
        //       scale-about-a-point, and that point is derived from the two view
        //       states rather than read off the mouse -- which is what makes it
        //       equally correct for cursor zoom, keyboard zoom, and
        //       NavigateToContent/Selection (which pan AND zoom, and whose
        //       fixed point is nowhere near the cursor). The canvas point c held
        //       fixed by the step satisfies
        //           (c - Oold)*Zold == (c - Onew)*Znew
        //       =>  c = (Oold*Zold - Onew*Znew) / (Zold - Znew)
        //       and its screen position is F = (c - Oold)*Zold. The phase then
        //       scales about F by the GRID's own ratio Snew/Sold -- not the
        //       view's -- so the pattern grows out of the same point the editor
        //       zoomed about while still answering zoom sublinearly.
        void UpdatePhase(const GraphGridView& v) noexcept
        {
            const float sNew = GridScale(v.scale);
            if (!m_havePrevView)
            {
                // Seed. Any phase is legal (the lattice is virtual), so pick the
                // one with a statement attached: at 100% zoom, and only there,
                // the lines fall on canvas-space multiples of kBaseSpacingPx.
                m_phaseX = -v.originX * sNew;
                m_phaseY = -v.originY * sNew;
                m_havePrevView = true;
            }
            else if (std::fabs(v.scale - m_prevScale) <= kScaleEpsilon * m_prevScale)
            {
                m_phaseX -= (v.originX - m_prevOriginX) * v.scale;
                m_phaseY -= (v.originY - m_prevOriginY) * v.scale;
            }
            else
            {
                const float zOld = m_prevScale, zNew = v.scale;
                const float denom = zOld - zNew;
                const float cx = (m_prevOriginX * zOld - v.originX * zNew) / denom;
                const float cy = (m_prevOriginY * zOld - v.originY * zNew) / denom;
                const float fx = (cx - m_prevOriginX) * zOld;
                const float fy = (cy - m_prevOriginY) * zOld;
                const float r  = sNew / GridScale(m_prevScale);
                m_phaseX = fx + (m_phaseX - fx) * r;
                m_phaseY = fy + (m_phaseY - fy) * r;
            }
            m_prevOriginX = v.originX;
            m_prevOriginY = v.originY;
            m_prevScale   = v.scale;

            // Keep the phase bounded so a long session of panning cannot walk it
            // out to where float spacing swallows a line width. Wrapping by the
            // COARSEST lattice actually drawn (16 * pm -- see ps_main's four
            // LineCoverage calls) is invisible by construction: every drawn
            // period divides it, so all four lattices land exactly where they
            // were. Later LOD steps stay continuous regardless, because the
            // octave crossfade's subset argument does not depend on phase.
            const float wrap = MinorPeriod(sNew) * kMajorEvery * 2.0f;
            if (wrap > 0.0f)
            {
                m_phaseX = std::fmod(m_phaseX, wrap);
                m_phaseY = std::fmod(m_phaseY, wrap);
            }
        }

        bool Init()
        {
            m_bindingLayout = m_device->createBindingLayout(
                nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
            m_cb = m_device->createBuffer(nvrhi::BufferDesc()
                .setByteSize(sizeof(GridCB))
                .setIsConstantBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                .setKeepInitialState(true)
                .setDebugName("GraphGrid.CB"));
            if (!m_bindingLayout || !m_cb)
                return false;
            m_bindingSet = m_device->createBindingSet(
                nvrhi::BindingSetDesc()
                    .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_cb)),
                m_bindingLayout);
            m_cl = m_device->createCommandList();
            return m_bindingSet != nullptr && m_cl != nullptr;
        }

        bool EnsureTarget(std::uint32_t w, std::uint32_t h)
        {
            const std::uint32_t wantW = Quantize(w);
            const std::uint32_t wantH = Quantize(h);
            // Grow on demand; shrink only past a full quantum of slack, so a
            // panel oscillating around a boundary settles instead of thrashing.
            const bool fits = m_tex && m_allocW >= wantW && m_allocH >= wantH &&
                              m_allocW <= wantW + kAllocQuantum &&
                              m_allocH <= wantH + kAllocQuantum;
            if (fits)
                return true;

            if (m_tex)
            {
                // The outgoing texture may still be referenced by draw data the
                // GPU has not finished (frames in flight), and by an ImGui draw
                // list this frame has not replayed yet. Same synchronous
                // teardown OffscreenCanvas::Resize performs, and equally rare.
                m_device->waitForIdle();
                m_device->runGarbageCollection();
            }
            m_fb = nullptr;
            m_tex = nullptr;
            m_rendered = false;

            m_tex = m_device->createTexture(nvrhi::TextureDesc()
                .setWidth(wantW).setHeight(wantH)
                .setFormat(nvrhi::Format::RGBA8_UNORM)
                .setIsRenderTarget(true)
                .setInitialState(nvrhi::ResourceStates::ShaderResource)
                .setKeepInitialState(true)
                .setDebugName("GraphGrid"));
            m_fb = m_tex ? m_device->createFramebuffer(
                               nvrhi::FramebufferDesc().addColorAttachment(m_tex))
                         : nullptr;
            if (!m_tex || !m_fb)
            {
                m_fb = nullptr;
                m_tex = nullptr;
                return false;
            }
            m_allocW = wantW;
            m_allocH = wantH;
            m_pipeline = nullptr;   // framebuffer info changed
            return true;
        }

        static std::uint32_t Quantize(std::uint32_t v) noexcept
        {
            const std::uint32_t q = ((v + kAllocQuantum - 1) / kAllocQuantum) * kAllocQuantum;
            return q < kAllocQuantum ? kAllocQuantum : q;
        }

        nvrhi::IGraphicsPipeline* GetPipeline(std::uint64_t generation)
        {
            if (m_pipelineGeneration != generation)
            {
                m_pipeline = nullptr;
                m_shaderMissing = false;   // a reload may have produced them
                m_pipelineGeneration = generation;
            }
            if (m_pipeline)
                return m_pipeline;
            if (m_shaderMissing)
                return nullptr;   // latched: do not re-log every frame

            nvrhi::ShaderHandle vs =
                m_shaders->Get("graph_grid_vs", nvrhi::ShaderType::Vertex);
            nvrhi::ShaderHandle ps =
                m_shaders->Get("graph_grid_ps", nvrhi::ShaderType::Pixel);
            if (!vs || !ps)
            {
                m_shaderMissing = true;
                return nullptr;
            }
            auto desc = nvrhi::GraphicsPipelineDesc()
                .setVertexShader(vs)
                .setPixelShader(ps)
                .addBindingLayout(m_bindingLayout);
            desc.primType = nvrhi::PrimitiveType::TriangleList;
            desc.renderState.rasterState.setCullNone();
            desc.renderState.depthStencilState.disableDepthTest();
            desc.renderState.depthStencilState.disableStencil();
            m_pipeline = m_device->createGraphicsPipeline(desc,
                                                          m_fb->getFramebufferInfo());
            return m_pipeline;
        }

        nvrhi::IDevice*            m_device = nullptr;
        Arcane::ShaderLibrary*     m_shaders = nullptr;
        nvrhi::BindingLayoutHandle m_bindingLayout;
        nvrhi::BindingSetHandle    m_bindingSet;
        nvrhi::BufferHandle        m_cb;
        nvrhi::CommandListHandle   m_cl;
        nvrhi::TextureHandle       m_tex;
        nvrhi::FramebufferHandle   m_fb;
        nvrhi::GraphicsPipelineHandle m_pipeline;
        std::uint32_t   m_allocW = 0, m_allocH = 0;
        std::uint64_t   m_pipelineGeneration = 0;
        std::uint64_t   m_lastGeneration = 0;
        RenderKey       m_lastKey{};
        GraphGridColors m_lastColors{};
        // Grid phase (screen px, RT-relative) plus the view it was last carried
        // forward from. See UpdatePhase -- this is the grid's whole memory.
        float m_phaseX = 0.0f, m_phaseY = 0.0f;
        float m_prevOriginX = 0.0f, m_prevOriginY = 0.0f;
        float m_prevScale = 1.0f;
        bool m_havePrevView = false;
        bool m_rendered = false;
        bool m_shaderMissing = false;
    };
}
