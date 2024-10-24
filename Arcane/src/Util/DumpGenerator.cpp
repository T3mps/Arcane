#include "arcpch.h"
#include "DumpGenerator.h"

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
         ARC_CORE_ERROR("Failed to create minidump file at: " + dumpFilePath);
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
         ARC_CORE_ERROR("Minidump created at: " + dumpFilePath);
         return true;
      }
      else
      {
         ARC_CORE_ERROR("Failed to write minidump.");
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
#else
   void SignalHandler(int32_t signum)
   {
      ARC_CORE_ERROR("Received signal: " + signum + ", creating core dump...");
      std::abort();  // Causes core dump to be generated
   }

   bool ARC::DumpGenerator::MiniDump(void* exceptionInfo, DumpType dumpType, const std::string& customPath)
   {
      ARC_CORE_ERROR("Linux does not support MiniDump. Use core dump or external tools like gdb for debugging.");
      return false;
   }

   void ARC::RegisterDumpHandler()
   {
      std::signal(SIGSEGV, SignalHandler);  // Segmentation faults
      std::signal(SIGABRT, SignalHandler);  // Aborts
   }

#endif
