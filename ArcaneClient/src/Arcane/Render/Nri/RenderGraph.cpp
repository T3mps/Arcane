#include "RenderGraph.hpp"

#include <Arcane/Base/Assert.hpp>

#include <utility>

namespace Arcane
{
    void RenderGraph::AddNode(const char* name, NodeKind kind, Setup setup, Exec exec)
    {
        NodeDesc node;
        node.name = name != nullptr ? name : "";
        node.kind = kind;
        node.exec = std::move(exec);

        const std::size_t nodeIndex = m_nodes.size();
        m_nodes.push_back(std::move(node));

        // `setup` runs synchronously, right here -- eager setup, deferred
        // execute (see the header comment). m_currentNodeIndex is this
        // call's node for the duration, so SetColorAttachments/
        // SetDepthAttachment (reached from within `setup` via a captured
        // RenderGraph reference) target the right node.
        m_currentNodeIndex = nodeIndex;
        if (setup)
        {
            RenderGraphBuilder builder(*this, nodeIndex);
            setup(builder);
        }

        // Re-index fresh rather than keeping a reference across `setup`:
        // nothing in this task calls AddNode() reentrantly from within a
        // `setup`, but indexing fresh costs nothing and matches the
        // discipline the rest of the NRI substrate uses around callables
        // that could reallocate a vector out from under a cached reference
        // (Graveyard::ExecutePrefix).
        if (kind == NodeKind::Raster)
        {
            NodeDesc& declared = m_nodes[nodeIndex];
            const bool hasAttachment = !declared.colorAttachments.empty()
                                        || declared.depthAttachment.index != kInvalid;
            declared.hasRequiredAttachments = hasAttachment;

            // Non-fatal, recorded rather than refused -- the same
            // assert-later shape RecordAccess() uses below for a
            // never-written transient: a Raster node with no attachment is
            // still declared (NodeHasRequiredAttachments() reports the
            // miss), it just is not valid input for Task 4's Compile().
            ARC_ENSURE(hasAttachment,
                       "RenderGraph::AddNode: a Raster node was declared with no color or "
                       "depth attachment -- call SetColorAttachments/SetDepthAttachment "
                       "during Setup");
        }

        m_currentNodeIndex = kNoCurrentNode;
    }

