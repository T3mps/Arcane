#pragma once

// Internal: routes NVRHI's diagnostics (including the validation layer's)
// into the engine log.
//
// NRI Phase 5a, Task 8a: this class used to OWN the render-layer error latch
// and the device-removed hook slot. Both moved to Render/RenderErrorLatch.hpp
// -- a home with no nvrhi dependency -- because the graph path bumps them from
// every one of its error seams and the hosts fold the counter into their exit
// code, none of which may depend on a layer this phase deletes. What is left
// here is exactly what is nvrhi-shaped: the IMessageCallback implementation
// and the severity split. It holds no state of its own.

#include <Arcane/Render/RenderErrorLatch.hpp>

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
                RenderErrorLatch::Instance().NoteNvrhiError(messageText);
                break;
            case nvrhi::MessageSeverity::Fatal:
                RenderErrorLatch::Instance().NoteNvrhiFatal(messageText);
                break;
            }
        }
    };
}
