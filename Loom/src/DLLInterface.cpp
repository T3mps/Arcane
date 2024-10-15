#include "arcpch.h"
#include "DLLInterface.h"

#include "Util/StringUtil.h"

std::mutex dllMutex;

HMODULE LoadDLL(const std::wstring& dllName, const std::wstring& funcName, EntryPointFunc& runGameFunc)
{
   std::lock_guard<std::mutex> lock(dllMutex);

   HMODULE hModule = LoadLibrary(dllName.c_str());
   if (!hModule)
   {
      DWORD error = GetLastError();
      ARC_CORE_ERROR(L"Failed to load " + dllName + L", error code: " + ARC::StringUtil::ToWString(error) + L" (" + GetErrorMessage(error) + L")");
      return nullptr;
   }

   runGameFunc = (EntryPointFunc) GetProcAddress(hModule, ARC::StringUtil::ToString(funcName).c_str());
   if (!runGameFunc)
   {
      DWORD error = GetLastError();
      ARC_CORE_ERROR(L"Failed to find " + funcName + L" function in " + dllName + L", error code: " + ARC::StringUtil::ToWString(error) + L" (" + GetErrorMessage(error) + L")");
      FreeLibrary(hModule);
      return nullptr;
   }

   return hModule;
}

void RunDLL(EntryPointFunc runGameFunc, HINSTANCE hInstance, int32_t nCmdShow)
{
   try
   {
      runGameFunc(hInstance, nCmdShow);

      MSG msg = { 0 };
      while (GetMessage(&msg, NULL, 0, 0))
      {
         TranslateMessage(&msg);
         DispatchMessage(&msg);

         if (msg.message == WM_QUIT)
            break;
      }

      while (true)
      {
         int result = GetMessage(&msg, NULL, 0, 0);
         if (result > 0)
         {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
         }
         else if (result == WM_QUIT)
         {
            break;
         }
         else
         {
            DWORD error = GetLastError();
            ARC_CORE_ERROR(L"GetMessage failed with error: " + ARC::StringUtil::ToWString(error) + L" (" + GetErrorMessage(error) + L")");
            break;
         }
      }
   }
   catch (...)
   {
      ARC_CORE_ERROR(L"Exception occurred in client DLL.");
   }
}

void UnloadDLL(HMODULE hModule)
{
   std::lock_guard<std::mutex> lock(dllMutex);

   if (hModule)
      FreeLibrary(hModule);
}

std::wstring GetErrorMessage(DWORD errorCode)
{
   LPWSTR messageBuffer = nullptr;
   size_t size = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&messageBuffer, 0, NULL);
   std::wstring message(messageBuffer, size);
   LocalFree(messageBuffer);
   return message;
}
