#pragma once

namespace ARC
{
   class Window
   {
   public:
      using ResizeCallback       =  std::function<void(int32_t width, int32_t height)>;
      using KeyCallback          =  std::function<void(WPARAM key, bool isDown)>;
      using MouseMoveCallback    =  std::function<void(int32_t x, int32_t y)>;
      using MouseButtonCallback  =  std::function<void(int32_t button, bool isDown)>;
      using CloseCallback        =  std::function<bool()>; // Return false to cancel close

      Window(HINSTANCE hInstance, int32_t nCmdShow, const std::wstring& windowTitle = L"Arcane", int32_t width = 800, int32_t height = 600);
      ~Window();

      Window(const Window&) = delete;
      Window& operator=(const Window&) = delete;
      Window(Window&&) = delete;
      Window& operator=(Window&&) = delete;

      bool Initialize();
      void Cleanup();
      void ProcessMessages();

      void SetResizeCallback(ResizeCallback callback) { m_resizeCallback = callback; }
      void SetKeyCallback(KeyCallback callback) { m_keyCallback = callback; }
      void SetMouseMoveCallback(MouseMoveCallback callback) { m_mouseMoveCallback = callback; }
      void SetMouseButtonCallback(MouseButtonCallback callback) { m_mouseButtonCallback = callback; }
      void SetCloseCallback(CloseCallback callback) { m_closeCallback = callback; }

      int32_t GetWidth() const { return m_width; }
      int32_t GetHeight() const { return m_height; }
      const std::wstring& GetTitle() const { return m_windowTitle; }

      void SetWidth(int32_t width) { m_width = width; }
      void SetHeight(int32_t height) { m_height = height; }
      void SetTitle(const std::wstring& title);

      void ToggleFullscreen();
      bool IsFullscreen() const { return m_isFullscreen; }

      [[nodiscard]] HWND GetHWND() const { return m_hWnd; }
      bool IsRunning() const { return m_isRunning; }

   private:
      bool RegisterWindowClass();
      bool CreateAppWindow(int32_t nCmdShow);
      static LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

      HINSTANCE m_hInstance;
      HWND m_hWnd;
      std::wstring m_windowClassName;
      std::wstring m_windowTitle;
      int32_t m_width;
      int32_t m_height;
      WINDOWPLACEMENT m_windowPlacement;
      bool m_isRunning;
      bool m_isFullscreen;

      ResizeCallback m_resizeCallback;
      KeyCallback m_keyCallback;
      MouseMoveCallback m_mouseMoveCallback;
      MouseButtonCallback m_mouseButtonCallback;
      CloseCallback m_closeCallback;
   };
}
