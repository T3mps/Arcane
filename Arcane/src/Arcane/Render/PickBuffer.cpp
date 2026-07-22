#include <Arcane/Render/PickBuffer.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>   // offsetof
#include <vector>

namespace Arcane
{
    namespace
    {
        // The id target is a single-channel 32-bit UINT: 0 == background, k ==
        // the k-th drawn silhouette's hit-proxy id (id = drawable index + 1).
        // Integer format so the PS writes an exact id (no float rounding) and
        // clearTextureUInt zeroes it between picks.
        constexpr nvrhi::Format kIdFormat = nvrhi::Format::R32_UINT;

        // Per-vertex data for the id pass, matching entity_id.hlsl's VSInput. The
        // attribute array order below MUST match that struct's member order (nvrhi
        // assigns Vulkan input locations by declaration order; D3D matches the
        // custom semantic name at SemanticIndex 0).
        struct IdVertex
        {
            glm::vec2 pos;      // canvas px: the rotated bounding-quad corner
            glm::vec2 local;    // shape-local coords (unrotated), canvas px
            float     radius;   // canvas px (circle/capsule)
            float     halfLen;  // canvas px (capsule)
            uint32_t  kind;     // 0=Quad 1=Circle 2=Capsule 3=Box
            uint32_t  id;       // 1-based hit-proxy id
        };
        static_assert(sizeof(IdVertex) == 32, "id vertex is the wire format");

        struct IdPushConstants
        {
            glm::vec2 invHalfViewport;   // 2.0 / (canvasW, canvasH)
            glm::vec2 pad;
        };

        // kind -> shader code + the analytic-coverage kind used by the PS.
        uint32_t KindCode(PickDrawable::Kind k)
        {
            switch (k)
            {
            case PickDrawable::Kind::Quad:    return 0u;
            case PickDrawable::Kind::Circle:  return 1u;
            case PickDrawable::Kind::Capsule: return 2u;
            case PickDrawable::Kind::Box:     return 3u;
            }
            return 0u;
        }

        // Bounding half-extents (canvas px) of a drawable's silhouette: the quad
        // the id pass rasterizes. The PS analytically discards fragments outside
        // circle/capsule shapes; Quad/Box fill the whole bound.
        glm::vec2 BoundHalfExtents(const PickDrawable& d)
        {
            switch (d.kind)
            {
            case PickDrawable::Kind::Circle:  return glm::vec2(d.radius, d.radius);
            case PickDrawable::Kind::Capsule: return glm::vec2(d.halfLen + d.radius, d.radius);
            case PickDrawable::Kind::Quad:
            case PickDrawable::Kind::Box:
            default:                          return d.halfExtents;
            }
        }

        class PickBufferImpl final : public PickBuffer
        {
        public:
            PickBufferImpl(nvrhi::IDevice* device, ShaderLibrary& shaders, uint32_t supersample)
                : m_device(device), m_shaders(shaders), m_ss(supersample) {}

            bool Init(uint32_t width, uint32_t height)
            {
                m_commandList = m_device->createCommandList();
                if (!m_commandList)
                {
                    ARC_ERROR("PickBuffer: command list creation failed");
                    return false;
                }

                // 1x1 readback staging: Pick() copies a single pixel under the
                // cursor into it, so it is size-independent and never rebuilds on
                // Resize (the target does).
                auto stagingDesc = nvrhi::TextureDesc()
                    .setWidth(1).setHeight(1)
                    .setFormat(kIdFormat)
                    .setDebugName("PickBuffer.Readback");
                m_staging = m_device->createStagingTexture(
                    stagingDesc, nvrhi::CpuAccessMode::Read);
                if (!m_staging)
                {
                    ARC_ERROR("PickBuffer: staging texture creation failed");
                    return false;
                }

                if (!BuildTarget(width, height))
                    return false;
                return BuildPipelineResources();
            }

