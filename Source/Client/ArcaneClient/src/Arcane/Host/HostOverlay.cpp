#include <Arcane/Host/HostOverlay.hpp>

namespace Arcane
{
    namespace
    {
        HostOverlayHooks g_hooks;
        class NullHostOverlay final : public HostOverlay
        {
        public:
            void BeginFrame() override {}
            ImDrawData* RenderToDrawData() override { return nullptr; }
            void EndFrameDiscard() override {}
            bool WantCaptureKeyboard() const override { return false; }
            bool WantCaptureMouse() const override { return false; }
        };
    }

    void SetHostOverlayHooks(HostOverlayHooks hooks) noexcept { g_hooks = hooks; }
    HostOverlayHooks GetHostOverlayHooks() noexcept { return g_hooks; }
    std::unique_ptr<HostOverlay> CreateNullHostOverlay()
    {
        return std::make_unique<NullHostOverlay>();
    }
}
