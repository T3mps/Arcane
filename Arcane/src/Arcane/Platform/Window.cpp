#include <Arcane/Platform/Window.hpp>

#include <Arcane/Base/Log.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <stb_image.h>   // decode the icon file; implementation lives in Assets/StbImpl.cpp (same DLL)

#include <memory>
#include <string>

namespace Arcane
{
    namespace
    {
        struct FolderCbCtx { Arcane::Window::FolderPickedCallback cb; void* user; };

        // SDL hands us the full result list; the editor wants only the first folder
        // (allow_many=false), or nullptr on cancel/error. Invoked exactly once -> we
        // own and free the heap ctx here.
        void SDLCALL FolderDialogTrampoline(void* userdata, const char* const* filelist, int /*filter*/)
        {
            std::unique_ptr<FolderCbCtx> ctx(static_cast<FolderCbCtx*>(userdata));
            const char* picked = (filelist && filelist[0]) ? filelist[0] : nullptr;
            if (ctx->cb) ctx->cb(picked, ctx->user);
        }
    }

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
            // Engine-internal modules (ImGui) see every event first, before
            // Window's own quit/resize handling.
            if (m_tap)
                m_tap(&e, m_tapUser);

            switch (e.type)
            {
            case SDL_EVENT_QUIT:
                events.quitRequested = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == myId)
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

    void Window::SetSize(uint32_t width, uint32_t height)
    {
        if (!m_window)
            return;
        SDL_SetWindowSize(m_window, (int)width, (int)height);
    }

    void Window::Show()
    {
        if (!m_window) return;
        SDL_ShowWindow(m_window);
        // Raise, not just show (2026-07-30 desk-check, Task 8d defect B): a
        // bare SDL_ShowWindow left the editor opening BEHIND the existing
        // window stack. Both callers reveal this window one statement before
        // destroying the pre-device splash, and that splash is
        // WS_EX_APPWINDOW | WS_EX_TOPMOST and holds the foreground
        // (BootSplashWindow.cpp's ShowWindow(SW_SHOW) activates it) -- so
        // without this the editor is merely un-hidden UNDER a topmost window,
        // and when the splash is then destroyed Windows hands the foreground
        // to whatever is next in the Z-order, which is some other app.
        //
        // ORDERING, and why it is this way round rather than raising after
        // the splash closes: this runs while our own process still owns the
        // foreground window. Windows' foreground lock permits a process that
        // owns the current foreground window to set it, so the raise is
        // allowed here and reliably succeeds. Do it AFTER the splash is gone
        // instead and the foreground has already been handed to another
        // process -- the very defect -- at which point SetForegroundWindow is
        // exactly what the lock exists to refuse, and Windows flashes the
        // taskbar button instead of surfacing us. Raising first also means
        // the foreground is never briefly parked on a destroyed window.
        //
        // SDL_RaiseWindow, not ::SetForegroundWindow: the portable call is
        // documented as "raise above other windows and gain the input focus",
        // it is what SDL's own multi-viewport backend uses
        // (imgui_impl_sdl3.cpp:1226), and on Windows it lands on
        // SetForegroundWindow/SetFocus anyway. Project.cpp's
        // FocusWindowOfProcess keeps the raw Win32 route because it targets
        // ANOTHER process's HWND, which SDL cannot express -- a different
        // problem, not a precedent for this one.
        SDL_RaiseWindow(m_window);
    }

