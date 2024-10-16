#pragma once

#ifdef ARC_PLATFORM_WINDOWS
   #include "DbgHelp.h"
   #pragma comment(lib, "Dbghelp.lib")
#endif

namespace ARC
{
   namespace DumpGenerator
   {
#ifdef ARC_PLATFORM_WINDOWS
      enum class DumpType
      {
         MiniDumpNormal          =  MiniDumpNormal,
         MiniDumpWithDataSegs    =  MiniDumpWithDataSegs,
         MiniDumpWithFullMemory  =  MiniDumpWithFullMemory,
         MiniDumpWithHandleData  =  MiniDumpWithHandleData,
         MiniDumpWithThreadInfo  =  MiniDumpWithThreadInfo,
      };

      bool MiniDump(EXCEPTION_POINTERS* exceptionInfo = nullptr, DumpType dumpType = DumpType::MiniDumpWithFullMemory, const std::string& customPath = "");
#else
      enum class DumpType
      {
         MiniDumpNormal,
         MiniDumpWithDataSegs,
         MiniDumpWithFullMemory,
         MiniDumpWithHandleData,
         MiniDumpWithThreadInfo,
      };
      bool MiniDump(void* exceptionInfo = nullptr, DumpType dumpType = DumpType::MiniDumpWithFullMemory, const std::string& customPath = "");
#endif
   }

#ifdef ARC_PLATFORM_WINDOWS
   LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* exceptionInfo);
#endif

   void RegisterDumpHandler();
} 