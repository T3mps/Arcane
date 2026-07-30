#pragma once

// A plain OS window shown at process start, BEFORE any graphics device exists,
// so something is on screen within ~100ms of the click. Torn down once the real
// window is up (see the ordering note on Close()).
//
// Windows-only; a no-op elsewhere. It must NEVER be able to fail boot -- every
// failure path degrades to "no splash" silently.

#include <Arcane/Base/Api.hpp>

#include <memory>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::unique_ptr<Impl> on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API BootSplashWindow
    {
    public:
        // Never throws. A missing or unreadable image yields a live object whose
        // IsOpen() is false.
        explicit BootSplashWindow(const char* imagePath) noexcept;
        ~BootSplashWindow();

        BootSplashWindow(const BootSplashWindow&)            = delete;
        BootSplashWindow& operator=(const BootSplashWindow&) = delete;

        // ORDERING MATTERS: call this AFTER the real window is shown, never
        // before. Closing first leaves a frame with neither window on screen,
        // which is the flicker this whole component exists to avoid. UE gets
        // this right by hiding its splash during the game's first Tick rather
        // than at the end of Init (GameEngine.cpp:1975).
        void Close() noexcept;

        [[nodiscard]] bool IsOpen() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
