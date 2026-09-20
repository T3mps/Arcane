#pragma once

#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#undef ERROR

namespace Arcane
{
    class Graveyard;
    class NriDevice;
    class NriGraphContext;
    class NriPipelineCache;
    struct GpuSceneNodeInputs;

    inline constexpr bool kMeshCullEnabled = true; // runtime cvar is intentionally deferred to the cvar arc
    inline constexpr std::uint32_t kMeshCullThreads = 64;
    [[nodiscard]] constexpr std::uint32_t MeshCullDispatchGroups(std::uint32_t rowCount) noexcept
    {
        return (rowCount + kMeshCullThreads - 1u) / kMeshCullThreads;
    }

    class ARCANE_API MeshCullNode
    {
    public:
        static std::unique_ptr<MeshCullNode> Create(NriGraphContext& context);
        ~MeshCullNode() = default;
        MeshCullNode(const MeshCullNode&) = delete;
        MeshCullNode& operator=(const MeshCullNode&) = delete;

        void Record(RenderGraphNodeContext& context, class GpuScene& scene, const GpuSceneFrame* frame, std::uint32_t frameSlot);
        void Release(Graveyard& graves, std::uint64_t fence);

    private:
        MeshCullNode() = default;
        bool Init(NriGraphContext& context);
        bool UpdateSet(std::uint32_t frameSlot, class GpuScene& scene);

        NriDevice* m_device = nullptr;
        NriPipelineCache* m_pipelines = nullptr;
        std::span<const std::uint8_t> m_shader;
        std::uint32_t m_layoutId = 0xFFFFFFFFu;
        nri::DescriptorPool* m_pool = nullptr;
        nri::DescriptorSet* m_sets[kSwapchainFramesInFlight]{};
        const nri::Descriptor* m_instanceViews[kSwapchainFramesInFlight]{};
        const nri::Descriptor* m_batchViews[kSwapchainFramesInFlight]{};
        const nri::Descriptor* m_visibleViews[kSwapchainFramesInFlight]{};
        const nri::Descriptor* m_argViews[kSwapchainFramesInFlight]{};
        nri::Pipeline* m_pipeline = nullptr;
    };

    ARCANE_API void AddMeshCullNode(RenderGraph& graph, NriGraphContext* context,
                                    const GpuSceneNodeInputs& inputs, const GpuSceneFrame* frame);
}
