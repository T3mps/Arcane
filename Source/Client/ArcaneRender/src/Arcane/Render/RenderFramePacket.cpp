#include <Arcane/Render/RenderFramePacket.hpp>

#include <cstddef>

namespace Arcane
{
    void RenderFramePacket::Clear() noexcept
    {
        spriteView.reset();
        spriteViewFromScene = false;
        meshView.reset();
        viewport = glm::uvec2(0u, 0u);
        origin.translation = glm::vec3(0.0f);

        mesh = GpuSceneFrame{};
        visible = VisibleSet{};
        meshVisible = VisibleSet{};

        batchVertices.clear();
        batchIndices.clear();
        batchSpans.clear();
        globals = GlobalParams{};
        viewProjection = glm::mat4(1.0f);

        pickDrawables.clear();
        overlay = OverlayPrimitiveList{};
        post = PostChainDesc{};
        hasPost = false;
        imgui = ImDrawSnapshot{};
        gameUi = ImDrawSnapshot{};
        selectedIds.clear();

        lightDirection = glm::vec3(0.0f);
        lightColor = glm::vec3(0.0f);
        ambient = glm::vec3(0.0f);
    }

    RenderFramePacket& PacketRing::BeginExtract()
    {
        const std::uint32_t depth = FramesInFlight();
        RenderFramePacket& slot = m_slots[static_cast<std::size_t>(m_extractCount % depth)];
        slot.Clear();
        ++m_extractCount;
        return slot;
    }

    const RenderFramePacket& PacketRing::Slot(std::uint32_t extractsAgo) const
    {
        const std::uint32_t depth = FramesInFlight();
        const std::uint64_t index = (m_extractCount - 1 - extractsAgo) % depth;
        return m_slots[static_cast<std::size_t>(index)];
    }

    RenderFramePacket& PacketRing::Latest()
    {
        const std::uint32_t depth = FramesInFlight();
        return m_slots[static_cast<std::size_t>((m_extractCount - 1) % depth)];
    }
}