    bool Window::SetIcon(const std::filesystem::path& path)
    {
        if (!m_window)
            return false;

        // Resolve relative paths against the exe dir (not the CWD) so the icon loads
        // regardless of where the editor is launched from -- matching Assets/ShaderLibrary
        // and the editor's exe-relative font/data layout. SDL_GetBasePath() is SDL-owned.
        std::filesystem::path resolved = path;
        if (resolved.is_relative())
            if (const char* base = SDL_GetBasePath())
                resolved = std::filesystem::path(base) / path;

        int w = 0, h = 0, comp = 0;
        unsigned char* pixels = stbi_load(resolved.string().c_str(), &w, &h, &comp, 4);
        if (!pixels || w <= 0 || h <= 0)
        {
            ARC_WARN("Window::SetIcon: failed to load '{}'", resolved.string());
            if (pixels) stbi_image_free(pixels);
            return false;
        }

        // SDL_PIXELFORMAT_RGBA32 is byte-order R,G,B,A -- exactly stb's 4-channel layout.
        // SDL_SetWindowIcon copies the surface into the window, so the surface and its
        // backing pixels are ours to free immediately after.
        SDL_Surface* surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32,
                                                     pixels, w * 4);
        bool ok = false;
        if (surface)
        {
            ok = SDL_SetWindowIcon(m_window, surface);
            SDL_DestroySurface(surface);
        }
        else
        {
            ARC_WARN("Window::SetIcon: SDL_CreateSurfaceFrom failed: {}", SDL_GetError());
        }
        stbi_image_free(pixels);
        return ok;
    }

    void Window::SetNativeEventTap(NativeEventTap tap, void* user)
    {
        m_tap     = tap;
        m_tapUser = user;
    }

    void Window::ShowOpenFolderDialog(FolderPickedCallback cb, void* user) const
    {
        auto* ctx = new FolderCbCtx{ cb, user };   // freed by the trampoline
        SDL_ShowOpenFolderDialog(&FolderDialogTrampoline, ctx, m_window, nullptr, false);
    }

    namespace
    {
        // File-dialog context: owns the filter STRINGS too -- SDL requires the
        // filter array to stay valid until the callback fires, so everything
        // lives on the heap with the ctx and is freed by the trampoline.
        struct FileCbCtx
        {
            Window::FilePickedCallback cb;
            void* user;
            std::string filterName, filterPattern;
            std::string defaultPath;   // owned here too: outlive the async dialog
            SDL_DialogFileFilter filter{};
        };

        void SDLCALL FileDialogTrampoline(void* userdata, const char* const* filelist, int /*filter*/)
        {
            std::unique_ptr<FileCbCtx> ctx(static_cast<FileCbCtx*>(userdata));
            const char* picked = (filelist && filelist[0]) ? filelist[0] : nullptr;
            if (ctx->cb) ctx->cb(picked, ctx->user);
        }

        FileCbCtx* MakeFileCtx(Window::FilePickedCallback cb, void* user,
                               const char* filterName, const char* filterPattern,
                               const char* defaultPath)
        {
            auto* ctx = new FileCbCtx{ cb, user,
                                       filterName ? filterName : "",
                                       filterPattern ? filterPattern : "",
                                       defaultPath ? defaultPath : "" };
            ctx->filter.name = ctx->filterName.c_str();
            ctx->filter.pattern = ctx->filterPattern.c_str();
            return ctx;
        }
    }

    void Window::ShowSaveFileDialog(FilePickedCallback cb, void* user,
                                    const char* filterName, const char* filterPattern,
                                    const char* defaultPath) const
    {
        FileCbCtx* ctx = MakeFileCtx(cb, user, filterName, filterPattern, defaultPath);   // freed by the trampoline
        const bool hasFilter = filterName && filterPattern;
        SDL_ShowSaveFileDialog(&FileDialogTrampoline, ctx, m_window,
                               hasFilter ? &ctx->filter : nullptr, hasFilter ? 1 : 0,
                               ctx->defaultPath.empty() ? nullptr : ctx->defaultPath.c_str());
    }

    void Window::ShowOpenFileDialog(FilePickedCallback cb, void* user,
                                    const char* filterName, const char* filterPattern,
                                    const char* defaultPath) const
    {
        FileCbCtx* ctx = MakeFileCtx(cb, user, filterName, filterPattern, defaultPath);   // freed by the trampoline
        const bool hasFilter = filterName && filterPattern;
        SDL_ShowOpenFileDialog(&FileDialogTrampoline, ctx, m_window,
                               hasFilter ? &ctx->filter : nullptr, hasFilter ? 1 : 0,
                               ctx->defaultPath.empty() ? nullptr : ctx->defaultPath.c_str(),
                               /*allow_many=*/false);
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
