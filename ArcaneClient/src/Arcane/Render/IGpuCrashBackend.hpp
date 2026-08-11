#pragma once

// GPU crash diagnostics arc (Task 3): the seam a GPU backend implements
// (Task 5 = D3D12 via WriteBufferImmediate + DRED, Task 6 = Vulkan via the
// AMD buffer-marker / device-fault extensions). Pure interface -- no GPU
// calls live in THIS header, only shape. NEVER wrap nvrhi::ICommandList
// (NVRHI boundary rule) -- WriteMarker passes the raw pointer straight
// through to the backend, nothing more.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Arcane::Diag
{
    struct Envelope; // <Arcane/Base/DiagEnvelope.hpp> -- forward-declared to keep this header light
}

namespace Arcane
{
    class ARCANE_API IGpuCrashBackend
    {
    public:
        virtual ~IGpuCrashBackend() = default;

        // Emits a begin (true) or end (false) marker for scope `id` on
        // `commandList` (already open). False on failure (e.g. the
        // required feature/extension isn't available) -- the caller
        // decides whether that's fatal.
        virtual bool WriteMarker(nvrhi::ICommandList*, std::uint32_t id, bool begin) = 0;

        // Fills `envelope`'s fault-classification fields (and anything
        // else this backend can determine -- DRED breadcrumbs, device-fault
        // page/address, ...) from whatever GPU-side crash state it can
        // retrieve at the point a device-removed/hang is observed.
        virtual void CollectFault(Diag::Envelope&) = 0;

        // Short identifying label ("D3D12", "Vulkan") for logs and the
        // envelope's activeLayers.
        virtual const char* Name() const = 0;
    };
}
