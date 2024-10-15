#pragma once

#include "Window.h"
#include "Util/Delegate.h"
#include "Version.h"

namespace ARC
{
   struct ApplicationInfo
   {
      HINSTANCE hInstance;
      int nCmdShow;
   };

   class Application
   {
   public:
      static constexpr uint32_t TARGET_UPDATES_PER_SECOND = 60U;
      static constexpr uint32_t MAX_ACCUMULATED_UPDATES = 5U;

      explicit Application(ApplicationInfo info);
      ~Application();
      Application(const Application&) = delete;
      Application& operator=(const Application&) = delete;
      Application(Application&&) noexcept;
      Application& operator=(Application&&) noexcept;

      void Run();

      template<void (*Function)(float)>
      void RegisterUpdateCallback()       { m_updateCallback.Bind<Function>(); }

      template<void (*Function)(float)>
      void RegisterFixedUpdateCallback()  { m_fixedUpdateCallback.Bind<Function>(); }
      
      template<void (*Function)()>
      void RegisterRenderCallback()       { m_renderCallback.Bind<Function>(); }

   private:
      bool Initialize(HINSTANCE hInstance, int nCmdShow);

      void Update(float deltaTime);
      void FixedUpdate(float timeStep);
      void Render();

      void Cleanup();

      std::unique_ptr<Window> m_window;

      uint32_t m_framesPerSecond;
      uint32_t m_updatesPerSecond;
      uint32_t m_fixedUpdatesPerSecond;

      Delegate<void(float)> m_updateCallback;
      Delegate<void(float)> m_fixedUpdateCallback;
      Delegate<void(void)> m_renderCallback;
   };
}