#pragma once

// Internal: routes NVRHI's diagnostics (including the validation layer's)
// into the engine log.

#include <Arcane/Base/Log.hpp>

#include <nvrhi/nvrhi.h>

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
                ARC_ERROR("[nvrhi] {}", messageText);
                break;
            case nvrhi::MessageSeverity::Fatal:
                ARC_CRITICAL("[nvrhi] {}", messageText);
                break;
            }
        }
    };
}
