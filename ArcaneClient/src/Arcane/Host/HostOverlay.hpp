#pragma once

#include <Arcane/Client/Api.hpp>
#include <memory>

struct ImDrawData;

namespace Arcane
{
    class Window;

    class ARC_CLIENT_API HostOverlay
    {
    public:
        virtual ~HostOverlay() = default;
        virtual void BeginFrame() = 0;
        virtual ImDrawData* RenderToDrawData() = 0;
        virtual void EndFrameDiscard() = 0;
        virtual bool WantCaptureKeyboard() const = 0;
        virtual bool WantCaptureMouse() const = 0;
    };

    struct HostOverlayHooks
    {
        std::unique_ptr<HostOverlay> (*create)(Window&) = nullptr;
        void* (*saveUiContext)() noexcept = nullptr;
        void (*restoreUiContext)(void*) noexcept = nullptr;
    };

    ARC_CLIENT_API void SetHostOverlayHooks(HostOverlayHooks hooks) noexcept;
    ARC_CLIENT_API HostOverlayHooks GetHostOverlayHooks() noexcept;
    ARC_CLIENT_API std::unique_ptr<HostOverlay> CreateNullHostOverlay();
}
