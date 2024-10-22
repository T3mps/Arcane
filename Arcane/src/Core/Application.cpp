#include "arcpch.h"
#include "Application.h"

#include "Event/ApplicationEvent.h"
#include "Input.h"

#ifdef ARC_BUILD_DEBUG
   #include "Util/DumpGenerator.h"
#endif

ARC::Application* ARC::Application::s_instance = nullptr;
static std::thread::id s_mainThreadID;

ARC::Application::Application(ApplicationInfo info) :
   m_updatesPerSecond(0U),
   m_framesPerSecond(0U),
   m_running(true)
{
   s_instance = this;
   s_mainThreadID = std::this_thread::get_id();

   if (!Initialize(info))
   {
      if (&LoggingManager::GetCoreLogger())
         ARC::Log::CoreError("Application initialization failed");
#ifdef ARC_PLATFORM_WINDOWS
      MessageBox(NULL, L"Application initialization failed", L"Error", MB_OK);
#endif
#ifdef ARC_BUILD_DEBUG
      ARC_DEBUGBREAK();
#else
      exit(EXIT_FAILURE);
#endif;
   }
}

ARC::Application::~Application()
{
   Close();
}

ARC::Application::Application(Application&& other) noexcept :
   m_window(std::move(other.m_window)),
   m_framesPerSecond(other.m_framesPerSecond),
   m_updatesPerSecond(other.m_updatesPerSecond),
   m_updateCallback(std::move(other.m_updateCallback)),
   m_renderCallback(std::move(other.m_renderCallback)),
   m_running(other.m_running)
{
   other.m_framesPerSecond = 0;
   other.m_updatesPerSecond = 0;
}

ARC::Application& ARC::Application::operator=(Application&& other) noexcept
{
   if (this != &other)
   {
      m_window.reset();

      m_window = std::move(other.m_window);
      m_framesPerSecond = other.m_framesPerSecond;
      m_updatesPerSecond = other.m_updatesPerSecond;
      m_updateCallback = std::move(other.m_updateCallback);
      m_renderCallback = std::move(other.m_renderCallback);

      other.m_framesPerSecond = 0;
      other.m_updatesPerSecond = 0;
   }

   return *this;
}

void ARC::Application::Run()
{
   using std::chrono::steady_clock;
   using std::chrono::time_point;
   using std::chrono::duration;
   using std::chrono::milliseconds;
   using std::chrono::duration_cast;

   time_point<steady_clock> lastUpdateTime = steady_clock::now();
   time_point<steady_clock> lastSecond = steady_clock::now();

   uint32_t frames = 0;
   uint32_t updates = 0;

   while (m_running)
   {
      ProcessEvents();

      auto now = steady_clock::now();
      auto frameElapsedTime = duration_cast<duration<float>>(now - lastUpdateTime);
      lastUpdateTime = now;

      Update(frameElapsedTime.count());
      ++updates;

      Render();
      ++frames;

      Input::ClearReleasedKeys();

      if (duration_cast<duration<float>>(now - lastSecond).count() >= 1.0f)
      {
         m_framesPerSecond = frames;
         m_updatesPerSecond = updates;

         frames = 0;
         updates = 0;
         lastSecond = now;

         Log::CoreDebug("FPS: " + std::to_string(m_framesPerSecond) +
                        " UPS: "  + std::to_string(m_updatesPerSecond));
      }
   }
}

void ARC::Application::PushLayer(Layer* layer)
{
   m_layerStack.Push(layer);
   layer->OnAttach();
}

void ARC::Application::PushOverlay(Layer* layer)
{
   m_layerStack.PushFront(layer);
   layer->OnAttach();
}

void ARC::Application::PopLayer(Layer* layer)
{
   m_layerStack.Pop(layer);
   layer->OnDetach();
}

void ARC::Application::PopOverlay(Layer* layer)
{
   m_layerStack.PopFront(layer);
   layer->OnDetach();
}

