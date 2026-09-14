// NriMeshBufferCache -- see the header for what this owns, why the ring path
// for mesh geometry retires, and why these buffers are deliberately NOT graph
// resources.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include "NriMeshBufferCache.hpp"

#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/Graveyard.hpp>

#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <string>
#include <vector>

namespace Arcane
{
    std::unique_ptr<NriMeshBufferCache> NriMeshBufferCache::Create(NriDevice& device)
    {
        std::unique_ptr<NriMeshBufferCache> cache(new NriMeshBufferCache());
        cache->m_device = &device;

        if (!ARC_NRI_CHECK(nriGetInterface(device.Device(), NRI_INTERFACE(nri::HelperInterface),
                                            &cache->m_helper)))
        {
            ARC_ERROR("[nri-graph] NriMeshBufferCache: HelperInterface unavailable -- no mesh can be "
                      "made resident on the graph device");
            return nullptr;
        }
        return cache;
    }

    NriMeshBufferCache::~NriMeshBufferCache()
    {
        if (!m_device || m_entries.empty())
            return;

        bool any = false;
        for (const auto& [id, r] : m_entries)
            any = any || r.vertexBuffer || r.indexBuffer;
        if (!any)
            return;

        ARC_WARN("[nri-graph] NriMeshBufferCache destroyed with {} live NRI object set(s) -- its owner "
                 "never called Release(). Destroying directly behind a DeviceWaitIdle.",
                 m_entries.size());
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (auto& [id, r] : m_entries)
        {
            if (r.vertexBuffer) core.DestroyBuffer(r.vertexBuffer);
            if (r.indexBuffer)  core.DestroyBuffer(r.indexBuffer);
        }
        m_entries.clear();
    }

    void NriMeshBufferCache::Bury(Resident& r, Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();
        if (r.vertexBuffer)
            graveyard.Bury(fence, [core, b = r.vertexBuffer] { core->DestroyBuffer(b); });
        if (r.indexBuffer)
            graveyard.Bury(fence, [core, b = r.indexBuffer] { core->DestroyBuffer(b); });
        r.vertexBuffer = nullptr;
        r.indexBuffer  = nullptr;
        r.ready        = false;
        r.bytes        = 0;
        r.indexCount   = 0;
    }

