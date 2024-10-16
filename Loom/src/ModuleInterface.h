// ModuleInterface.h
#pragma once

#ifdef ARC_PLATFORM_WINDOWS
   #include <Windows.h>
   typedef HMODULE LibraryHandle;
   typedef DWORD ErrorCode;
#else
   #include <dlfcn.h>
   typedef void* LibraryHandle;
   typedef int ErrorCode;
#endif


namespace Module
{
   typedef void (*EntryPointFunc)();
   
   LibraryHandle Load(const std::wstring& moduleName, const char* funcName, EntryPointFunc& runGameFunc);
   void Run(EntryPointFunc runGameFunc);
   void Unload(LibraryHandle hModule);
   std::wstring GetErrorMessage(ErrorCode errorCode);
   
   extern std::mutex moduleMutex;
}


