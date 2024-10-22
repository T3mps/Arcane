#include "arcpch.h"
#include "LoggingManager.h"

#include "Core/Console.h"

ARC::LoggingManager::LoggingManager() :
   m_coreLogger(nullptr),
   m_applicationLogger(nullptr)
{}

void ARC::LoggingManager::Shutdown()
{
   auto& instance = GetInstance();
   instance.m_coreLogger.reset();
   instance.m_applicationLogger.reset();

   ARC::Console::Flush(std::clog);

   FreeConsole();
}
