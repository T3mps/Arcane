#include "arcpch.h"
#include "LoggingManager.h"

#include "Core/Console.h"

ARC::Result<void> ARC::LoggingManager::InitializeInternal(const LoggingInfo& info)
{
   auto coreResult = InitializeCoreInternal(info);
   if (!coreResult)
      return coreResult;

   return InitializeApplicationInternal(info);
}

ARC::Result<void> ARC::LoggingManager::InitializeCoreInternal(const LoggingInfo& info)
{
   Result<void> result;
   std::call_once(m_coreInitFlag, [&]()
   {
      result = CreateLogger(m_coreLogger, DEFAULT_CORE_LOGGER_NAME.data(), info);
   });
   return result;
}

ARC::Result<void> ARC::LoggingManager::InitializeApplicationInternal(const LoggingInfo& info)
{
   Result<void> result;
   std::call_once(m_appInitFlag, [&]()
   {
      result = CreateLogger(m_applicationLogger, DEFAULT_APPLICATION_LOGGER_NAME.data(), info);
   });
   return result;
}

void ARC::LoggingManager::Shutdown()
{
   auto& instance = GetInstance();
   ARC_CORE_INFO("Shutting down loggers");
   instance.m_coreLogger.reset();
   instance.m_applicationLogger.reset();
   Console::Flush(std::clog);
}
