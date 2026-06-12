#include <Arcane/Platform/Window.hpp>

#include <Arcane/Base/Log.hpp>

#include <SDL3/SDL.h>

namespace Arcane
{
    bool Window::Create(const WindowDesc& desc)
    {
        if (m_window)
        {
            ARC_WARN("Window::Create called on an already-created window");
            return false;
        }

        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            ARC_ERROR("SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = 0;
        if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
        if (desc.hidden)    flags |= SDL_WINDOW_HIDDEN;
        if (desc.vulkan)    flags |= SDL_WINDOW_VULKAN;

        m_window = SDL_CreateWindow(desc.title.c_str(),
                                    (int)desc.width, (int)desc.height, flags);
        if (!m_window)
        {
            ARC_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }

        ARC_INFO("Window created: '{}' {}x{}", desc.title, desc.width, desc.height);
        return true;
    }

    void Window::Destroy()
    {
        if (m_window)
        {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    Window::~Window()
    {
        Destroy();
    }

    WindowEvents Window::PumpEvents()
    {
        WindowEvents events;
        if (!m_window) return events;

        const SDL_WindowID myId = SDL_GetWindowID(m_window);
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
            case SDL_EVENT_QUIT:
                events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == myId)
                    events.quitRequested = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.windowID == myId && e.key.key == SDLK_ESCAPE)
                    events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == myId)
                {
                    events.resized = true;
                    events.width   = (uint32_t)e.window.data1;
                    events.height  = (uint32_t)e.window.data2;
                }
                break;
            default:
                break;
            }
        }
        return events;
    }

    void Window::SetTitle(const std::string& title)
    {
        if (!m_window) return;
        SDL_SetWindowTitle(m_window, title.c_str());
    }

    void Window::GetPixelSize(uint32_t& width, uint32_t& height) const
    {
        if (!m_window) { width = 0; height = 0; return; }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        width  = (uint32_t)w;
        height = (uint32_t)h;
    }

    bool Window::IsMinimized() const
    {
        if (!m_window) return false;
        return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0;
    }

    void* Window::NativeHandle() const
    {
        if (!m_window) return nullptr;
        return SDL_GetPointerProperty(SDL_GetWindowProperties(m_window),
                                      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    }
}
