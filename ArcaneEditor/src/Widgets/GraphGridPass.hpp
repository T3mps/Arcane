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
// (ShaderEditorDocument.cpp:3611-3621).
//
// =====================================================================
// THE GRAPH ARM'S GRID (NRI Phase 3, Task 11) -- DrawGraphGridFallback
// =====================================================================
// This class cannot exist on `--nri-graph`: Create() takes an nvrhi::IDevice
// and a ShaderLibrary, and the graph flavor has neither. Until this task that
// left the canvas with NO backdrop at all -- the vendored editor's own grid is
// switched off through StyleColor_Grid, so what showed was a flat panel
// background with nothing on it.
//
// THE CHOICE MADE, AND WHY. Two routes were available: render the grid through
// a tiny offscreen graph frame, or draw it with ImGui primitives. The grid is
// CHROME -- a backdrop for a node canvas, never captured, never compared
// against a golden, and its only job is to tell the eye that the canvas
// panned. An offscreen graph context per canvas (and there are two per
// document: the graph canvas and the pass canvas) is a whole RenderGraph,
// command-buffer set, descriptor pool and graveyard lane, plus a
// user-texture entry on the chrome backend and the invalidate obligations that
// come with it -- all to draw straight lines. ImGui primitives draw the same
// lines with no GPU object at all. So: PRIMITIVES.
//
// WHAT THE FALLBACK KEEPS, and it is the part that matters -- the MOTION. It
// runs the identical phase state machine (GraphGridPhase below, extracted from
// this class verbatim so there is one copy of it), so the grid tracks the
// nodes 1:1 on a pan and grows out of the editor's own zoom fixed point,
// answering zoom sublinearly through the same pow(scale, 0.7) curve. It draws
// the same two lattices at the same snapped period.
//
// WHAT IT LOSES, stated rather than discovered: the four-octave crossfade (one
// octave is drawn, so a zoom crosses LOD steps as a pop rather than a fade),
// the vignette, and the shader's analytic anti-aliasing (ImGui's 1px lines are
// its own AA). And it costs what this header's own WHY A SHADER AT ALL block
// says it costs: a few hundred AddLine calls per canvas per frame. That is the
// price of the mode, paid knowingly, in a dev-only mode.

#include "Widgets/GraphGridPhase.hpp"   // GraphGridView / GraphGridColors / GraphGridPhase

#include <Arcane/Render/ShaderLibrary.hpp>

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

namespace Arcane::Editor
{
    // The graph arm's backdrop: the same lattice, drawn into `dl` with ImGui
    // primitives. See THE GRAPH ARM'S GRID at the top of this file for what it
    // keeps, what it loses and why this is the honest answer for chrome.
    //
    // `min`/`size` are the canvas region in SCREEN coordinates; `phase` is the
    // caller's per-canvas state (one instance per canvas, exactly as one
    // GraphGridPass per canvas) and is ADVANCED by this call.
    inline void DrawGraphGridFallback(ImDrawList* dl, ImVec2 min, ImVec2 size,
                                      const GraphGridView& view,
                                      const GraphGridColors& colors,
                                      GraphGridPhase& phase)
    {
        if (!dl || size.x <= 0.0f || size.y <= 0.0f)
            return;

        // Phase first, and unconditionally: it is a function of the view
        // HISTORY, so a frame that skipped it would leave a gap the next frame
        // reads as a jump.
        phase.Update(view);

        const auto pack = [](const float (&c)[4])
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
        };
        const ImVec2 maxPt(min.x + size.x, min.y + size.y);
        // Opaque backdrop, exactly as the shader writes it ("the backdrop is
        // always written opaque", GraphGridColors above).
        dl->AddRectFilled(min, maxPt,
                          ImGui::ColorConvertFloat4ToU32(
                              ImVec4(colors.canvas[0], colors.canvas[1], colors.canvas[2], 1.0f)));

        const float pm = GraphGridPhase::MinorPeriod(GraphGridPhase::GridScale(view.scale));
        if (!(pm > 0.5f))
            return;   // degenerate; a line every half pixel is a fill, not a grid
        const float pM = pm * GraphGridPhase::kMajorEvery;

        const ImU32 minorCol = pack(colors.minor);
        const ImU32 majorCol = pack(colors.major);

        // ONE octave, at the snapped period -- see WHAT IT LOSES above. Majors
        // are drawn over minors rather than instead of them, which is what the
        // shader's additive octaves do at a major line.
        const auto lattice = [&](float period, ImU32 col)
        {
            // Start at the first lattice point at or after the region's left
            // edge. fmod can return either sign, so it is normalized into
            // [0, period) before use.
            float ox = std::fmod(phase.x, period);
            if (ox > 0.0f) ox -= period;
            for (float sx = ox; sx <= size.x; sx += period)
                if (sx >= 0.0f)
                    dl->AddLine(ImVec2(min.x + sx, min.y), ImVec2(min.x + sx, maxPt.y), col);
            float oy = std::fmod(phase.y, period);
            if (oy > 0.0f) oy -= period;
            for (float sy = oy; sy <= size.y; sy += period)
                if (sy >= 0.0f)
                    dl->AddLine(ImVec2(min.x, min.y + sy), ImVec2(maxPt.x, min.y + sy), col);
        };
        lattice(pm, minorCol);
        lattice(pM, majorCol);
    }

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
            m_phase.Update(view);

            const float viewW = static_cast<float>(view.width);
            const float viewH = static_cast<float>(view.height);

            const std::uint64_t generation = m_shaders->Generation();
            const RenderKey key{ m_phase.x, m_phase.y, view.scale, viewW, viewH };
            if (m_rendered && m_lastKey == key && m_lastColors == colors &&
                m_lastGeneration == generation)
                return m_tex.Get();   // idle: nothing recorded

            nvrhi::IGraphicsPipeline* pipeline = GetPipeline(generation);
            if (!pipeline)
                return nullptr;

            // Mirror of `cbuffer GraphGridCB` in graph_grid.hlsl.
            GridCB cb{};
            cb.phaseX = m_phase.x;
            cb.phaseY = m_phase.y;
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

        // THE MIRRORED CONSTANTS AND THE PHASE STATE MACHINE now live on
        // GraphGridPhase above -- EXTRACTED, not rewritten, so the shader
        // backdrop and the graph arm's ImGui backdrop provably move the same
        // lattice (NRI Phase 3, Task 11). Read that struct for the rationale
        // this comment used to carry.

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
        // forward from -- see GraphGridPhase, which is the grid's whole memory.
        GraphGridPhase  m_phase{};
        bool m_rendered = false;
        bool m_shaderMissing = false;
    };
}
