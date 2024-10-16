#include "arcpch.h"
#include "Loom.h"

#include "CommandLineParser.h"
#include "Core/Console.h"
#include "ModuleInterface.h"
#include "Exception.h"
#include "Util/StringUtil.h"

static constexpr const char* ENTRY_POINT_FUNCTION_NAME = "EntryPoint";

Loom::Loom(int argc, char* argv[]) : m_hModule(nullptr)
{
   ARC::Console::Initialize();

   ARC::LoggingManager::GetInstance().SetCoreLogger(new ARC::Logger(ARC::LoggingManager::DEFAULT_CORE_LOGGER_NAME));

   std::wstring moduleName;
   try
   {
      ProcessCommandLineArgs(argc, argv, moduleName);
      ProcessModule(moduleName);
   } 
   catch (const Exception& e)
   {
      ARC_CORE_FATAL(ARC::StringUtil::ToWString(e.what()));
   }
}

Loom::~Loom()
{
   ARC_CORE_INFO(L"Cleaning up Loom resources...");
   if (m_hModule)
      Module::Unload(m_hModule);
   ARC::LoggingManager::Shutdown();
}

void Loom::ProcessCommandLineArgs(int argc, char* argv[], std::wstring& moduleName) const
{
   OptionHandlers handlers;
   PositionalArguments positionalArgs;
   std::wstring errorMsg;

   ARC_CORE_INFO(L"Processing command line arguments...");
   if (!ProcessCommandLine(argc, argv, handlers, positionalArgs, errorMsg))
   {
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }

   ARC_CORE_INFO(L"Validating command line arguments...");
   if (GetPositionalArgumentCount(positionalArgs) < 1)
   {
      errorMsg = L"Usage: Loom <ClientModuleName>";
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }

   ARC_CORE_INFO(L"Acquiring client module...");
   errorMsg = L"No valid module name provided.";
   if (TryPopPositionalArgument(positionalArgs, moduleName, errorMsg.c_str()) != 0)
   {
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }
}

void Loom::ProcessModule(const std::wstring& moduleName) const
{
   Module::EntryPointFunc runGameFunc;
   m_hModule = Module::Load(moduleName, ENTRY_POINT_FUNCTION_NAME, runGameFunc);
   if (!m_hModule)
   {
      const std::wstring errorMsg = L"Error loading " + moduleName;
      ARC_CORE_ERROR(errorMsg);
      throw ModuleLoadException(errorMsg);
   }

   ARC_CORE_INFO(L"Successfully loaded '" + moduleName + L"'.");
   Module::Run(runGameFunc);
}
