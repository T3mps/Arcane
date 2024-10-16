#include "arcpch.h"
#include "Application.h"

#include "Scene/SceneManager.h"

#ifdef ARC_BUILD_DEBUG
   #include "Util/DumpGenerator.h"
#endif

static std::thread::id s_mainThreadID;

ARC::Application::Application(ApplicationInfo info) :
   m_updatesPerSecond(0U),
   m_fixedUpdatesPerSecond(0U),
   m_framesPerSecond(0U),
   m_running(true)
{
   if (!Initialize(info))
   {
      if (&LoggingManager::GetCoreLogger())
         ARC_CORE_ERROR("Application initialization failed");
      MessageBox(NULL, L"Application initialization failed", L"Error", MB_OK);

#ifdef ARC_BUILD_DEBUG
      ARC_DEBUGBREAK();
#else
      exit(EXIT_FAILURE);
#endif;
   }
}

ARC::Application::~Application()
{
   Cleanup();
}

ARC::Application::Application(Application&& other) noexcept :
   m_window(std::move(other.m_window)),
   m_framesPerSecond(other.m_framesPerSecond),
   m_updatesPerSecond(other.m_updatesPerSecond),
   m_fixedUpdatesPerSecond(other.m_fixedUpdatesPerSecond),
   m_updateCallback(std::move(other.m_updateCallback)),
   m_fixedUpdateCallback(std::move(other.m_fixedUpdateCallback)),
   m_renderCallback(std::move(other.m_renderCallback))
{
   other.m_framesPerSecond = 0;
   other.m_updatesPerSecond = 0;
   other.m_fixedUpdatesPerSecond = 0;
}

ARC::Application& ARC::Application::operator=(Application&& other) noexcept
{
   if (this != &other)
   {
      m_window.reset();

      m_window = std::move(other.m_window);
      m_framesPerSecond = other.m_framesPerSecond;
      m_updatesPerSecond = other.m_updatesPerSecond;
      m_fixedUpdatesPerSecond = other.m_fixedUpdatesPerSecond;
      m_updateCallback = std::move(other.m_updateCallback);
      m_fixedUpdateCallback = std::move(other.m_fixedUpdateCallback);
      m_renderCallback = std::move(other.m_renderCallback);

      other.m_framesPerSecond = 0;
      other.m_updatesPerSecond = 0;
      other.m_fixedUpdatesPerSecond = 0;
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

   constexpr duration<float> fixedUpdateRate = milliseconds(1000 / TARGET_UPDATES_PER_SECOND);
   constexpr float maxAccumulatedTime = fixedUpdateRate.count() * MAX_ACCUMULATED_UPDATES;

   time_point<steady_clock> lastUpdateTime = steady_clock::now();
   time_point<steady_clock> lastFixedUpdateTime = steady_clock::now();
   time_point<steady_clock> lastSecond = steady_clock::now();

   uint32_t frames = 0;
   uint32_t updates = 0;
   uint32_t fixedUpdates = 0;
   duration<float> accumulator = duration<float>::zero();

   while (m_running)
   {
      m_window->ProcessEvents();

      auto now = steady_clock::now();
      auto frameElapsedTime = duration_cast<duration<float>>(now - lastUpdateTime);
      lastUpdateTime = now;

      Update(frameElapsedTime.count());
      ++updates;

      auto fixedUpdateElapsedTime = duration_cast<duration<float>>(now - lastFixedUpdateTime);
      lastFixedUpdateTime = now;
      accumulator += fixedUpdateElapsedTime;

      if (accumulator.count() > maxAccumulatedTime)
         accumulator = duration<float>(maxAccumulatedTime);

      while (accumulator >= fixedUpdateRate)
      {
         FixedUpdate(fixedUpdateRate.count());
         accumulator -= fixedUpdateRate;
         ++fixedUpdates;
      }

      Render();
      ++frames;

      if (duration_cast<duration<float>>(now - lastSecond).count() >= 1.0f)
      {
         m_framesPerSecond = frames;
         m_updatesPerSecond = updates;
         m_fixedUpdatesPerSecond = fixedUpdates;

         frames = 0;
         updates = 0;
         fixedUpdates = 0;
         lastSecond = now;

         /*ARC_CORE_INFO("FPS: "   + std::to_string(m_framesPerSecond)    +
                     " UPS: "    + std::to_string(m_updatesPerSecond)   +
                     " FUPS: "   + std::to_string(m_fixedUpdatesPerSecond));*/
      }

      //std::this_thread::sleep_for(milliseconds(1));
   }
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
      logging.SetCoreLogger(new Logger(LoggingManager::DEFAULT_CORE_LOGGER_NAME));
      if (!&logging.GetCoreLogger())
      {
         success = false;
         return;
      }
      logging.SetApplicationLogger(new Logger(LoggingManager::DEFAULT_APPLICATION_LOGGER_NAME));
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
      m_window->CenterInScreen();
      m_window->SetResizable(info.resizableWindow);

      SceneManager::Initialize();

      ARC_CORE_INFO("Application initialized");
   });

   return success;
}

void ARC::Application::Update(float deltaTime)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Update(deltaTime);

   if (m_updateCallback)
      m_updateCallback(deltaTime);
}

void ARC::Application::FixedUpdate(float timeStep)
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->FixedUpdate(timeStep);

   if (m_fixedUpdateCallback)
      m_fixedUpdateCallback(timeStep);
}

void ARC::Application::Render()
{
   if (auto currentScene = SceneManager::GetCurrentScene())
      currentScene->Render();

   if (m_renderCallback)
      m_renderCallback();
}

void ARC::Application::Cleanup()
{
   m_running = false;
}
