#include "arcpch.h"
#include "Window.h"

#include "Logging/Logging.h"

ARC::Window::Window(HINSTANCE hInstance, int32_t nCmdShow, const std::wstring& windowTitle, int32_t width, int32_t height) :
   m_hInstance(hInstance),
   m_hWnd(nullptr),
   m_windowClassName(L"ArcaneWindow"),
   m_windowTitle(windowTitle),
   m_width(width),
   m_height(height),
   m_windowPlacement(),
   m_isRunning(true),
   m_isFullscreen(false)
{
   m_windowPlacement.length = sizeof(WINDOWPLACEMENT);

   if (!Initialize())
   {
      ARC_CORE_ERROR("Window initialization failed");
      MessageBox(NULL, L"Window initialization failed", L"Error", MB_OK);
      exit(EXIT_FAILURE);
   }

   ShowWindow(m_hWnd, nCmdShow);
   UpdateWindow(m_hWnd);
}

ARC::Window::~Window()
{
   Cleanup();
}

bool ARC::Window::Initialize()
{
   if (!RegisterWindowClass())
      return false;

   if (!CreateAppWindow(SW_SHOW))
      return false;

   ARC_CORE_INFO("Window initialized");
   return true;
}

void ARC::Window::ProcessMessages()
{
   MSG msg = { 0 };
   while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
   {
      TranslateMessage(&msg);
      DispatchMessage(&msg);

      if (msg.message == WM_QUIT)
         m_isRunning = false;
   }
}

void ARC::Window::Cleanup()
{
   if (m_hWnd)
   {
      DestroyWindow(m_hWnd);
      m_hWnd = nullptr;
   }
   UnregisterClass(m_windowClassName.c_str(), m_hInstance);
}

bool ARC::Window::RegisterWindowClass()
{
   WNDCLASSEX wc = { 0 };
   wc.cbSize = sizeof(WNDCLASSEX);
   wc.style = CS_HREDRAW | CS_VREDRAW;
   wc.lpfnWndProc = WindowProc;
   wc.hInstance = m_hInstance;
   wc.hCursor = LoadCursor(NULL, IDC_ARROW);
   wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
   wc.lpszClassName = m_windowClassName.c_str();

   if (!RegisterClassEx(&wc))
   {
      ARC_CORE_ERROR("Window registration failed");
      MessageBox(NULL, L"Window Registration failed", L"Error", MB_OK);
      return false;
   }
   return true;
}

bool ARC::Window::CreateAppWindow(int32_t nCmdShow)
{
   m_hWnd = CreateWindowEx
   (
      0,
      m_windowClassName.c_str(),
      m_windowTitle.c_str(),
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      m_width, m_height,
      NULL, NULL,
      m_hInstance,
      NULL
   );

   if (!m_hWnd)
   {
      ARC_CORE_ERROR("Window creation failed");
      MessageBox(NULL, L"Window creation failed", L"Error", MB_OK);
      return false;
   }

   SetWindowLongPtr(m_hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
   return true;
}

void ARC::Window::SetTitle(const std::wstring& title)
{
   m_windowTitle = title;
   if (m_hWnd)
      SetWindowText(m_hWnd, title.c_str());
}

void ARC::Window::ToggleFullscreen()
{
   if (m_isFullscreen)
   {
      SetWindowLongPtr(m_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
      SetWindowPlacement(m_hWnd, &m_windowPlacement);
      SetWindowPos(m_hWnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
      m_isFullscreen = false;
   }
   else
   {
      GetWindowPlacement(m_hWnd, &m_windowPlacement);
      SetWindowLongPtr(m_hWnd, GWL_STYLE, WS_POPUP);
      MONITORINFO mi = { sizeof(mi) };
      if (GetMonitorInfo(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTOPRIMARY), &mi))
         SetWindowPos(m_hWnd, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_FRAMECHANGED);
      m_isFullscreen = true;
   }
}

LRESULT CALLBACK ARC::Window::WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
   Window* window = reinterpret_cast<Window*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

   if (window)
   {
      switch (message)
      {
      case WM_SIZE:
         if (window->m_resizeCallback)
         {
            int32_t width = LOWORD(lParam);
            int32_t height = HIWORD(lParam);
            window->m_resizeCallback(width, height);
         }
         break;

      case WM_KEYDOWN:
      case WM_KEYUP:
         if (window->m_keyCallback)
         {
            bool isDown = (message == WM_KEYDOWN);
            window->m_keyCallback(wParam, isDown);
         }
         break;

      case WM_MOUSEMOVE:
         if (window->m_mouseMoveCallback)
         {
            int32_t x = GET_X_LPARAM(lParam);
            int32_t y = GET_Y_LPARAM(lParam);
            window->m_mouseMoveCallback(x, y);
         }
         break;

      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      {
         if (window->m_mouseButtonCallback)
         {
            bool isDown = (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN);
            int32_t button = 0;
            switch (message)
            {
               case WM_LBUTTONDOWN:
               case WM_LBUTTONUP:
                  button = 0; // Left button
                  break;
               case WM_RBUTTONDOWN:
               case WM_RBUTTONUP:
                  button = 1; // Right button
                  break;
               case WM_MBUTTONDOWN:
               case WM_MBUTTONUP:
                  button = 2; // Middle button
                  break;
               default:
                  break;
            }
            window->m_mouseButtonCallback(button, isDown);
         }
         break;
      }

      case WM_CLOSE:
         if (window->m_closeCallback)
         {
            if (!window->m_closeCallback())
               return 0; // Cancel close
         }
         window->m_isRunning = false;
         DestroyWindow(hWnd);
         break;

      case WM_DESTROY:
         window->m_isRunning = false;
         PostQuitMessage(0);
         break;

      default:
         return DefWindowProc(hWnd, message, wParam, lParam);
      }
      return 0;
   }

   return DefWindowProc(hWnd, message, wParam, lParam);
}
