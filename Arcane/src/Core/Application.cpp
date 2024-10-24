#include "arcpch.h"
#include "Application.h"

#include "Input/Input.h"

#ifdef ARC_BUILD_DEBUG
   #include "Util/DumpGenerator.h"
#endif

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
         ARC_CORE_ERROR("Application initialization failed");
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
   if (m_running)
      Close();
}

void ARC::Application::Run()
{
   using duration = std::chrono::duration<float>;
   using clock = std::chrono::steady_clock;
   using time_point = clock::time_point;

   constexpr const duration fixedDeltaTime = duration(1.0f / TARGET_UPDATES_PER_SECOND);
   duration accumulator = duration::zero();

   time_point lastTime = clock::now();
   time_point lastSecond = lastTime;

   uint32_t frames = 0;
   uint32_t updates = 0;
   uint32_t fixedUpdates = 0;

   while (m_running)
   {
      time_point currentTime = clock::now();
      duration frameTime = currentTime - lastTime;
      lastTime = currentTime;

      constexpr const duration maxFrameTime = duration(0.25f); // 250ms
      if (frameTime > maxFrameTime)
      {
         ARC_CORE_WARN("Large frame time detected: " + StringUtil::ToString(frameTime.count()));
         frameTime = maxFrameTime;
      }

      accumulator += frameTime;

      ProcessEvents();

      Update(frameTime.count());
      ++updates;

      int fixedUpdateCount = 0;
      const int maxFixedUpdatesPerFrame = 5;

      while (accumulator >= fixedDeltaTime && fixedUpdateCount < maxFixedUpdatesPerFrame)
      {
         FixedUpdate(fixedDeltaTime.count());
         accumulator -= fixedDeltaTime;
         ++fixedUpdates;
         ++fixedUpdateCount;
      }

      if (fixedUpdateCount == maxFixedUpdatesPerFrame)
      {
         accumulator -= fixedDeltaTime * fixedUpdateCount;
         ARC_CORE_WARN("FixedUpdate loop exceeded maximum iterations.");
      }

      float alpha = accumulator / fixedDeltaTime;
      Render(alpha);
      ++frames;

      Input::ClearReleasedKeys();

      if (currentTime - lastSecond >= duration(1.0))
      {
         m_updatesPerSecond = updates;
         m_fixedUpdatesPerSecond = fixedUpdates;
         m_framesPerSecond = frames;

         updates = 0;
         fixedUpdates = 0;
         frames = 0;
         lastSecond = currentTime;

         /*ARC_CORE_DEBUG("FPS: "   + std::to_string(m_framesPerSecond)   +
                        " UPS: "  + std::to_string(m_updatesPerSecond)  +
                        " FUPS: " + std::to_string(m_fixedUpdatesPerSecond));*/
      }
   }
}

void ARC::Application::PushLayer(std::unique_ptr<Layer> layer)
{
   layer->OnAttach();
   m_layerStack.Push(std::move(layer));
}

void ARC::Application::PushOverlay(std::unique_ptr<Layer> overlay)
{
   overlay->OnAttach();
   m_layerStack.PushFront(std::move(overlay));
}

std::unique_ptr<ARC::Layer> ARC::Application::PopLayer(Layer* layer)
{
   auto removedLayer = m_layerStack.Pop(layer);
   if (removedLayer)
      removedLayer->OnDetach();
   return removedLayer;
}

std::unique_ptr<ARC::Layer> ARC::Application::PopOverlay(Layer* overlay)
{
   auto removedOverlay = m_layerStack.PopFront(overlay);
   if (removedOverlay)
      removedOverlay->OnDetach();
   return removedOverlay;
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
      if (!m_window)
      {
         ARC_CORE_ERROR("Unable to create window instance.");
         success = false;
         return;
      }

      m_window->Initialize();
      m_window->CenterInScreen();
      m_window->SetResizable(info.resizableWindow);

      EventBus::GetInstance().Subscribe<WindowClosedEvent>([this](const WindowClosedEvent& event)
      {
         Close();
      });

      SceneManager::Initialize();

      ARC_CORE_INFO("Application initialized");
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

   std::function<void()> callback;
   while (GetNextEvent(callback))
   {
      // Call the function outside the lock
      try
      {
         callback();
      }
      catch (const std::exception& e)
      {
         ARC_CORE_ERROR("Exception caught during event processing: {}", e.what());
         // TODO: Handle or rethrow as appropriate
      }
   }
}

bool ARC::Application::GetNextEvent(std::function<void()>& func)
{
   std::scoped_lock<std::mutex> lock(m_eventQueueMutex);

   if (m_eventQueue.empty())
      return false;

   func = std::move(m_eventQueue.front());
   m_eventQueue.pop_front();
   return true;
}

void ARC::Application::OnEvent(Event& event)
{
   for (auto it = m_layerStack.rbegin(); it != m_layerStack.rend(); ++it)
   {
      (*it)->OnEvent(event);
      if (event.handled)
         break;
   }

   if (!event.handled)
      EventBus::GetInstance().Publish(event);
}

void ARC::Application::Update(float deltaTime)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Update(deltaTime);

   for (const auto& layer : m_layerStack)
      layer->OnUpdate(deltaTime);

   if (m_updateCallback)
      m_updateCallback(deltaTime);
}

void ARC::Application::FixedUpdate(float timeStep)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->FixedUpdate(timeStep);

   for (const auto& layer : m_layerStack)
      layer->OnFixedUpdate(timeStep);

   if (m_fixedUpdateCallback)
      m_fixedUpdateCallback(timeStep);
}

void ARC::Application::Render(float alpha)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Render();

   if (m_renderCallback)
      m_renderCallback();
}

void ARC::Application::Close()
{
   m_layerStack.Clear();

   m_running = false;
}
