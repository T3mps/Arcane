#pragma once

// Internal: routes NVRHI's diagnostics (including the validation layer's)
// into the engine log.

#include <Arcane/Base/Log.hpp>

#include <nvrhi/nvrhi.h>

#include <atomic>
#include <cstdint>

namespace Arcane
{
    class NvrhiMessageCallback final : public nvrhi::IMessageCallback
    {
    public:
        static NvrhiMessageCallback& Instance()
        {
            static NvrhiMessageCallback s_instance;
            return s_instance;
        }

        uint64_t ErrorCount() const { return m_errorCount.load(); }

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
                break;
            case nvrhi::MessageSeverity::Fatal:
                ++m_errorCount;
                ARC_CRITICAL("[nvrhi] {}", messageText);
                break;
            }
        }

    private:
        std::atomic<uint64_t> m_errorCount{0};
    };
}