    void RenderGraph::SetColorAttachments(std::span<const RgTexture> attachments)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetColorAttachments: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        m_nodes[m_currentNodeIndex].colorAttachments.assign(attachments.begin(), attachments.end());
    }

    void RenderGraph::SetDepthAttachment(RgTexture attachment)
    {
        ARC_ASSERT(m_currentNodeIndex < m_nodes.size(),
                   "RenderGraph::SetDepthAttachment: no node's Setup is currently running -- "
                   "call this only from within the Setup passed to AddNode()");
        m_nodes[m_currentNodeIndex].depthAttachment = attachment;
    }

    void RenderGraph::Reset()
    {
        m_nodes.clear();
        m_textures.clear();
        m_buffers.clear();
        m_accesses.clear();
        m_currentNodeIndex = kNoCurrentNode;
        ++m_generation;
    }

    const char* RenderGraph::NodeName(std::size_t nodeIndex) const
    {
        ARC_ASSERT(nodeIndex < m_nodes.size(), "RenderGraph::NodeName: index out of range");
        return m_nodes[nodeIndex].name.c_str();
    }

    bool RenderGraph::NodeHasRequiredAttachments(std::size_t nodeIndex) const
    {
        ARC_ASSERT(nodeIndex < m_nodes.size(), "RenderGraph::NodeHasRequiredAttachments: index out of range");
        return m_nodes[nodeIndex].hasRequiredAttachments;
    }

    const char* RenderGraph::NameOf(RgTexture texture) const
    {
        ARC_ASSERT(texture.index < m_textures.size(), "RenderGraph::NameOf(RgTexture): handle is invalid or stale");
        return m_textures[texture.index].name.c_str();
    }

    const char* RenderGraph::NameOf(RgBuffer buffer) const
    {
        ARC_ASSERT(buffer.index < m_buffers.size(), "RenderGraph::NameOf(RgBuffer): handle is invalid or stale");
        return m_buffers[buffer.index].name.c_str();
    }

    bool RenderGraph::IsTransient(RgTexture texture) const
    {
        ARC_ASSERT(texture.index < m_textures.size(), "RenderGraph::IsTransient(RgTexture): handle is invalid or stale");
        return m_textures[texture.index].kind == ResourceKind::Transient;
    }

    bool RenderGraph::IsTransient(RgBuffer buffer) const
    {
        ARC_ASSERT(buffer.index < m_buffers.size(), "RenderGraph::IsTransient(RgBuffer): handle is invalid or stale");
        return m_buffers[buffer.index].kind == ResourceKind::Transient;
    }

    bool RenderGraph::WasWritten(RgTexture texture) const
    {
        ARC_ASSERT(texture.index < m_textures.size(), "RenderGraph::WasWritten(RgTexture): handle is invalid or stale");
        return m_textures[texture.index].everWritten;
    }

    bool RenderGraph::WasWritten(RgBuffer buffer) const
    {
        ARC_ASSERT(buffer.index < m_buffers.size(), "RenderGraph::WasWritten(RgBuffer): handle is invalid or stale");
        return m_buffers[buffer.index].everWritten;
    }

    RgTexture RenderGraph::CreateTextureInternal(const char* name, const RgTextureDesc& desc)
    {
        TextureResource resource;
        resource.name = name != nullptr ? name : "";
        resource.desc = desc;
        resource.kind = ResourceKind::Transient;
        resource.everWritten = false; // undefined content until the first declared Write()

        const std::uint32_t index = static_cast<std::uint32_t>(m_textures.size());
        m_textures.push_back(std::move(resource));
        return RgTexture{ index };
    }

    RgTexture RenderGraph::ImportTextureInternal(const char* name, nri::Texture* texture,
                                                  nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                                  bool persistent)
    {
        TextureResource resource;
        resource.name = name != nullptr ? name : "";
        resource.kind = ResourceKind::Imported;
        resource.imported = texture;
        resource.importEntry = entry;
        resource.importExit = exit;
        resource.persistent = persistent;
        resource.everWritten = true; // see TextureResource::everWritten in the header

        const std::uint32_t index = static_cast<std::uint32_t>(m_textures.size());
        m_textures.push_back(std::move(resource));
        return RgTexture{ index };
    }

    RgBuffer RenderGraph::CreateBufferInternal(const char* name, std::uint64_t size, nri::BufferUsageBits usage)
    {
        BufferResource resource;
        resource.name = name != nullptr ? name : "";
        resource.size = size;
        resource.usage = usage;
        resource.kind = ResourceKind::Transient;
        resource.everWritten = false;

        const std::uint32_t index = static_cast<std::uint32_t>(m_buffers.size());
        m_buffers.push_back(std::move(resource));
        return RgBuffer{ index };
    }

    RgBuffer RenderGraph::ImportBufferInternal(const char* name, nri::Buffer* buffer, std::uint64_t size)
    {
        BufferResource resource;
        resource.name = name != nullptr ? name : "";
        resource.size = size;
        resource.kind = ResourceKind::Imported;
        resource.imported = buffer;
        resource.everWritten = true; // see TextureResource::everWritten in the header

        const std::uint32_t index = static_cast<std::uint32_t>(m_buffers.size());
        m_buffers.push_back(std::move(resource));
        return RgBuffer{ index };
    }

    void RenderGraph::RecordAccess(std::size_t nodeIndex, std::uint32_t resourceIndex,
                                    bool isTexture, bool isWrite, RgUsage usage)
    {
        if (isTexture)
        {
            ARC_ASSERT(resourceIndex < m_textures.size(),
                       "RenderGraph: RgTexture handle is invalid or stale (out of bounds -- "
                       "was it held across a Reset()?)");
            if (isWrite)
                m_textures[resourceIndex].everWritten = true;
        }
        else
        {
            ARC_ASSERT(resourceIndex < m_buffers.size(),
                       "RenderGraph: RgBuffer handle is invalid or stale (out of bounds -- "
                       "was it held across a Reset()?)");
            if (isWrite)
                m_buffers[resourceIndex].everWritten = true;
        }

        // A Read() of a never-written transient is accepted here -- the
        // assert-later model (plan brief, Task 3): declaration never fails
        // for this, it only records the access (this Access entry) and
        // leaves everWritten as-is (still false for that case). Task 4's
        // Compile() walks these and refuses, naming the resource.
        m_accesses.push_back(Access{ nodeIndex, resourceIndex, isTexture, isWrite, usage });
    }

    RgTexture RenderGraphBuilder::CreateTexture(const char* name, const RgTextureDesc& desc)
    {
        return m_graph.CreateTextureInternal(name, desc);
    }

    RgTexture RenderGraphBuilder::ImportTexture(const char* name, nri::Texture* texture,
                                                 nri::AccessLayoutStage entry, nri::AccessLayoutStage exit,
                                                 bool persistent)
    {
        return m_graph.ImportTextureInternal(name, texture, entry, exit, persistent);
    }

    RgBuffer RenderGraphBuilder::CreateBuffer(const char* name, std::uint64_t size, nri::BufferUsageBits usage)
    {
        return m_graph.CreateBufferInternal(name, size, usage);
    }

    RgBuffer RenderGraphBuilder::ImportBuffer(const char* name, nri::Buffer* buffer, std::uint64_t size)
    {
        return m_graph.ImportBufferInternal(name, buffer, size);
    }

    void RenderGraphBuilder::Read(RgTexture texture, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, texture.index, /*isTexture=*/true, /*isWrite=*/false, usage);
    }

    void RenderGraphBuilder::Read(RgBuffer buffer, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, buffer.index, /*isTexture=*/false, /*isWrite=*/false, usage);
    }

    void RenderGraphBuilder::Write(RgTexture texture, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, texture.index, /*isTexture=*/true, /*isWrite=*/true, usage);
    }

    void RenderGraphBuilder::Write(RgBuffer buffer, RgUsage usage)
    {
        m_graph.RecordAccess(m_nodeIndex, buffer.index, /*isTexture=*/false, /*isWrite=*/true, usage);
    }
}
