#pragma once

#include "Event/ApplicationEvent.h"
#include "Event/Event.h"
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
      Application(Application&&) noexcept;
      Application& operator=(Application&&) noexcept;

      static inline Application& GetInstance() { return *s_instance; }

      void Run();

      void PushLayer(Layer* layer);
      void PushOverlay(Layer* layer);
      void PopLayer(Layer* layer);
      void PopOverlay(Layer* layer);

      void OnEvent(Event& event); 
      inline void AddEventCallback(const Event::Callback& eventCallback) { m_eventCallbacks.push_back(eventCallback); }

      template<typename Func>
      inline void QueueEvent(Func&& func)
      {
         std::scoped_lock<std::mutex> lock(m_eventQueueMutex);
         m_eventQueue.emplace_back(true, func);
      }

      template<typename E, bool DispatchImmediately = false, typename... EventArgs>
      inline void DispatchEvent(EventArgs&&... args)
      {
         std::shared_ptr<E> event = std::make_shared<E>(std::forward<EventArgs>(args)...);
         if constexpr (DispatchImmediately)
         {
            OnEvent(*event);
         }
         else
         {
            std::scoped_lock<std::mutex> lock(m_eventQueueMutex);
            m_eventQueue.emplace_back(false, [event](){ Application::GetInstance().OnEvent(*event); });
         }
      }

      void SyncEvents();

      template<void (*Callback)(float)>
      inline void RegisterUpdateCallback()       { m_updateCallback.Bind<Callback>(); }
      template<void (*Callback)()>
      inline void RegisterRenderCallback()       { m_renderCallback.Bind<Callback>(); }

      inline const ApplicationInfo& GetInfo() const { return m_info; }
      inline Window& GetWindow() { return *m_window; }
      inline bool IsRunning() const { return m_running; }

      static std::thread::id GetMainThreadID();

   private:
      bool Initialize(ApplicationInfo info);

      void ProcessEvents();

      bool OnWindowClose(WindowClosedEvent& e);

      void Update(float deltaTime);
      void Render();

      void Close();

      ApplicationInfo m_info;
      std::unique_ptr<Window> m_window;
      LayerStack m_layerStack;

      std::mutex m_eventQueueMutex;
      std::deque<std::pair<bool, std::function<void()>>> m_eventQueue;
      std::vector<Event::Callback> m_eventCallbacks;

      uint32_t m_framesPerSecond;
      uint32_t m_updatesPerSecond;
      bool m_running;

      Delegate<void(float)> m_updateCallback;
      Delegate<void(void)> m_renderCallback;

      static Application* s_instance;
   };
} // namespace ARC
