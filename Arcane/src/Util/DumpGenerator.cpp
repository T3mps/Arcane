#include "arcpch.h"
#include "DumpGenerator.h"

namespace { std::mutex dumpMutex; }

std::string GetDefaultDumpFileName()
{
   auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
   std::tm timeInfo;
   localtime_s(&timeInfo, &now);

   std::ostringstream fileName;
   fileName << "arcane_" << std::put_time(&timeInfo, "%Y-%m-%d_%H-%M-%S") << ".dmp";
   return fileName.str();
}

bool ARC::DumpGenerator::MiniDump(EXCEPTION_POINTERS* exceptionInfo, DumpType dumpType, const std::string& customPath)
{
   std::lock_guard<std::mutex> guard(dumpMutex); // Thread-safe access

   std::string dumpFilePath = customPath.empty() ? GetDefaultDumpFileName() : customPath;

   HANDLE hFile = CreateFileA(dumpFilePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
   if (hFile == INVALID_HANDLE_VALUE)
   {
      std::cerr << "Failed to create minidump file at: " << dumpFilePath << std::endl;
      return false;
   }

   MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
   if (exceptionInfo)
   {
      dumpInfo.ThreadId = GetCurrentThreadId();
      dumpInfo.ExceptionPointers = exceptionInfo;
      dumpInfo.ClientPointers = TRUE;
   }

   BOOL success = MiniDumpWriteDump
   (
      GetCurrentProcess(),
      GetCurrentProcessId(),
      hFile,
      static_cast<MINIDUMP_TYPE>(dumpType),
      exceptionInfo ? &dumpInfo : nullptr,
      nullptr,
      nullptr
   );

   CloseHandle(hFile);

   if (success)
   {
      std::cout << "Minidump created at: " << dumpFilePath << std::endl;
      return true;
   }
   else
   {
      std::cerr << "Failed to write minidump." << std::endl;
      return false;
   }
}

LONG WINAPI ARC::UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo)
{
   DumpGenerator::MiniDump(exceptionInfo, DumpGenerator::DumpType::MiniDumpWithFullMemory, "");
   return EXCEPTION_EXECUTE_HANDLER;
}

void ARC::RegisterDumpHandler()
{
   SetUnhandledExceptionFilter(UnhandledExceptionHandler);
}
