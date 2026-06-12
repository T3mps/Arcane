#pragma once

// Platform module: SDL3 window + event pump. M1 surfaces only what the
// host loop needs (quit, resize); the input-action layer
// (input_actions.json semantics) is a later milestone.

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
        bool vulkan       = false;  // reserved: marks window intended for Vulkan
                                    // (surface created via Win32 ext, not SDL)
    };

    struct WindowEvents
    {
        bool quitRequested = false;  // SDL_EVENT_QUIT, window close, or ESC
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
        void GetPixelSize(uint32_t& width, uint32_t& height) const;
        bool IsMinimized() const;

        void* NativeHandle() const;                       // HWND on Windows
        SDL_Window* SdlWindow() const { return m_window; }

    private:
        SDL_Window* m_window = nullptr;
    };
}
