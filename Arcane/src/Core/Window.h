#pragma once

#include "GLFW/glfw3.h"
#include "Math/Vector.h"

namespace ARC
{
   struct WindowInfo
   {
      std::string title = "Arcane";
      uint32_t width = 960;
      uint32_t height = 540;
      bool decorated = true;
      bool fullscreen = false;
   };

   class Window
   {
   public:
      Window(const WindowInfo& info);
      ~Window();

      void Initialize();

      void SwapBuffers();
      void ProcessEvents();

      void CenterInScreen();
      void Maximize();

      inline GLFWwindow* GetGLFWPointer() const { return m_windowPointer; }

      inline uint32_t GetWidth() const { return m_properties.width; }
      inline uint32_t GetHeight() const { return m_properties.height; }

      const std::string& GetTitle() const { return m_properties.title; }
      void SetTitle(const std::string& title);

      void SetResizable(bool resizable) const;

   private:
      void Cleanup();

      GLFWwindow* m_windowPointer;
      WindowInfo m_properties;
      struct WindowData
      {
         std::string title;
         uint32_t width, height;
      } m_data;
   };
}