            void RenderIdPass(Astra::Registry& registry, const PickView& view) override
            {
                if (!m_target || !m_targetFb)
                    return;

                // Rebuild the id<->entity table + geometry for this scene state.
                m_drawables.clear();
                CollectPickables(registry, view, m_drawables);
                BuildGeometry();

                m_commandList->open();
                // 0 == background; every pixel not covered by a silhouette stays 0.
                m_commandList->clearTextureUInt(m_target, nvrhi::AllSubresources, 0u);

                if (!m_indices.empty())
                {
                    if (nvrhi::IGraphicsPipeline* pipeline = GetPipeline())
                    {
                        EnsureBuffers();
                        m_commandList->writeBuffer(m_vertexBuffer, m_vertices.data(),
                                                   m_vertices.size() * sizeof(IdVertex));
                        m_commandList->writeBuffer(m_indexBuffer, m_indices.data(),
                                                   m_indices.size() * sizeof(uint32_t));

                        // Logical 1x dims: PickDrawable geometry (PickEmit) lives in
                        // canvas px derived from PickView (world*scale+offset), which
                        // carries no target-size dependency. Feeding the LOGICAL
                        // width/height here (not the supersampled target's physical
                        // size) keeps the world->NDC map identical to the ss=1 case;
                        // only the viewport below grows, so the SAME logical content
                        // rasterizes at higher density -- supersampling with zero
                        // vertex-shader change (see Step 0 in the task brief).
                        const IdPushConstants push{
                            glm::vec2(2.0f / (float)m_width, 2.0f / (float)m_height),
                            glm::vec2(0.0f) };

                        auto state = nvrhi::GraphicsState()
                            .setPipeline(pipeline)
                            .setFramebuffer(m_targetFb)
                            .addBindingSet(m_bindingSet)
                            .setIndexBuffer({ m_indexBuffer, nvrhi::Format::R32_UINT, 0 })
                            .addVertexBuffer({ m_vertexBuffer, 0, 0 });
                        // Viewport spans the PHYSICAL (supersampled) target so the
                        // rasterizer samples the logical content at ss x density.
                        state.viewport.addViewportAndScissorRect(
                            nvrhi::Viewport((float)(m_width * m_ss), (float)(m_height * m_ss)));
                        m_commandList->setGraphicsState(state);
                        m_commandList->setPushConstants(&push, sizeof(push));
                        m_commandList->drawIndexed(nvrhi::DrawArguments()
                            .setVertexCount((uint32_t)m_indices.size()));
                    }
                }

                m_commandList->close();
                m_device->executeCommandList(m_commandList);
            }

            Astra::Entity Pick(Astra::Registry& registry, const PickView& view,
                               glm::vec2 pixel) override
            {
                // Out-of-range (incl. clicks outside the viewport) -> background.
                if (!m_target || !m_staging ||
                    pixel.x < 0.0f || pixel.y < 0.0f ||
                    pixel.x >= (float)m_width || pixel.y >= (float)m_height)
                    return Astra::Entity{};

                // Render the id pass (rebuilds the id target + the id<->entity
                // table), then copy the single subsample texel under the cursor
                // out -- the center subsample of the click's 1x pixel when the
                // id target is supersampled by m_ss.
                RenderIdPass(registry, view);

                const nvrhi::TextureDesc& targetDesc = m_target->getDesc();
                const glm::ivec2 texel = PickSampleTexel(
                    pixel, m_ss, targetDesc.width, targetDesc.height);

                m_commandList->open();
                m_commandList->copyTexture(
                    m_staging, nvrhi::TextureSlice(),
                    m_target, nvrhi::TextureSlice()
                                  .setOrigin((uint32_t)texel.x, (uint32_t)texel.y)
                                  .setWidth(1).setHeight(1));
                m_commandList->close();
                m_device->executeCommandList(m_commandList);
                m_device->waitForIdle();   // one synchronous stall per click

                uint32_t id = 0;
                size_t rowPitch = 0;
                if (const auto* mapped = static_cast<const uint32_t*>(
                        m_device->mapStagingTexture(m_staging, nvrhi::TextureSlice(),
                                                    nvrhi::CpuAccessMode::Read, &rowPitch)))
                {
                    id = mapped[0];
                    m_device->unmapStagingTexture(m_staging);
                }
                m_device->runGarbageCollection();

                // id 0 -> invalid (background); id k -> the k-th drawn entity.
                return PickEntityForId(m_drawables, id);
            }

            nvrhi::ITexture* IdTarget() const override { return m_target; }

            uint32_t PassIdOf(Astra::Entity e) const override
            {
                // m_drawables IS the retained id<->entity table (rebuilt each
                // RenderIdPass/Pick, not cleared afterward -- Pick()'s
                // PickEntityForId(m_drawables, id) reads the same table in the
                // other direction). PickPassId's contract takes a plain
                // vector<Astra::Entity>, so this mirrors its k+1 loop directly
                // over m_drawables rather than copying entities into a temporary
                // vector on every call.
                if (e == Astra::Entity::Invalid())
                    return 0u;
                for (size_t k = 0; k < m_drawables.size(); ++k)
                    if (m_drawables[k].entity == e)
                        return static_cast<uint32_t>(k + 1);
                return 0u;
            }

            void Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0 ||
                    (width == m_width && height == m_height))
                    return;

                // The caller owns frame pacing; tearing down a target the GPU may
                // still read requires an idle (OffscreenCanvas::Resize does the
                // same).
                m_device->waitForIdle();
                m_targetFb = nullptr;
                m_target   = nullptr;
                m_device->runGarbageCollection();
                BuildTarget(width, height);
                // The id pipeline is compatible with any same-format framebuffer
                // (R32_UINT), so it survives the resize; no rebuild needed.
            }

