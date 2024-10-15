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

   FreeConsole();

   ARC::Console::Flush(std::cout);
   ARC::Console::Flush(std::cerr);
   ARC::Console::Flush(std::clog);
}