void ARC::Application::OnEvent(Event& event)
{
   EventDispatcher dispatcher(event);
   dispatcher.Dispatch<WindowClosedEvent>([this](WindowClosedEvent& e)        { return OnWindowClose(e); });

   for (auto& callback : m_eventCallbacks)
   {
      callback(event);

      if (event.handled)
         break;
   }

   if (event.handled)
      return;

   for (auto it = m_layerStack.end(); it != m_layerStack.begin(); )
   {
      (*--it)->OnEvent(event);
      if (event.handled)
         break;
   }
}

void ARC::Application::SyncEvents()
{
   std::scoped_lock<std::mutex> lock(m_eventQueueMutex);
   for (auto& [synced, _] : m_eventQueue)
   {
      synced = true;
   }
}

std::thread::id ARC::Application::GetMainThreadID()
{
   return s_mainThreadID;
}

bool ARC::Application::Initialize(ApplicationInfo info)
{
   static std::once_flag initFlag;
   bool success = true;

   std::call_once(initFlag, [&]()
   {
#ifdef ARC_BUILD_DEBUG
      RegisterDumpHandler();
#endif

      //std::filesystem::current_path("/");

      auto& logging = LoggingManager::GetInstance();
      logging.SetCoreLogger(new Logger(LoggingManager::DEFAULT_CORE_LOGGER_NAME.data()));
      if (!&logging.GetCoreLogger())
      {
         success = false;
         return;
      }
      logging.SetApplicationLogger(new Logger(LoggingManager::DEFAULT_APPLICATION_LOGGER_NAME.data()));
      if (!&logging.GetApplicationLogger())
      {
         success = false;
         return;
      }

      WindowInfo windowInfo;
      windowInfo.title = info.name;
      windowInfo.width = info.windowWidth;
      windowInfo.height = info.windowHeight;
      windowInfo.decorated = info.decoratedWindow;
      windowInfo.fullscreen = info.fullscreenWindow;

      m_window = std::make_unique<Window>(windowInfo);
      ARC_ASSERT((success = m_window != nullptr), "Unable to create window instance.");

      m_window->Initialize();
      m_window->SetEventCallback([this](Event& e) { OnEvent(e); });
      m_window->CenterInScreen();
      m_window->SetResizable(info.resizableWindow);

      SceneManager::Initialize();

      Log::CoreInfo("Application initialized");
   });

   return success;
}

void ARC::Application::ProcessEvents()
{
   Input::TransitionPressedKeys();
   Input::TransitionPressedButtons();

   m_window->ProcessEvents();

   // We need to ensure thread safety when accessing the event queue, but we
   // can't hold the lock while calling func() because we have no control over it.
   // Potential problems with holding the lock during func() include:
   // 1. func() might be slow, which would hold the lock for an extended period,
   //    degrading performance and preventing other threads from accessing the queue.
   // 2. func() might queue additional events, causing a deadlock if the event queue
   //    is accessed again while the lock is held.

   // To mitigate these risks, we only hold the lock when accessing the queue, 
   // but we release it before calling func(). This ensures:
   // - The queue is safely modified within the lock.
   // - func() is called outside the lock, avoiding deadlocks and performance issues.
   while (true)
   {
      std::function<void()> func;
      {
         // Lock the mutex only when accessing the queue to prevent race conditions
         std::scoped_lock<std::mutex> lock(m_eventQueueMutex);

         if (m_eventQueue.empty())
            break;

         const auto& [synced, eventFunc] = m_eventQueue.front();
         if (!synced)
            break;

         func = eventFunc;
         // Remove the event from the queue while holding the lock
         m_eventQueue.pop_front();
      }

      // Call the function outside the lock
      func();
   }
}

bool ARC::Application::OnWindowClose(WindowClosedEvent& e)
{
   Close();
   return false;
}

void ARC::Application::Update(float deltaTime)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Update(deltaTime);

   for (Layer* layer : m_layerStack)
      layer->OnUpdate(deltaTime);

   if (m_updateCallback)
      m_updateCallback(deltaTime);
}

void ARC::Application::Render()
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Render();

   if (m_renderCallback)
      m_renderCallback();
}

void ARC::Application::Close()
{
   m_eventCallbacks.clear();
   m_window->SetEventCallback([](Event& e) {});
   m_layerStack.Clear();

   m_running = false;
}
