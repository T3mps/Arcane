#pragma once

#include "Window.h"
#include "Util/Delegate.h"
#include "Version.h"

namespace ARC
{
   struct ApplicationInfo
   {
      std::string name = "Arcane";
      uint32_t windowWidth = 960;
      uint32_t windowHeight = 540;
      bool resizableWindow = true;
      bool fullscreenWindow = false;
      bool decoratedWindow = true;
   };

   class Application
   {
   public:
      static constexpr uint32_t TARGET_UPDATES_PER_SECOND = 60U;
      static constexpr uint32_t MAX_ACCUMULATED_UPDATES = 5U;

      explicit Application(ApplicationInfo info = {});
      ~Application();
      Application(const Application&) = delete;
      Application& operator=(const Application&) = delete;
      Application(Application&&) noexcept;
      Application& operator=(Application&&) noexcept;

      static inline Application& GetInstance() { return *s_instance; }

      void Run();

      template<void (*Function)(float)>
      void RegisterUpdateCallback()       { m_updateCallback.Bind<Function>(); }

      template<void (*Function)(float)>
      void RegisterFixedUpdateCallback()  { m_fixedUpdateCallback.Bind<Function>(); }
      
      template<void (*Function)()>
      void RegisterRenderCallback()       { m_renderCallback.Bind<Function>(); }

   private:
      bool Initialize(ApplicationInfo info);

      void Update(float deltaTime);
      void FixedUpdate(float timeStep);
      void Render();

      void Cleanup();

      std::unique_ptr<Window> m_window;

      uint32_t m_framesPerSecond;
      uint32_t m_updatesPerSecond;
      uint32_t m_fixedUpdatesPerSecond;

      bool m_running;

      Delegate<void(float)> m_updateCallback;
      Delegate<void(float)> m_fixedUpdateCallback;
      Delegate<void(void)> m_renderCallback;

      static Application* s_instance;
   };
}