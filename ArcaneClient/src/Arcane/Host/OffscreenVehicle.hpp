#pragma once

// OffscreenVehicle: the same device -> NRI wrap -> graph context recipe
// NriGraphPixelTest.cpp's MakeVehicle uses, promoted out of test-only code so
// any host (not just [gpu] tests) can build a device with NO WINDOW and NO
// SWAPCHAIN. This is the missing piece for --headless: CreateOffscreen()
// only BORROWS a device, and until now the only thing that created one for
// it to borrow was the pixel test's private helper.
//
// Create() runs the ordered boot (native device -> NRI wrap -> offscreen
// graph context); on any fallible step it logs ARC_ERROR and returns
// nullptr, and the partially-built unique_ptr<OffscreenVehicle> unwinds
// (RAII) in reverse member declaration order -- the same order the
// destructor would use at shutdown.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ARCANE_API OffscreenVehicle
    {
    public:
        // Null on any failure, each step logged. Ordered: native device ->
        // NRI wrap -> offscreen graph context.
        static std::unique_ptr<OffscreenVehicle> Create(const HostConfig& cfg,
                                                         std::uint32_t width,
                                                         std::uint32_t height,
                                                         const NriGraphContext::NodeSet& nodes = {});
        ~OffscreenVehicle();

        [[nodiscard]] NriGraphContext& Graph() noexcept { return *m_ctx; }
        [[nodiscard]] NriDevice&       Device() noexcept { return *m_nri; }

    private:
        OffscreenVehicle() = default;

        // TEARDOWN ORDER: ctx first (it owns objects on the device), then the
        // NRI wrap, then the native owner. Declaration order is reverse
        // destruction order, same contract GpuContext documents.
        std::unique_ptr<NativeDeviceOwner>  m_native;
        std::unique_ptr<NriDevice>          m_nri;
        std::unique_ptr<NriGraphContext>    m_ctx;
    };
}