    void NriMeshBufferCache::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        for (auto& [id, r] : m_entries)
            Bury(r, graveyard, fence);
        m_entries.clear();
        // The warn latch stays SET across a release: a run that could not
        // resolve a mesh before a resize should not re-announce it after one.
    }

    void NriMeshBufferCache::Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence)
    {
        const auto it = m_entries.find(id);
        if (it == m_entries.end())
            return;
        Bury(it->second, graveyard, fence);
        m_entries.erase(it);
    }

    std::size_t NriMeshBufferCache::ResidentCount() const noexcept
    {
        std::size_t n = 0;
        for (const auto& [id, r] : m_entries)
            if (r.ready)
                ++n;
        return n;
    }

    std::uint64_t NriMeshBufferCache::ResidentBytes() const noexcept
    {
        std::uint64_t n = 0;
        for (const auto& [id, r] : m_entries)
            if (r.ready)
                n += r.bytes;
        return n;
    }

    std::size_t NriMeshBufferCache::SectionBytes(const MeshData& mesh) noexcept
    {
        std::size_t n = mesh.sections.size() * sizeof(MeshSection);
        for (const MeshSection& s : mesh.sections)
            n += s.name.size();
        return n;
    }

    bool NriMeshBufferCache::Upload(Resident& r, const Guid& id)
    {
        const nri::CoreInterface& core = m_device->Core();
        const std::uint64_t vertexBytes = r.cpu.vertices.size() * sizeof(MeshVertex);
        const std::uint64_t indexBytes  = r.cpu.indices.size() * sizeof(std::uint32_t);

        nri::BufferDesc vbDesc = {};
        vbDesc.size  = vertexBytes;
        vbDesc.usage = nri::BufferUsageBits::VERTEX_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                       0.0f, vbDesc, r.vertexBuffer))
            || !r.vertexBuffer)
        {
            r.vertexBuffer = nullptr;
            return false;
        }
        core.SetDebugName(r.vertexBuffer, ("mesh vb " + id.ToString()).c_str());

        nri::BufferDesc ibDesc = {};
        ibDesc.size  = indexBytes;
        ibDesc.usage = nri::BufferUsageBits::INDEX_BUFFER;
        if (!ARC_NRI_CHECK(core.CreateCommittedBuffer(m_device->Device(), nri::MemoryLocation::DEVICE,
                                                       0.0f, ibDesc, r.indexBuffer))
            || !r.indexBuffer)
        {
            r.indexBuffer = nullptr;
            return false;
        }
        core.SetDebugName(r.indexBuffer, ("mesh ib " + id.ToString()).c_str());

        nri::BufferUploadDesc uploads[2] = {};
        uploads[0].data   = r.cpu.vertices.data();
        uploads[0].buffer = r.vertexBuffer;
        uploads[0].after  = { nri::AccessBits::VERTEX_BUFFER, nri::StageBits::VERTEX_SHADER };
        uploads[1].data   = r.cpu.indices.data();
        uploads[1].buffer = r.indexBuffer;
        uploads[1].after  = { nri::AccessBits::INDEX_BUFFER, nri::StageBits::INDEX_INPUT };

        if (!ARC_NRI_CHECK(m_helper.UploadData(*m_device->GraphicsQueue(), nullptr, 0, uploads, 2)))
            return false;

        r.indexCount = static_cast<std::uint32_t>(r.cpu.indices.size());
        r.bytes      = MeshResidencyBytes(static_cast<std::size_t>(vertexBytes),
                                          static_cast<std::size_t>(indexBytes),
                                          SectionBytes(r.cpu));
        r.ready      = true;
        return true;
    }

    const NriMeshBufferCache::Resident* NriMeshBufferCache::Resolve(const Guid& id,
                                                                    std::uint64_t frameCounter)
    {
        if (id.IsNil())
            return nullptr;

        if (auto it = m_entries.find(id); it != m_entries.end())
        {
            Resident& r = it->second;
            if (r.ready)
            {
                r.lastDrawnFrame = frameCounter;
                return &r;
            }
            // Cold CPU copy left after EvictToBudget: re-upload without asking
            // the supply (s7.2 -- the CPU copy is what makes re-upload cheap).
            if (!r.cpu.vertices.empty() && !r.cpu.indices.empty())
            {
                if (!Upload(r, id))
                {
                    if (!m_warnedMiss)
                    {
                        m_warnedMiss = true;
                        ARC_WARN("[nri-graph] NriMeshBufferCache: mesh {} re-upload failed -- "
                                 "the draw is skipped (further occurrences are silent)",
                                 id.ToString());
                    }
                    return nullptr;
                }
                r.lastDrawnFrame = frameCounter;
                return &r;
            }
            // Memoized failure (empty CPU, not ready).
            return nullptr;
        }

        const auto reportMiss = [&](const std::string& why)
        {
            if (m_warnedMiss)
                return;
            m_warnedMiss = true;
            ARC_WARN("[nri-graph] NriMeshBufferCache: mesh {} is not resident on the graph device -- "
                     "{}; the draw is skipped (further occurrences are silent)",
                     id.ToString(), why);
        };

        if (!m_supply)
        {
            m_entries[id];   // memoize the miss
            reportMiss("no mesh supply is installed on this vehicle");
            return nullptr;
        }

        // Inserted BEFORE the supply consult so a Failed answer is attempted
        // once rather than every frame. PendingCook erases this entry below.
        Resident& resident = m_entries[id];
        const SupplyResult supplied = m_supply(id);
        if (supplied.state == MeshResolveState::PendingCook)
        {
            m_entries.erase(id);
            return nullptr;   // quiet, retryable -- not a miss
        }
        if (supplied.state != MeshResolveState::Ready || !supplied.mesh)
        {
            reportMiss("the asset resolved to nothing, or its mesh could not be decoded");
            return nullptr;   // empty entry stays -- memoized failure
        }
        if (supplied.mesh->vertices.empty() || supplied.mesh->indices.empty())
        {
            reportMiss("the mesh has no indices or no vertices -- a zero-size nri::Buffer is an NRI error, not a mesh");
            return nullptr;
        }

        resident.cpu = *supplied.mesh;
        if (!Upload(resident, id))
        {
            reportMiss("CreateCommittedBuffer or UploadData failed");
            return nullptr;   // partial GPU objects stay so Release/Invalidate bury them
        }
        resident.lastDrawnFrame = frameCounter;
        return &resident;
    }

    void NriMeshBufferCache::EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                                           std::uint64_t fence, std::uint64_t budget)
    {
        std::vector<MeshResidencyEntry> live;
        live.reserve(m_entries.size());
        for (const auto& [id, r] : m_entries)
        {
            if (!r.ready)
                continue;
            live.push_back(MeshResidencyEntry{ id, r.bytes, r.lastDrawnFrame });
        }

        const std::vector<Guid> drop = SelectEvictions(live, budget, frameCounter);
        for (const Guid& id : drop)
        {
            const auto it = m_entries.find(id);
            if (it == m_entries.end())
                continue;
            // GPU goes; CPU stays so the next Resolve re-uploads without the
            // supply (s7.2). Combined `bytes` drop because ready becomes false.
            Bury(it->second, graveyard, fence);
        }

        if (ResidentBytes() > budget && !m_warnedOverBudget)
        {
            m_warnedOverBudget = true;
            ARC_WARN("[nri-graph] NriMeshBufferCache: protected set is {} bytes over the {}-byte "
                     "budget -- the in-flight frame is kept intact (further occurrences are silent)",
                     ResidentBytes(), budget);
        }
    }
}
