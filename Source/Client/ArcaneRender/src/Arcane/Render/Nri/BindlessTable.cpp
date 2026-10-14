// BindlessTable -- see the header for what this owns and why the
// descriptor-SET plumbing is deliberately NOT here (that is Task 10).
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>

#include "BindlessTable.hpp"

#include <Arcane/Render/Nri/Graveyard.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>

#include <Arcane/Base/Log.hpp>

#undef ERROR

namespace Arcane
{
    std::unique_ptr<BindlessTable> BindlessTable::Create(NriDevice& device, std::uint32_t capacity)
    {
        if (capacity == 0)
        {
            ARC_ERROR("[nri] BindlessTable: refused -- capacity must be nonzero");
            return nullptr;
        }

        std::unique_ptr<BindlessTable> table(new BindlessTable());
        table->m_device   = &device;
        table->m_capacity = capacity;
        table->m_slots.reserve(capacity);
        return table;
    }

    BindlessTable::~BindlessTable()
    {
        if (!m_device || m_slots.empty())
            return;

        ARC_WARN("[nri] BindlessTable destroyed with {} live descriptor(s) -- its owner never "
                 "called Release(). Destroying directly behind a DeviceWaitIdle.",
                 m_slots.size());
        const nri::CoreInterface& core = m_device->Core();
        (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&m_device->Device()));
        for (nri::Descriptor* d : m_slots)
            if (d) core.DestroyDescriptor(d);
        m_slots.clear();
    }

    std::uint32_t BindlessTable::Add(nri::Descriptor* srv)
    {
        // A null descriptor is a caller-code bug, not the capacity
        // condition below -- refused silently, no slot consumed. See the
        // header's own Add() doc comment.
        if (!srv)
            return kInvalidSlot;

        if (m_slots.size() >= m_capacity)
        {
            if (!m_warnedFull)
            {
                m_warnedFull = true;
                ARC_WARN("[nri] BindlessTable: capacity {} exhausted -- Add refused, returning "
                         "kInvalidSlot (further occurrences are silent)",
                         m_capacity);
            }
            return kInvalidSlot;
        }

        const std::uint32_t slot = static_cast<std::uint32_t>(m_slots.size());
        m_slots.push_back(srv);
        return slot;
    }

    void BindlessTable::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        if (!m_device)
            return;
        const nri::CoreInterface* core = &m_device->Core();

        // One burial per occupied slot -- see BindlessTable.hpp's OWNERSHIP
        // note: Add() transferred ownership of each descriptor here, so
        // Release() is what discharges it, buried rather than destroyed
        // directly (same shape as Batch2DNode::Release / NriTextureCache::
        // Release).
        for (nri::Descriptor* d : m_slots)
        {
            if (!d)
                continue;
            graveyard.Bury(fence, [core, p = d] { core->DestroyDescriptor(p); });
        }
        m_slots.clear();
    }
}
