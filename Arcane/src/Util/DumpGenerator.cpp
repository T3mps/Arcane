#include "arcpch.h"
#include "DumpGenerator.h"

#include <ctime>
#include <csignal>

namespace { std::mutex dumpMutex; }

std::string GetDefaultDumpFileName()
{
   auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
   std::tm timeInfo;
#ifdef ARC_PLATFORM_WINDOWS
   localtime_s(&timeInfo, &now);
#else
   localtime_r(&now, &timeInfo);
#endif

   std::ostringstream fileName;
   fileName << "arcane_" << std::put_time(&timeInfo, "%Y-%m-%d_%H-%M-%S") << ".dmp";
   return fileName.str();
}

#ifdef ARC_PLATFORM_WINDOWS

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

      BOOL success = MiniDumpWriteDump(
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
      ARC::DumpGenerator::MiniDump(exceptionInfo, ARC::DumpGenerator::DumpType::MiniDumpWithFullMemory, "");
      return EXCEPTION_EXECUTE_HANDLER;
   }

   void ARC::RegisterDumpHandler()
   {
      SetUnhandledExceptionFilter(UnhandledExceptionHandler);
   }

#else // Linux implementation

   void SignalHandler(int signum)
   {
      std::cerr << "Received signal: " << signum << ", creating core dump..." << std::endl;
      std::abort();  // Causes core dump to be generated
   }

   bool ARC::DumpGenerator::MiniDump(void* /* exceptionInfo */, DumpType /* dumpType */, const std::string& /* customPath */)
   {
      std::cerr << "Linux does not support MiniDump. Use core dump or external tools like gdb for debugging." << std::endl;
      return false;
   }

   void ARC::RegisterDumpHandler()
   {
      std::signal(SIGSEGV, SignalHandler);  // Handle segmentation faults
      std::signal(SIGABRT, SignalHandler);  // Handle aborts
   }

#endif
