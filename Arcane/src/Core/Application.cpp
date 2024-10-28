#include "arcpch.h"
#include "Application.h"

#include "Input/ActionSystem.h"
#include "Input/Input.h"
#include "Input/InputManager.h"
#include "Network/Network.h"
#include "RuntimeLayer.h"

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
#endif
   }
}

ARC::Application::~Application()
{
   if (m_running)
      Close();
}

void ARC::Application::Run()
{
   using Clock = std::chrono::steady_clock;
   using TimePoint = Clock::time_point;
   using Duration = std::chrono::duration<float>;

   static constexpr float TARGET_DELTA_TIME = 1.0f / TARGET_UPDATES_PER_SECOND;
   static constexpr Duration FIXED_DELTA_TIME { TARGET_DELTA_TIME };
   static constexpr Duration MAX_FRAME_TIME { 0.25f };  // 250ms max frame time
   static constexpr int MAX_FIXED_UPDATES_PER_FRAME = 5;

   Duration accumulator { 0.0f };
   TimePoint currentTime = Clock::now();
   TimePoint lastTime = currentTime;
   TimePoint fpsCounterTime = currentTime;

   PerformanceMetrics metrics;

   while (m_running)
   {
      currentTime = Clock::now();
      Duration frameTime = currentTime - lastTime;
      lastTime = currentTime;

      if (frameTime > MAX_FRAME_TIME)
      {
         ARC_CORE_WARN("Frame time spike detected: {}s", frameTime.count());
         frameTime = MAX_FRAME_TIME;
      }

      accumulator += frameTime;

      ProcessEvents();

      Update(frameTime.count());
      ++metrics.updates;

      int fixedUpdateCount = 0;
      const int maxFixedUpdatesPerFrame = 5;

      int fixedSteps = 0;
      while (accumulator >= FIXED_DELTA_TIME && fixedSteps < MAX_FIXED_UPDATES_PER_FRAME)
      {
         FixedUpdate(FIXED_DELTA_TIME.count());
         accumulator -= FIXED_DELTA_TIME;
         ++metrics.fixedUpdates;
         ++fixedSteps;
      }

      if (fixedSteps == MAX_FIXED_UPDATES_PER_FRAME)
      {
         ARC_CORE_WARN("Dropping {} seconds of accumulated time", (accumulator - (FIXED_DELTA_TIME * fixedSteps)).count());
         accumulator = Duration { 0 };
      }

      float alpha = std::clamp(accumulator / FIXED_DELTA_TIME, 0.0f, 1.0f);
      Render(alpha);
      ++metrics.frames;

      Input::ClearReleasedKeys();

      constexpr Duration oneSecond { 1.0f };
      if (currentTime - fpsCounterTime >= oneSecond)
      {
         UpdatePerformanceMetrics(metrics);
         ResetMetrics(metrics);
         fpsCounterTime = currentTime;
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

      if (!LoggingManager::InitializeCore() || !LoggingManager::InitializeApplication())
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
      m_window->SetEventCallback([this](Event& e) { OnEvent(e); });
      m_window->CenterInScreen();
      m_window->SetResizable(info.resizableWindow);

      EventBus::GetInstance().Subscribe<WindowClosedEvent>([this](const WindowClosedEvent& event) { Close(); });

      auto runtimeLayer = std::make_unique<RuntimeLayer>();
      PushLayer(std::move(runtimeLayer));

      Network::Initialize();

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

   InputManager::GetInstance().Update();
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
   EventBus::GetInstance().Publish(event);

   if (event.handled)
      return;

   for (auto it = m_layerStack.rbegin(); it != m_layerStack.rend(); ++it)
   {
      (*it)->OnEvent(event);
      if (event.handled)
         break;
   }
}

void ARC::Application::Update(float deltaTime)
{
   for (const auto& layer : m_layerStack)
   {
      layer->OnUpdate(deltaTime);
   }
   ARC_ASSERT(m_updateCallback, "Attempted to call a null update delegate.");
   m_updateCallback(deltaTime);
}

void ARC::Application::FixedUpdate(float timeStep)
{
   for (const auto& layer : m_layerStack)
   {
      layer->OnFixedUpdate(timeStep);
   }
   ARC_ASSERT(m_updateCallback, "Attempted to call a null fixed update delegate.");
   m_fixedUpdateCallback(timeStep);
}

void ARC::Application::Render(float alpha /* TODO: Implement */)
{
   for (const auto& layer : m_layerStack)
   {
      layer->OnRender();
   }
   ARC_ASSERT(m_renderCallback, "Attempted to call a null render delegate.");
   m_renderCallback();
}

void ARC::Application::UpdatePerformanceMetrics(const PerformanceMetrics& metrics)
{
   m_framesPerSecond = metrics.frames;
   m_updatesPerSecond = metrics.updates;
   m_fixedUpdatesPerSecond = metrics.fixedUpdates;

#ifdef ARC_BUILD_DEBUG
   //ARC_CORE_DEBUG("Performance: FPS={} UPS={} FUPS={}", m_framesPerSecond, m_updatesPerSecond, m_fixedUpdatesPerSecond);
#endif
}

void ARC::Application::ResetMetrics(PerformanceMetrics& metrics)
{
   metrics = PerformanceMetrics{};
}

void ARC::Application::Close()
{
   if (!m_running)
      return;

   m_running = false;

   m_layerStack.Clear();

   if (m_window)
   {
      m_window->SetEventCallback([](Event& e) {});
      m_window->ProcessEvents();
      m_window.reset();
   }
   {
      std::scoped_lock<std::mutex> lock(m_eventQueueMutex);
      m_eventQueue.clear();
   }

   Network::Shutdown();
   LoggingManager::Shutdown();
}
