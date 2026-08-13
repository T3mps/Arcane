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
