#pragma once

#include "Event/ApplicationEvent.h"
#include "Event/Event.h"
#include "Event/EventBus.h"
#include "Event/KeyEvent.h"
#include "Event/MouseEvent.h"
#include "Event/LayerStack.h"
#include "Scene/SceneManager.h"
#include "Window.h"
#include "Util/Delegate.h"
#include "Version.h"

namespace ARC
{
   struct ApplicationInfo
   {
      int32_t argc          = 0;
      char* argv            = nullptr;
      std::string name      = "Arcane";
      float windowScale     = 0.75f;
      uint32_t windowWidth  = static_cast<uint32_t>(1920 * windowScale);
      uint32_t windowHeight = static_cast<uint32_t>(1080 * windowScale);
      bool resizableWindow  = true;
      bool fullscreenWindow = false;
      bool decoratedWindow  = true;
   };

   class Application
   {
   public:
      static constexpr uint32_t TARGET_UPDATES_PER_SECOND = 60U;

      explicit Application(ApplicationInfo info = {});
      ~Application();
      Application(const Application&) = delete;
      Application& operator=(const Application&) = delete;

      static inline Application& GetInstance() { return *s_instance; }

      void Run();

      void PushLayer(std::unique_ptr<Layer> overlay);
      void PushOverlay(std::unique_ptr<Layer> overlay);
      std::unique_ptr<Layer> PopLayer(Layer* layer);
      std::unique_ptr<Layer> PopOverlay(Layer* overlay);

      template<typename Func>
      inline void QueueEvent(Func&& func)
      {
         std::scoped_lock<std::mutex> lock(m_eventQueueMutex);
         m_eventQueue.emplace_back(std::forward<Func>(func));
      }

      template<typename E, typename... EventArgs>
      void DispatchEvent(EventArgs&&... args)
      {
         auto event = std::make_shared<E>(std::forward<EventArgs>(args)...);

         if (std::this_thread::get_id() == GetMainThreadID())
         {
            OnEvent(*event);
         }
         else
         {
            QueueEvent([event = std::move(event), this]() mutable
            {
               OnEvent(*event);
            });
         }
      }

      template<void (*Callback)(float)>
      inline void RegisterUpdateCallback()       { m_updateCallback.Bind<Callback>(); }
      template<void (*Callback)(float)>
      inline void RegisterFixedUpdateCallback()       { m_fixedUpdateCallback.Bind<Callback>(); }
      template<void (*Callback)()>
      inline void RegisterRenderCallback()       { m_renderCallback.Bind<Callback>(); }

      inline const ApplicationInfo& GetInfo() const { return m_info; }
      inline Window& GetWindow() { return *m_window; }
      inline bool IsRunning() const { return m_running; }

      static std::thread::id GetMainThreadID();

   private:
      Application(Application&&) = delete;
      Application& operator=(Application&&) = delete;

      bool Initialize(ApplicationInfo info);

      void ProcessEvents();
      bool GetNextEvent(std::function<void()>& func);
      void OnEvent(Event& event);

      void Update(float deltaTime);
      void FixedUpdate(float timeStep);
      void Render(float alpha);

      void Close();

      ApplicationInfo m_info;
      std::unique_ptr<Window> m_window;
      LayerStack m_layerStack;

      std::deque<std::function<void()>> m_eventQueue;
      std::mutex m_eventQueueMutex;

      uint32_t m_framesPerSecond;
      uint32_t m_fixedUpdatesPerSecond;
      uint32_t m_updatesPerSecond;
      bool m_running;

      Delegate<void(float)> m_updateCallback;
      Delegate<void(float)> m_fixedUpdateCallback;
      Delegate<void(void)> m_renderCallback;

      inline static Application* s_instance;
   };
} // namespace ARC
