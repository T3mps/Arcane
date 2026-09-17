// GridNode -- see the header for what this node owns, what it deliberately is
// NOT, where it sits in the frame, and the spec s14 depth-lifetime decision.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "GridNode.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderConventions.hpp>   // kVsEntry / kPsEntry

#undef ERROR

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Arcane
{
    namespace
    {
        void GraphError(const std::string& text)
        {
            RenderErrorLatch::Instance().NoteError("nri-graph", text.c_str());
        }

        // The offline artifacts this node loads. Their stems' _vs/_ps suffixes
        // agree with grid.hlsl's vs_main/ps_main entry points -- the INVARIANT
        // compile-shaders.bat states at the top of itself.
        constexpr const char* kGridVs = "grid_vs";
        constexpr const char* kGridPs = "grid_ps";

        // The quad's MINIMUM half-extent, and how it grows with the eye's
        // altitude above the plane: UE scales its grid radii with the camera's
        // height so the grid never ends inside the view when you fly up. At
        // 2000 m the far edge is ten times the default fade distance away,
        // so it is never visible at ground level either.
        constexpr float kMinHalfExtent      = 2000.0f;
        constexpr float kHalfExtentPerMetre = 100.0f;

        // data/shaders/grid.hlsl's GridFrameCB (b1, space1), field for field.
        // std140/HLSL cbuffer packing puts every float4 on its own 16-byte
        // boundary, which the glm::vec4s below satisfy by construction.
        //
        // 64 + 16 + 16 + 4*16 = 160 bytes -- PAST Vulkan's 128-byte guaranteed
        // push-constant minimum, which is why this is a per-frame constant
        // buffer and not a root/push block (GridNode.hpp's header).
        struct GridFrameConstants
        {
            glm::mat4 viewProjection{ 1.0f };
            glm::vec4 eyeAndPlane{ 0.0f, 0.0f, 0.0f, 0.0f };   // xyz eye, w plane id
            glm::vec4 params{ 1.0f, 10.0f, 200.0f, 2000.0f };  // minor, majorEvery, fade, halfExtent
            glm::vec4 minorColor{ 0.0f };
            glm::vec4 majorColor{ 0.0f };
            glm::vec4 axisUColor{ 0.0f };
            glm::vec4 axisVColor{ 0.0f };
        };
        static_assert(sizeof(GridFrameConstants) == 160, "must match grid.hlsl's GridFrameCB");
        static_assert(sizeof(GridFrameConstants) <= GridNode::kFrameCbMaxBytes,
                      "the frame constants must fit one arena region");

        // Every element finite -- MeshNode's own guard, for the same reason:
        // a NaN clip position is undefined on the GPU, not a wrong picture.
        [[nodiscard]] bool IsFinite(const glm::mat4& m) noexcept
        {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    if (!std::isfinite(m[c][r]))
                        return false;
            return true;
        }
    }

    std::unique_ptr<GridNode> GridNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<GridNode> node(new GridNode());
        if (!node->Init(context))
            return nullptr;
        return node;
    }

    bool GridNode::Init(NriGraphContext& context)
    {
        m_device    = &context.Device();
        m_pipelines = &context.Pipelines();

        m_vs = context.ShaderBytecode(kGridVs);
        m_ps = context.ShaderBytecode(kGridPs);
        if (m_vs.empty() || m_ps.empty())
        {
            ARC_ERROR("[nri-graph] GridNode: shader bin '{}'/'{}' is missing -- the 3D grid "
                      "cannot be built", kGridVs, kGridPs);
            return false;
        }

        return CreateBindings() && CreateConstantArena() && CreateSets();
    }

    nri::DescriptorPoolDesc GridNode::PoolSizes() noexcept
    {
        // ONE dimension: kSwapchainFramesInFlight per-frame sets, each
        // carrying exactly ONE CONSTANT_BUFFER descriptor (b1). No textures,
        // no samplers, no update-after-set.
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum  = kSwapchainFramesInFlight;
        poolDesc.constantBufferMaxNum = kSwapchainFramesInFlight;
        return poolDesc;
    }

    bool GridNode::CreateBindings()
    {
        const nri::CoreInterface& core = m_device->Core();

        // THE LAYOUT: one ordinary descriptor set at space1 = { b1, the
        // per-frame constant buffer }, both stages (the VS reads the
        // view-projection, the eye and the extent; the PS reads the eye, the
        // spacings and the colours). NO root constants, NO root sampler, NO
        // other set -- so THE REGISTER-SPACE RULE (Batch2DNode.hpp: NRI
        // refuses rootRegisterSpace == a set's registerSpace only once the
        // layout carries a root DESCRIPTOR or root SAMPLER) has nothing to
        // bite on here. space1 is kept anyway so grid.hlsl's `b1, space1`
        // reads exactly like mesh.hlsl's and rides the `-fvk-b-shift 256 1`
        // entry compile-shaders.bat / ShaderConventions.hpp already carry.
        nri::DescriptorRangeDesc frameRange = {};
        frameRange.baseRegisterIndex = 1;                     // b1
        frameRange.descriptorNum     = 1;
        frameRange.descriptorType    = nri::DescriptorType::CONSTANT_BUFFER;
        frameRange.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        nri::DescriptorSetDesc frameSetDesc = {};
        frameSetDesc.registerSpace = 1;
        frameSetDesc.ranges        = &frameRange;
        frameSetDesc.rangeNum      = 1;

        // Value-initialized then assigned field by field: NriPipelineCache's
        // DEDUP CONTRACT (the desc is compared byte-wise, so padding must be
        // zeroed).
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 0;
        layoutDesc.descriptorSets    = &frameSetDesc;
        layoutDesc.descriptorSetNum  = 1;
        layoutDesc.shaderStages      = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;

        m_layoutId = m_pipelines->RegisterLayout(layoutDesc);
        if (!m_pipelines->Layout(m_layoutId))
        {
            ARC_ERROR("[nri-graph] GridNode: pipeline layout registration failed");
            return false;
        }

        // The pool. Every set is written once, at Create, and never again --
        // which keeps ResetDescriptorPool and its fence discipline out of this
        // file, exactly as in MeshNode.
        const nri::DescriptorPoolDesc poolDesc = PoolSizes();
        if (!ARC_NRI_CHECK(core.CreateDescriptorPool(m_device->Device(), poolDesc, m_pool)) || !m_pool)
        {
            ARC_ERROR("[nri-graph] GridNode: descriptor pool creation failed");
            return false;
        }
        return true;
    }

    bool GridNode::CreateConstantArena()
    {
        const nri::CoreInterface& core = m_device->Core();
        const nri::DeviceDesc& deviceDesc = core.GetDeviceDesc(m_device->Device());

        // MeshNode::CreateConstantArena, shape for shape: the device's
        // constant-buffer offset alignment fixes both the region stride and
        // the SIZE every CB view is created with (D3D12 requires a multiple
        // of 256 for D3D12_CONSTANT_BUFFER_VIEW_DESC::SizeInBytes).
        m_arenaStride = CbRegionStride(deviceDesc.memoryAlignment.constantBufferOffset);
        const std::uint64_t arenaBytes = m_arenaStride * kSwapchainFramesInFlight;

        nri::BufferDesc bufferDesc = {};
        bufferDesc.size  = arenaBytes;
        bufferDesc.usage = nri::BufferUsageBits::CONSTANT_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::HOST_UPLOAD,
                                                       0.0f, bufferDesc, m_arena))
            || !m_arena)
        {
            ARC_ERROR("[nri-graph] GridNode: the frame constant-buffer arena ({} bytes) could not "
                      "be created", arenaBytes);
            return false;
        }
        core.SetDebugName(m_arena, "nri-graph grid frame CBs");

        // Persistent map, unmapped once in Release()/~GridNode. The NONE
        // backend's MapBuffer returns null, so this node is [gpu]-only from
        // here down -- same as MeshNode.
        m_arenaCpu = core.MapBuffer(*m_arena, 0, nri::WHOLE_SIZE);
        if (!m_arenaCpu)
        {
            ARC_ERROR("[nri-graph] GridNode: the frame constant-buffer arena could not be mapped "
                      "(the NONE backend cannot -- this node is a [gpu] path)");
            return false;
        }

        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            nri::BufferViewDesc viewDesc = {};
            viewDesc.buffer = m_arena;
            viewDesc.type   = nri::BufferView::CONSTANT_BUFFER;
            viewDesc.offset = ArenaOffset(slot);
            viewDesc.size   = m_arenaStride;
            if (!ARC_NRI_CHECK(core.CreateBufferView(viewDesc, m_frameCbView[slot]))
                || !m_frameCbView[slot])
            {
                ARC_ERROR("[nri-graph] GridNode: the frame constant-buffer view for frame slot {} "
                          "could not be created", slot);
                return false;
            }
        }
        return true;
    }

    bool GridNode::CreateSets()
    {
        const nri::CoreInterface& core = m_device->Core();
        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            ARC_ERROR("[nri-graph] GridNode: no layout to allocate descriptor sets from");
            return false;
        }

        for (std::uint32_t slot = 0; slot < kSwapchainFramesInFlight; ++slot)
        {
            // setIndex 0: the ARRAY position of frameSetDesc in CreateBindings'
            // layout -- the only set there is.
            if (!ARC_NRI_CHECK(core.AllocateDescriptorSets(*m_pool, *layout, 0,
                                                           &m_sets[slot], 1, 0))
                || !m_sets[slot])
            {
                ARC_ERROR("[nri-graph] GridNode: descriptor-set allocation failed for frame slot "
                          "{} -- the pool holds {} sets (PoolSizes)", slot,
                          PoolSizes().descriptorSetMaxNum);
                return false;
            }

            // `cb`'s ADDRESS must outlive the UpdateDescriptorRanges call:
            // UpdateDescriptorRangeDesc::descriptors is a pointer to an array.
            const nri::Descriptor* cb = m_frameCbView[slot];

            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = m_sets[slot];
            update.rangeIndex    = 0;   // b1 -- the only range in this set
            update.descriptors   = &cb;
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }
        return true;
    }

    GridNode::~GridNode()
    {
        if (m_device && (m_pool || m_arena))
        {
            ARC_WARN("[nri-graph] GridNode destroyed with live NRI objects -- either Create() failed "
                     "part way (an ERROR above says which step) or its owner never called Release(). "
                     "Destroying directly behind a DeviceWaitIdle.");
            const nri::CoreInterface& core = m_device->Core();
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
            if (m_pool) core.DestroyDescriptorPool(m_pool);
            for (nri::Descriptor*& view : m_frameCbView)
                if (view) { core.DestroyDescriptor(view); view = nullptr; }
            if (m_arena)
            {
                if (m_arenaCpu) core.UnmapBuffer(*m_arena);
                core.DestroyBuffer(m_arena);
            }
            for (nri::DescriptorSet*& set : m_sets)
                set = nullptr;
            m_pool = nullptr;
            m_arena = nullptr; m_arenaCpu = nullptr;
        }
    }

    void GridNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // Descriptors before the resources they view -- the graveyard runs
        // burials in order, so a view never outlives its arena.
        if (m_pool)
        {
            graveyard.Bury(fence, [core, p = m_pool] { core->DestroyDescriptorPool(p); });
            m_pool = nullptr;
        }
        for (nri::DescriptorSet*& set : m_sets)
            set = nullptr;   // owned by the pool buried above

        for (nri::Descriptor*& view : m_frameCbView)
        {
            if (!view)
                continue;
            graveyard.Bury(fence, [core, d = view] { core->DestroyDescriptor(d); });
            view = nullptr;
        }
        if (m_arena)
        {
            // Unmapped HERE rather than in the burial -- the map is a CPU-side
            // fact with no GPU lifetime (MeshNode::Release's reasoning).
            if (m_arenaCpu)
            {
                core->UnmapBuffer(*m_arena);
                m_arenaCpu = nullptr;
            }
            graveyard.Bury(fence, [core, b = m_arena] { core->DestroyBuffer(b); });
            m_arena = nullptr;
        }

        m_pipeline = nullptr;   // owned by the shared cache; the vehicle clears it
    }

    nri::Pipeline* GridNode::PipelineFor(nri::Format canvasFormat, bool hasDepth)
    {
        NriPipelineCache::GraphicsKey key = {};
        key.shaderPairId    = kShaderPairId;
        key.layoutId        = m_layoutId;
        key.colorFormats[0] = canvasFormat;
        key.colorCount      = 1;
        // TWO PSOs, keyed apart by the depth format: a frame with a mesh pass
        // attaches its D32 depth and the grid tests against it; a frame with
        // none attaches nothing (UNKNOWN), and NRI bakes both facts into the
        // pipeline.
        key.depthFormat     = hasDepth ? kGraphDepthFormat : nri::Format::UNKNOWN;
        key.topology        = nri::Topology::TRIANGLE_LIST;
        // STRAIGHT ALPHA: the shader writes (rgb, coverage * fades) and the
        // canvas keeps the mesh pass's colour underneath -- the grid is a
        // translucent overlay, which is why it does not write depth either.
        key.blend           = NriPipelineCache::GraphicsKey::Blend::AlphaOver;

        // `stages` lives in THIS frame, which encloses the GetGraphics call
        // (fill contract rule 2); the bytecode is the vehicle's.
        nri::ShaderDesc stages[2] = {};
        stages[0].stage          = nri::StageBits::VERTEX_SHADER;
        stages[0].bytecode       = m_vs.data();
        stages[0].size           = m_vs.size();
        stages[0].entryPointName = kVsEntry;
        stages[1].stage          = nri::StageBits::FRAGMENT_SHADER;
        stages[1].bytecode       = m_ps.data();
        stages[1].size           = m_ps.size();
        stages[1].entryPointName = kPsEntry;

        return m_pipelines->GetGraphics(key, [&, hasDepth](nri::GraphicsPipelineDesc& desc)
        {
            // NO vertex input: grid.hlsl's vs_main builds the quad from
            // SV_VertexID. A null vertexInput is NRI's "no vertex streams".
            desc.vertexInput = nullptr;
            desc.shaders     = stages;
            desc.shaderNum   = 2;

            // CULL NOTHING: the grid is seen from both sides of its plane
            // (orbit below the XZ ground, or behind the XY plane), and a
            // single quad has one winding.
            desc.rasterization.fillMode              = nri::FillMode::SOLID;
            desc.rasterization.cullMode              = nri::CullMode::NONE;
            desc.rasterization.frontCounterClockwise = true;

            // THE DEPTH TEST, and NOT a depth write (spec s5.2). FORWARD-Z
            // [0,1] (SceneCamera.hpp's DEPTH CONVENTION): LESS_OR_EQUAL so a
            // grid fragment exactly coplanar with a mesh surface on the plane
            // still draws (a cube resting on y = 0 shares its bottom face
            // with the grid; that face is back-facing from above and culled
            // by the mesh pass, but a mesh face exactly ON the plane would
            // otherwise z-fight the grid to nothing). write = false is what
            // keeps the grid out of the depth buffer: it has no volume, and a
            // later pass testing against it would be wrong. With no depth
            // attachment NRI ignores this block (depthStencilFormat UNKNOWN).
            if (hasDepth)
            {
                desc.outputMerger.depth.compareOp = nri::CompareOp::LESS_EQUAL;
                desc.outputMerger.depth.write     = false;
            }
        });
    }

    void GridNode::Prepare(nri::Format canvasFormat, bool hasDepth)
    {
        // The PSO, built HERE so a first-frame pipeline compile does not land
        // inside the recording window; re-resolved every frame because a hit
        // is a linear scan over a handful of entries and the depth-attached
        // state can differ frame to frame (a scene gains or loses its last
        // mesh).
        m_pipeline = PipelineFor(canvasFormat, hasDepth);
    }

    void GridNode::Record(RenderGraphNodeContext& context, const GridSceneDesc& scene,
                          std::uint32_t frameSlot)
    {
        const nri::CoreInterface& core = context.core;

        if (!m_pipeline)
        {
            if (!m_warnedNoPipeline)
            {
                m_warnedNoPipeline = true;
                ARC_WARN("[nri-graph] GridNode: no pipeline for the canvas format -- the 3D grid "
                         "draws nothing this run (Prepare was skipped, or the cache refused it and "
                         "said why)");
            }
            return;
        }

        // THE CAMERA, checked rather than trusted (IsFinite's comment).
        if (!IsFinite(scene.view.view) || !IsFinite(scene.view.projection))
        {
            if (!m_warnedBadCamera)
            {
                m_warnedBadCamera = true;
                GraphError("GridNode: the view or projection matrix is not finite -- the 3D grid is "
                           "dropped rather than recording a draw with an undefined clip position");
            }
            return;
        }

        nri::PipelineLayout* layout = m_pipelines->Layout(m_layoutId);
        if (!layout)
        {
            GraphError("GridNode: the pipeline layout is gone -- nothing recorded");
            return;
        }

        nri::DescriptorSet* set = frameSlot < kSwapchainFramesInFlight ? m_sets[frameSlot] : nullptr;
        if (!set)
        {
            GraphError("GridNode: no descriptor set for this frame slot -- nothing recorded");
            return;
        }

        // ---------------------------------------------------------------
        // THE FRAME CONSTANTS, into THIS frame slot's arena region -- written
        // at record time, behind the frame-pacing fence Execute already
        // waited (MeshNode::Record's reasoning). HOST_UPLOAD is host-coherent:
        // a memcpy is the whole upload.
        // ---------------------------------------------------------------
        const glm::mat4 inverseView = glm::inverse(scene.view.view);
        const glm::vec3 eye         = glm::vec3(inverseView[3]);
        const bool      xy          = scene.plane == GridSceneDesc::Plane::XY;
        // The eye's height ABOVE THE PLANE: y for XZ, z for XY. The half-extent
        // grows with it so the quad's edge never enters the view from above.
        const float     altitude    = std::abs(xy ? eye.z : eye.y);
        const float     halfExtent  = std::max(kMinHalfExtent, kHalfExtentPerMetre * altitude);
        // A zero or negative spacing would divide by zero in the shader (and
        // a zero majorEvery would NaN the wrap): clamp to something sane
        // rather than trust the desc.
        const float     minor       = std::max(scene.minorSpacing, 1e-3f);
        const float     major       = std::max(scene.majorEvery,   minor);
        const float     fade        = std::max(scene.fadeDistance, 1e-3f);

        GridFrameConstants constants;
        constants.viewProjection = scene.view.projection * scene.view.view;
        constants.eyeAndPlane    = glm::vec4(eye, xy ? 1.0f : 0.0f);
        constants.params         = glm::vec4(minor, major, fade, halfExtent);
        constants.minorColor     = scene.minorColor;
        constants.majorColor     = scene.majorColor;
        constants.axisUColor     = scene.axisUColor;
        constants.axisVColor     = scene.axisVColor;
        if (auto* arena = static_cast<std::uint8_t*>(m_arenaCpu))
            std::memcpy(arena + ArenaOffset(frameSlot), &constants, sizeof(constants));

        // The viewport and scissor are the executor's (RenderGraphExec sets
        // both to the attachment extent for every Raster node), same as the
        // mesh pass relies on.
        core.CmdSetDescriptorPool(context.cmd, *m_pool);
        core.CmdSetPipelineLayout(context.cmd, nri::BindPoint::GRAPHICS, *layout);

        nri::SetDescriptorSetDesc frameSetDesc = {};
        frameSetDesc.setIndex      = 0;
        frameSetDesc.descriptorSet = set;
        core.CmdSetDescriptorSet(context.cmd, frameSetDesc);

        core.CmdSetPipeline(context.cmd, *m_pipeline);

        // Six vertices, two triangles, no vertex buffer -- SV_VertexID.
        nri::DrawDesc draw = {};
        draw.vertexNum   = 6;
        draw.instanceNum = 1;
        core.CmdDraw(context.cmd, draw);
    }

    void AddGridNode(RenderGraph& graph, NriGraphContext* context,
                     RgTexture canvas, nri::Format canvasFormat,
                     RgTexture depth, const GridSceneDesc& scene,
                     std::uint32_t width, std::uint32_t height)
    {
        // The extent is the attachments' own (the executor's viewport is the
        // attachment extent) -- carried in the signature for symmetry with
        // AddMeshNode and for a future caller that mints its own target here.
        (void)width;
        (void)height;

        const bool hasDepth = graph.IsHandleValid(depth);

        // Resolved at DECLARATION time on purpose (GridNode::Prepare).
        if (context)
        {
            if (GridNode* node = context->Grid())
                node->Prepare(canvasFormat, hasDepth);
        }

        // `scene` is COPIED into the exec fn (a ViewTransform + a few floats),
        // so nothing here borrows the caller's object past this call.
        graph.AddNode("grid", RenderGraph::NodeKind::Raster,
            [&](RenderGraphBuilder& builder)
            {
                // The canvas was minted by batch2d and already carries the 2D
                // content and the mesh pass's colour; this pass blends on top
                // -- a Write, not a Read.
                builder.Write(canvas, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&canvas, 1));
                if (hasDepth)
                {
                    // DepthWrite is the graph's ONE barrier state for a bound
                    // depth attachment (DEPTH_STENCIL_ATTACHMENT); the PSO's
                    // depth.write = false is what actually keeps this pass
                    // from writing. Declaring it here is also what extends
                    // the depth transient's lifetime through this node (the
                    // spec s14 decision, GridNode.hpp's header).
                    builder.Write(depth, RgUsage::DepthWrite);
                    graph.SetDepthAttachment(depth);
                }
            },
            [context, scene](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (GridNode* node = context->Grid())
                    node->Record(nodeContext, scene, context->FrameSlot());
            });
    }
}
