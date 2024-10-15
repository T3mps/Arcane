#include "arcpch.h"
#include "Loom.h"

#include "CommandLineParser.h"
#include "Core/Console.h"
#include "DLLInterface.h"
#include "Exception.h"
#include "Util/StringUtil.h"

static constexpr const wchar_t* ENTRY_POINT_FUNCTION_NAME = L"EntryPoint";

std::unique_ptr<Loom> Loom::Create(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int32_t nCmdShow)
{
   auto loom = std::make_unique<Loom>(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
   if (!loom)
   {
#ifdef ARC_BUILD_DEBUG
      ARC_DEBUGBREAK();
#endif;
      exit(EXIT_FAILURE);
   }
   return loom;
}

Loom::Loom(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int32_t nCmdShow) : m_hModule(nullptr)
{
   ARC::Console::Initialize();

   ARC::LoggingManager::GetInstance().SetCoreLogger(new ARC::Logger(ARC::LoggingManager::DEFAULT_CORE_LOGGER_NAME));

   std::wstring dllName;
   try
   {
      ProcessCommandLineArgs(lpCmdLine, dllName);
      ProcessDLL(hInstance, nCmdShow, dllName);
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
      UnloadDLL(m_hModule);
   ARC::LoggingManager::Shutdown();
}

void Loom::ProcessCommandLineArgs(LPWSTR lpCmdLine, std::wstring& dllName) const
{
   OptionHandlers handlers;
   PositionalArguments positionalArgs;
   std::wstring errorMsg;

   ARC_CORE_INFO(L"Processing command line arguments...");
   if (!ProcessCommandLine(lpCmdLine, handlers, positionalArgs, errorMsg))
   {
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }

   ARC_CORE_INFO(L"Validating command line arguments...");
   if (GetPositionalArgumentCount(positionalArgs) < 1)
   {
      errorMsg = L"Usage: Loom.exe <ClientDLLName>";
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }

   ARC_CORE_INFO(L"Acquiring client DLL...");
   errorMsg = L"No valid DLL name provided.";
   if (TryPopPositionalArgument(positionalArgs, dllName, errorMsg.c_str()) != 0)
   {
      ARC_CORE_ERROR(errorMsg);
      throw CommandLineException(errorMsg);
   }
}

void Loom::ProcessDLL(HINSTANCE hInstance, int32_t nCmdShow, const std::wstring& dllName) const
{
   EntryPointFunc runGameFunc;
   m_hModule = LoadDLL(dllName, ENTRY_POINT_FUNCTION_NAME, runGameFunc);
   if (!m_hModule)
   {
      const std::wstring errorMsg = L"Error loading " + dllName;
      ARC_CORE_ERROR(errorMsg);
      throw DLLLoadException(errorMsg);
   }

   ARC_CORE_INFO(L"Successfully loaded '" + dllName + L"'.");
   RunDLL(runGameFunc, hInstance, nCmdShow);
}
