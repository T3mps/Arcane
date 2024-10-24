#include "arcpch.h"
#include "Loom.h"

#include "Core/Console.h"
#include "DynamicLibrary.h"
#include "ErrorCode.h"

Loom::Loom(int32_t argc, char* argv[])
{
   ARC::Console::Initialize();
   ARC::LoggingManager::GetInstance().SetCoreLogger(new ARC::Logger(ARC::LoggingManager::DEFAULT_CORE_LOGGER_NAME.data()));

   auto result = Initialize(argc, argv);
   if (!result)
   {
      if (&ARC::LoggingManager::GetCoreLogger())
      {
         ARC_CORE_ERROR(result.GetErrorMessage());
         ARC_CORE_ERROR("Loom initialization failed");
      }
#ifdef ARC_PLATFORM_WINDOWS
      MessageBox(NULL, L"Loom initialization failed", L"Error", MB_OK);
#endif

      ARC::Error error = result.Error();
      uint64_t errorCode = error.Code();
      exit(static_cast<int32_t>(errorCode));
   }
   ARC_CORE_INFO("Loom initialized");
}

Loom::~Loom()
{
   Cleanup();
}

ARC::Result<void> Loom::Initialize(int32_t argc, char* argv[])
{
   static std::once_flag initFlag;

   ARC::Result<void> result;
   
   std::call_once(initFlag, [&]()
   {
      m_argParser = ArgumentParser();
      m_argParser.RegisterFlag("help", [this](const std::string&) { ShowHelpMessage(); }, "Displays help");
      m_argParser.RegisterFlag("h",    [this](const std::string&) { ShowHelpMessage(); }, "Displays help");
      m_argParser.Parse(argc, argv);

      ARC_CORE_INFO("Initializing Loom...");

      const auto& positionalArgs = m_argParser.GetPositionalArgs();
      if (positionalArgs.empty())
      {
         ARC_CORE_INFO(LOOM_USEAGE_STRING);
         result = ARC::Error::Create(LoomError::IncorrectParameterUsage, "No client module specified.");
         return;
      }

      std::ostringstream oss;
      for (int32_t i = 0; i < positionalArgs.size(); ++i)
      {
         const std::string& str = positionalArgs[i];
         oss << str;
         if (i != positionalArgs.size() - 1)
            oss << ", ";
      }
      ARC_CORE_INFO("Identified positional arguments: " + oss.str());

      std::filesystem::path clientModulePath = positionalArgs.front();

      m_clientModule = std::make_unique<Module>(clientModulePath);

      result = m_clientModule->Load();
      if (!result)
      {
         ARC_CORE_ERROR("Failed to load client module: " + clientModulePath.string());
         return;
      }

      ARC_CORE_INFO("Successfully loaded client module: " + m_clientModule->GetName());

      m_clientModule->StartMonitoring();
   });

   return result;
}

void Loom::Cleanup()
{
   if (m_clientModule)
   {
      auto result = m_clientModule->Unload();
      if (!result)
      {
         ARC_CORE_ERROR("Error occured while trying to unload module.");
         ARC_CORE_ERROR(result.GetErrorMessage());
      }
      m_clientModule.reset();
   }
}

void Loom::ShowHelpMessage()
{
   std::string message = m_argParser.BuildHelpMessage();
   ARC::Console::Output(message);
   exit(0);
}
