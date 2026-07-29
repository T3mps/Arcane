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
// IDLE COST IS ZERO: Update() compares the incoming view+palette against the
// last rendered ones and returns the existing texture untouched when nothing
// moved, so a still canvas records no command list at all. The target is
// over-allocated to a quantum so ordinary panel resizes do not churn GPU
// memory (see kAllocQuantum) -- a real reallocation costs a waitForIdle, the
// same trade OffscreenCanvas::Resize makes (OffscreenCanvas.cpp:131-147).
//
// NVRHI boundary: no ICommandList wrapper, no manual barriers. The target is
// created with setKeepInitialState(true) so NVRHI auto-transitions
// RenderTarget <-> ShaderResource, exactly as the node-preview thumbnails do
// (ShaderEditorDocument.cpp:2737-2747).

#include <Arcane/Render/ShaderLibrary.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstring>
#include <memory>

namespace Arcane::Editor
{
    // The canvas transform, as the node editor reports it.
    //
    // ZOOM IS THE VISUAL SCALE (1 = 100%, 2 = zoomed in 2x). Note that
    // ed::GetCurrentZoom() returns the RECIPROCAL of this -- it hands back
    // CanvasView::InvScale, i.e. canvas units per screen pixel
    // (imgui_node_editor_api.cpp:665-668, imgui_canvas.h:70). Callers must
    // invert it before filling this in.
    //
    // panX/panY are the canvas-space coordinate under the region's top-left
    // corner, MULTIPLIED BY zoom -- the shader's gPhasePx. See the coordinate
    // contract at the top of graph_grid.hlsl for the derivation.
    struct GraphGridView
    {
        std::uint32_t width  = 0;
        std::uint32_t height = 0;
        float panX = 0.0f;
        float panY = 0.0f;
        float zoom = 1.0f;

        bool operator==(const GraphGridView&) const = default;
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

            const std::uint64_t generation = m_shaders->Generation();
            if (m_rendered && m_lastView == view && m_lastColors == colors &&
                m_lastGeneration == generation)
                return m_tex.Get();   // idle: nothing recorded

            nvrhi::IGraphicsPipeline* pipeline = GetPipeline(generation);
            if (!pipeline)
                return nullptr;

            // Mirror of `cbuffer GraphGridCB` in graph_grid.hlsl.
            GridCB cb{};
            cb.phaseX = view.panX;
            cb.phaseY = view.panY;
            cb.zoom   = view.zoom;
            cb.pad0   = 0.0f;
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

            m_lastView       = view;
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
            float canvasColor[4];
            float minorColor[4];
            float majorColor[4];
        };
        static_assert(sizeof(GridCB) == 64, "GridCB must match graph_grid.hlsl");

        // Targets are rounded up to this so dragging a dock splitter does not
        // reallocate (and waitForIdle) on every pixel of travel.
        static constexpr std::uint32_t kAllocQuantum = 256;

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
        GraphGridView   m_lastView{};
        GraphGridColors m_lastColors{};
        bool m_rendered = false;
        bool m_shaderMissing = false;
    };
}