            uint32_t Width()  const override { return m_width;  }
            uint32_t Height() const override { return m_height; }
            uint32_t Supersample() const override { return m_ss; }

        private:
            bool BuildTarget(uint32_t width, uint32_t height)
            {
                // The id target is sized PHYSICAL = m_ss * logical so the id pass
                // (below) can rasterize the same logical content at higher
                // density; m_width/m_height (below) stay LOGICAL -- everything
                // outside this function (Width()/Height(), the push-constant
                // NDC map, Pick()'s bounds check) reasons in 1x.
                const uint32_t physW = width  * m_ss;
                const uint32_t physH = height * m_ss;

                // R32_UINT render target: the id pass writes per-pixel ids here;
                // Pick() copies 1 texel out. KeepInitialState lets NVRHI auto-
                // transition between RenderTarget and CopySource.
                auto desc = nvrhi::TextureDesc()
                    .setWidth(physW).setHeight(physH)
                    .setFormat(kIdFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::RenderTarget)
                    .setKeepInitialState(true)
                    .setDebugName("PickBuffer.IdTarget");
                m_target = m_device->createTexture(desc);
                if (!m_target)
                {
                    ARC_ERROR("PickBuffer: id target creation failed ({}x{}, ss={})",
                              physW, physH, m_ss);
                    return false;
                }

                m_targetFb = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_target));
                if (!m_targetFb)
                {
                    ARC_ERROR("PickBuffer: id framebuffer creation failed");
                    return false;
                }

                m_width  = width;
                m_height = height;
                return true;
            }

            bool BuildPipelineResources()
            {
                // The id pass binds ONE resource: a push constant carrying
                // 2/viewport for the canvas->clip map (VS only).
                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Vertex)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(
                        0, sizeof(IdPushConstants)));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);
                if (!m_bindingLayout)
                {
                    ARC_ERROR("PickBuffer: binding layout creation failed");
                    return false;
                }

                nvrhi::ShaderHandle vs =
                    m_shaders.Get("entity_id_vs", nvrhi::ShaderType::Vertex);
                if (!vs)
                {
                    ARC_ERROR("PickBuffer: entity_id_vs shader missing");
                    return false;
                }

                // Attribute order MUST match entity_id.hlsl's VSInput member order
                // (Vulkan locations are assigned by declaration order).
                const nvrhi::VertexAttributeDesc attributes[] = {
                    nvrhi::VertexAttributeDesc()
                        .setName("POSITION")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(IdVertex, pos))
                        .setElementStride(sizeof(IdVertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("LOCAL")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(IdVertex, local))
                        .setElementStride(sizeof(IdVertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("SHAPEPARAM")           // (radius, halfLen)
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(IdVertex, radius))
                        .setElementStride(sizeof(IdVertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("KINDID")               // (kind, id)
                        .setFormat(nvrhi::Format::RG32_UINT)
                        .setOffset(offsetof(IdVertex, kind))
                        .setElementStride(sizeof(IdVertex)),
                };
                m_inputLayout = m_device->createInputLayout(
                    attributes, (uint32_t)std::size(attributes), vs);
                if (!m_inputLayout)
                {
                    ARC_ERROR("PickBuffer: input layout creation failed");
                    return false;
                }

                m_bindingSet = m_device->createBindingSet(
                    nvrhi::BindingSetDesc().addItem(
                        nvrhi::BindingSetItem::PushConstants(0, sizeof(IdPushConstants))),
                    m_bindingLayout);
                if (!m_bindingSet)
                {
                    ARC_ERROR("PickBuffer: binding set creation failed");
                    return false;
                }

                if (!m_shaders.Get("entity_id_ps", nvrhi::ShaderType::Pixel))
                {
                    ARC_ERROR("PickBuffer: entity_id_ps shader missing");
                    return false;
                }
                return true;
            }

            // Build the id-pass vertex + index arrays from m_drawables. One quad
            // (4 verts / 6 indices) per drawable; the k-th drawable (0-based) gets
            // id k+1. Drawables are already ordered back-to-front, so index order
            // = draw order = front-most last (the output merger, primitive-ordered,
            // makes the last-drawn silhouette win a contested pixel -- no depth).
            void BuildGeometry()
            {
                m_vertices.clear();
                m_indices.clear();

                // Bounding-quad corner sign pattern: TL, TR, BR, BL.
                static const glm::vec2 kSigns[4] = {
                    { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

                for (size_t di = 0; di < m_drawables.size(); ++di)
                {
                    const PickDrawable& d = m_drawables[di];
                    const uint32_t id   = (uint32_t)di + 1u;   // 1-based
                    const uint32_t code = KindCode(d.kind);
                    const glm::vec2 bound = BoundHalfExtents(d);

                    // Rotate the bounding quad by the drawable's angle (canvas has
                    // no rotation, only scale + offset already folded into the
                    // drawable). The PS coverage test uses the UNROTATED `local`.
                    const float c = std::cos(d.angle);
                    const float s = std::sin(d.angle);
                    const uint32_t base = (uint32_t)m_vertices.size();

                    for (int i = 0; i < 4; ++i)
                    {
                        const glm::vec2 local(kSigns[i].x * bound.x, kSigns[i].y * bound.y);
                        const glm::vec2 rot(c * local.x - s * local.y,
                                            s * local.x + c * local.y);
                        IdVertex v;
                        v.pos     = d.center + rot;
                        v.local   = local;
                        v.radius  = d.radius;
                        v.halfLen = d.halfLen;
                        v.kind    = code;
                        v.id      = id;
                        m_vertices.push_back(v);
                    }

                    const uint32_t quad[6] = { base, base + 1, base + 2,
                                               base, base + 2, base + 3 };
                    m_indices.insert(m_indices.end(), quad, quad + 6);
                }
            }

            nvrhi::IGraphicsPipeline* GetPipeline()
            {
                // Lazy rebuild on shader hot reload (one pipeline; one target
                // format). Built against the id target's framebuffer info.
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    m_pipeline = nullptr;
                    m_pipelineGeneration = m_shaders.Generation();
                }
                if (!m_pipeline)
                {
                    nvrhi::ShaderHandle vs =
                        m_shaders.Get("entity_id_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps =
                        m_shaders.Get("entity_id_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("PickBuffer: id shaders unavailable");
                        return nullptr;
                    }
                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .setInputLayout(m_inputLayout)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    // No blend: the target is R32_UINT (integer -- unblendable);
                    // front-most wins by submission order, not blending.
                    m_pipeline = m_device->createGraphicsPipeline(
                        desc, m_targetFb->getFramebufferInfo());
                }
                return m_pipeline;
            }

            void EnsureBuffers()
            {
                const size_t vBytes = m_vertices.size() * sizeof(IdVertex);
                const size_t iBytes = m_indices.size() * sizeof(uint32_t);
                if (!m_vertexBuffer || m_vertexBuffer->getDesc().byteSize < vBytes)
                {
                    m_vertexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(vBytes, 16 * 1024))
                        .setIsVertexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::VertexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("PickBuffer.VB"));
                }
                if (!m_indexBuffer || m_indexBuffer->getDesc().byteSize < iBytes)
                {
                    m_indexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(iBytes, 8 * 1024))
                        .setIsIndexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::IndexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("PickBuffer.IB"));
                }
            }

            nvrhi::IDevice*             m_device = nullptr;
            ShaderLibrary&              m_shaders;
            nvrhi::TextureHandle        m_target;       // R32_UINT id render target
            nvrhi::FramebufferHandle    m_targetFb;
            nvrhi::StagingTextureHandle m_staging;      // 1x1 readback (Task 4)
            nvrhi::CommandListHandle    m_commandList;

            // Id pipeline resources (Task 3).
            nvrhi::BindingLayoutHandle    m_bindingLayout;
            nvrhi::InputLayoutHandle      m_inputLayout;
            nvrhi::BindingSetHandle       m_bindingSet;
            nvrhi::BufferHandle           m_vertexBuffer;
            nvrhi::BufferHandle           m_indexBuffer;
            nvrhi::GraphicsPipelineHandle m_pipeline;
            uint64_t                      m_pipelineGeneration = 0;

            std::vector<PickDrawable>  m_drawables;   // the id<->entity table
            std::vector<IdVertex>      m_vertices;
            std::vector<uint32_t>      m_indices;

            uint32_t m_width  = 0;   // LOGICAL 1x size
            uint32_t m_height = 0;   // LOGICAL 1x size
            uint32_t m_ss     = 1;   // supersample factor; id target is m_ss*width x m_ss*height
        };
    }

    std::unique_ptr<PickBuffer> PickBuffer::Create(
        nvrhi::IDevice* device, ShaderLibrary& shaders,
        uint32_t width, uint32_t height, uint32_t supersample)
    {
        if (!device || width == 0 || height == 0 || supersample == 0)
        {
            ARC_ERROR("PickBuffer::Create: bad args ({}x{}, ss={})",
                      width, height, supersample);
            return nullptr;
        }

        auto pb = std::make_unique<PickBufferImpl>(device, shaders, supersample);
        if (!pb->Init(width, height))
            return nullptr;
        return pb;
    }
}
