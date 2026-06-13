#pragma once

// Platform module: SDL3 window + event pump. Quit/resize surface only;
// input lives in Arcane/Input (snapshot-driven action system).

#include <Arcane/Base/Api.hpp>

#include <cstdint>
#include <string>

struct SDL_Window;

namespace Arcane
{
    struct WindowDesc
    {
        std::string title = "Arcane";
        uint32_t width    = 1280;
        uint32_t height   = 720;
        bool resizable    = true;
        bool hidden       = false;  // tests create hidden windows
        bool vulkan       = false;  // set true for Vulkan windows (SDL_WINDOW_VULKAN)
    };

    struct WindowEvents
    {
        bool quitRequested = false;  // SDL_EVENT_QUIT or window close (ESC is the host's business via InputActions)
        bool resized       = false;  // pixel size changed since last pump
        uint32_t width     = 0;      // valid when resized
        uint32_t height    = 0;
    };

    class ARCANE_API Window
    {
    public:
        Window() = default;
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool Create(const WindowDesc& desc);
        void Destroy();

        WindowEvents PumpEvents();

        void SetTitle(const std::string& title);
        void SetSize(uint32_t width, uint32_t height);

        // Native event tap for engine-internal modules (ImGui): invoked
        // once per SDL event during PumpEvents, BEFORE Window's own
        // handling. `event` is a const SDL_Event* passed opaquely so SDL
        // types stay out of public headers. One tap; null uninstalls.
        using NativeEventTap = void (*)(const void* event, void* user);
        void SetNativeEventTap(NativeEventTap tap, void* user);

        void GetPixelSize(uint32_t& width, uint32_t& height) const;
        bool IsMinimized() const;

        void* NativeHandle() const;                       // HWND on Windows
        SDL_Window* SdlWindow() const { return m_window; }

    private:
        SDL_Window*    m_window  = nullptr;
        NativeEventTap m_tap     = nullptr;
        void*          m_tapUser = nullptr;
    };
}
