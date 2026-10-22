#pragma once
#include <Arcane/Core/Api.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/SystemScheduler.hpp>

#include <cstdint>
#include <span>

// The sim hook (3D-readiness spec 3.1, Plan A Task 8). Engine no longer names a
// physics module: each physics module records ONE SimHook at image load (the
// same shape as ModuleAttach, NOT SystemFactoryTable -- that table drops entries
// while no owner is open) and Runtime calls every hook in `order`. A hook owns
// its world, its gravity and its interpolation buffer; Engine passes none.
namespace Arcane
{
    struct SimHook
    {
        std::uint32_t order = 0; // ascending; RegisterComponents runs in this order, so component ids do not depend on DLL load order
        void (*Prewarm)() noexcept = nullptr;
        void (*RegisterComponents)(Astra::ComponentRegistry&) noexcept = nullptr;
        void (*InstallSystems)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
        void (*FrameBegin)(Astra::Registry&) noexcept = nullptr;
        void (*EnsureWorld)(Astra::Registry&, Mosaic::IWorkScheduler* scheduler) noexcept = nullptr;
        void (*EditPass)(Astra::Registry&, float fixedDt) noexcept = nullptr;
        void (*ResetWorld)(Astra::Registry&) noexcept = nullptr;
        void (*ReplaceFixedRate)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
    };

    // Inserts in ascending `order`; equal orders keep registration order.
    ARC_CORE_API void RegisterSimHook(SimHook hook) noexcept;
    // A snapshot view: valid until the next RegisterSimHook / ReplaceSimHooksForTest.
    ARC_CORE_API std::span<const SimHook> SimHooks() noexcept;
    // Test seam: replaces the whole list (sorted the same way).
    ARC_CORE_API void ReplaceSimHooksForTest(std::span<const SimHook> hooks) noexcept;
}
