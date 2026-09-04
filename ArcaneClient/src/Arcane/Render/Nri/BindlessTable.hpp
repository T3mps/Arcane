#pragma once

// BindlessTable -- the descriptor array + slot allocator behind bindless SRV
// indexing (NRI Phase 4 Task 8 / F2b Task 9, executed verbatim).
//
// WHAT THIS IS ON THE NRI API: a flat table of shader-resource descriptors
// meant to back ONE descriptor set/range that SupportsBindless()-tier
// hardware indexes dynamically in the shader (no per-draw descriptor-set
// rebind). THIS FILE is the allocator + lifecycle half only: it owns slot
// bookkeeping and the buried-on-Release destruction of whatever
// nri::Descriptor* objects its caller hands it through Add(). The actual
// descriptor-SET plumbing (CreateDescriptorSet, UpdateDescriptorRanges, the
// shader-visible binding) is Task 10's MeshNode wiring, not this file -- see
// the F2b Task 9 brief.
//
// SLOT POLICY: Add-only. Slots are assigned densely from 0, in Add() call
// order, and never reused -- there is no per-slot Remove() in the Phase 4
// interface this class implements, and this file does not invent one. The
// only release is of the WHOLE table, via Release().
//
// OWNERSHIP: Add() takes ownership of the nri::Descriptor* it is handed --
// Release() is what destroys it (buried, never direct). A caller that
// destroys a descriptor itself after Adding it here will double-destroy it
// once Release() reaps. If the owner never calls Release() at all --
// an exception path, an early return, a teardown reorder -- the
// destructor is the SAFETY NET, not a second path: see ~BindlessTable()'s
// own comment. Same house practice as NriTextureCache and Batch2DNode,
// neither of which publishes its destructor as part of its interface
// either; a forgotten Release() is a real failure mode worth destroying
// loudly for, not leaking silently forever.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp
// (Extensions/NRIDeviceCreation.h declares nri::Message::ERROR, and
// <windows.h>, dragged in transitively by Arcane/Base/Log.hpp -> spdlog,
// #defines ERROR via wingdi.h).
#include <NRI.h>

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARCANE_API BindlessTable
    {
    public:
        static constexpr std::uint32_t kInvalidSlot = 0xFFFFFFFFu;

        // Borrows `device` (which must outlive this object, same contract as
        // NriTextureCache::Create) -- only for Release()'s DestroyDescriptor
        // calls; nothing here creates an NRI object of its own. Refused
        // (null, logged) if capacity == 0: a zero-capacity table can never
        // hold a single descriptor, so there is nothing sound to build.
        [[nodiscard]] static std::unique_ptr<BindlessTable> Create(NriDevice& device,
                                                                     std::uint32_t capacity);

        // SAFETY NET, NOT THE PATH -- the same shape as ~NriTextureCache /
        // ~Batch2DNode. The sanctioned release is Release() at a fence the
        // owner knows; if this still holds descriptors it destroys them
        // directly behind a DeviceWaitIdle and says so at WARN, because
        // there is no fence value to bury against here and burying at 0
        // would violate Graveyard's nondecreasing rule on a device the
        // graph has been burying against all run.
        ~BindlessTable();

        BindlessTable(const BindlessTable&)            = delete;
        BindlessTable& operator=(const BindlessTable&) = delete;

        // Appends `srv` at the next dense slot and returns it. Refuses --
        // returns kInvalidSlot, no slot consumed, no warning -- when `srv`
        // is null: a null descriptor is a caller-code bug visible at the
        // call site, not the resource-exhaustion condition this table
        // diagnoses. Otherwise returns kInvalidSlot, with a ONE-SHOT
        // warning (further occurrences silent), once the table is at
        // capacity -- Add() never grows the table or evicts an existing
        // slot.
        [[nodiscard]] std::uint32_t Add(nri::Descriptor* srv);

        // Buries every occupied slot's descriptor at `fence` via
        // Graveyard::Bury -- never destroys directly -- then empties the
        // table. Idempotent: a second call buries nothing. The caller picks
        // the fence, same reasoning as NriTextureCache::Release /
        // NriPipelineCache::Clear: only it knows which timeline this
        // table's descriptors were last read on.
        void Release(Graveyard& graveyard, std::uint64_t fence);

    private:
        BindlessTable() = default;

        NriDevice*                    m_device     = nullptr;
        std::uint32_t                 m_capacity   = 0;
        std::vector<nri::Descriptor*> m_slots;
        // THE ONE-SHOT CAPACITY WARN: one line per table, not one per
        // refused Add() -- a caller looping over a full frame's worth of
        // materials past capacity must not flood the log every draw call.
        bool                           m_warnedFull = false;
    };
}
