#pragma once

// Internal: routes NVRHI's diagnostics (including the validation layer's)
// into the engine log.

#include <Arcane/Base/Log.hpp>

#include <nvrhi/nvrhi.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace Arcane
{
    class NvrhiMessageCallback final : public nvrhi::IMessageCallback
    {
    public:
        // F-3b: NVRHI detects device removal on submit on BOTH backends and
        // reports it through THIS sink as an Error carrying the literal text
        // "Device Removed!" (ThirdParty/nvrhi/src/d3d12/d3d12-device.cpp:630
        // and .../vulkan/vulkan-queue.cpp:200). That makes this the earliest
        // cross-backend observation point -- it fires on submit, before the
        // next Present -- so a GPU crash backend installs a hook here rather
        // than inventing a second, backend-shaped detection path.
        //
        // Raw function pointer + last-writer-wins, mirroring the diagnostics
        // Sink slot. The hook must be cleared by whoever installed it, before
        // the object it captures dies.
        using DeviceRemovedHook = void (*)();

        static NvrhiMessageCallback& Instance()
        {
            static NvrhiMessageCallback s_instance;
            return s_instance;
        }

        uint64_t ErrorCount() const { return m_errorCount.load(); }

        // Test support ONLY: the counter is documented (Device.hpp) as
        // "since process start" and production code must never call this --
        // it exists so a test that deliberately drives an Error/Fatal
        // through this sink (proving a discipline macro or callback wiring
        // actually reaches the shared 0/0 gate latch, e.g. NriCommon's
        // ARC_NRI_CHECK) can restore the latch afterward instead of
        // permanently failing every OTHER test case's RenderErrorCount()==0
        // assertion for the rest of the process. Same idiom as
        // GpuInstrumentation's ResetGpuDeviceLost(), reused by
        // GpuCrashReportTest.cpp for the same "other cases must not leak
        // into this one" reason.
        void ResetForTest() noexcept { m_errorCount.store(0); }

        void SetDeviceRemovedHook(DeviceRemovedHook hook) noexcept
        {
            m_deviceRemovedHook.store(hook, std::memory_order_release);
        }

        // What the slot currently holds, or null. Read-only and side-effect
        // free -- it never invokes the hook. Exists because the slot is
        // last-writer-wins and now has TWO writers (the NVRHI device layer
        // and, after Phase 3, Render/Nri/NriDiagnostics): "did arming
        // actually install one, and did a second arm leave it alone" is a
        // property worth pinning, and it is unobservable without a reader.
        // (Named Current* because a member called `DeviceRemovedHook` would
        // shadow the type alias above inside this class.)
        [[nodiscard]] DeviceRemovedHook CurrentDeviceRemovedHook() const noexcept
        {
            return m_deviceRemovedHook.load(std::memory_order_acquire);
        }

        // Phase 2, Task 1: the TAGGED error seam, for every producer that is
        // NOT NVRHI (the D3D12 debug layer's InfoQueue1 callback today; the
        // frame graph's `NoteError("nri-graph", ...)` next). It gives such a
        // producer the two things it needs and withholds the one it must not
        // have:
        //
        //   - It bumps the SAME atomic RenderErrorCount() reads, so a D3D12
        //     VUID or a graph refusal fails the 0/0 gate exactly like an
        //     NVRHI error does. That is the whole point of the latch.
        //   - It logs under the producer's OWN tag. The D3D12 callback used
        //     to reach the latch by calling message() below, which printed
        //     debug-layer text as "[nvrhi] ..." -- a lie about the message's
        //     origin, and the first thing to mislead whoever reads the log.
        //   - It deliberately does NOT run NotifyIfDeviceRemoved. That hook
        //     writes a diagnostic report and a minidump; the D3D12 debug
        //     layer invokes its message callback on whatever thread tripped
        //     the error, from INSIDE a D3D12 call, so letting a "Device
        //     Removed" substring there re-enter removal handling is a
        //     re-entrancy hazard for no gain. NVRHI's own submit-time feed
        //     (see message()) stays the ONE device-removed observation
        //     point -- F-3b, and the reason the hook lives on that path.
        void NoteError(const char* tag, const char* text) noexcept
        {
            ++m_errorCount;
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
        }

        // Phase 2, Task 6: the TYPED device-lost seam. NoteError above
        // deliberately withholds the removal hook because its producer (the
        // D3D12 debug layer) reports from inside a D3D12 call on whatever
        // thread tripped the error. This one is for the opposite case: a
        // producer that holds a TYPED result -- nri::Result::DEVICE_LOST off
        // its own QueueSubmit/AcquireNextTexture/QueuePresent call, on its
        // own render thread, outside any driver callback -- where firing the
        // hook is both safe and the whole point.
        //
        // It exists because the substring path cannot serve that producer:
        // NotifyIfDeviceRemoved matches NVRHI's literal "Device Removed!"
        // text, and NRI's failure text says "DEVICE_LOST", so before this an
        // NRI-path device loss reached the latch but NEVER
        // ObserveDeviceRemoved -- i.e. no .arcdiag, no .gpudump, no host
        // shutdown latch. Same counter, same hook, one honest tag.
        void NoteDeviceLost(const char* tag, const char* text) noexcept
        {
            ++m_errorCount;
            ARC_ERROR("[{}] {}", tag ? tag : "?", text ? text : "");
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        void message(nvrhi::MessageSeverity severity, const char* messageText) override
        {
            switch (severity)
            {
            case nvrhi::MessageSeverity::Info:
                ARC_INFO("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Warning:
                ARC_WARN("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Error:
                ++m_errorCount;
                ARC_ERROR("[nvrhi] {}", messageText);
                NotifyIfDeviceRemoved(messageText);
                break;
            case nvrhi::MessageSeverity::Fatal:
                ++m_errorCount;
                ARC_CRITICAL("[nvrhi] {}", messageText);
                NotifyIfDeviceRemoved(messageText);
                break;
            }
        }

    private:
        void NotifyIfDeviceRemoved(const char* messageText)
        {
            // Substring, not equality: NVRHI's own text is exactly
            // "Device Removed!", but the validation layer wraps messages and a
            // future NVRHI could add context around it.
            if (!messageText || std::strstr(messageText, "Device Removed") == nullptr)
                return;
            if (const DeviceRemovedHook hook = m_deviceRemovedHook.load(std::memory_order_acquire))
                hook();
        }

        std::atomic<uint64_t>          m_errorCount{0};
        std::atomic<DeviceRemovedHook> m_deviceRemovedHook{nullptr};
    };
}
