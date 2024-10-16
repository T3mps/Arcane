#include "arcpch.h"
#include "ModuleInterface.h"

#include "Util/StringUtil.h"

std::mutex Module::moduleMutex;

LibraryHandle Module::Load(const std::wstring& moduleName, const char* funcName, EntryPointFunc& runGameFunc)
{
   std::lock_guard<std::mutex> lock(moduleMutex);

   LibraryHandle hModule = nullptr;

#ifdef ARC_PLATFORM_WINDOWS
   hModule = LoadLibrary(moduleName.c_str());

   if (!hModule)
   {
      ErrorCode error = GetLastError();
      ARC_CORE_ERROR(L"Failed to load " + moduleName + L", error code: " + ARC::StringUtil::ToWString(error) + L" (" + GetErrorMessage(error) + L")");
      return nullptr;
   }

   runGameFunc = (EntryPointFunc) GetProcAddress(hModule, ARC::StringUtil::ToString(funcName).c_str());
   if (!runGameFunc)
   {
      ErrorCode error = GetLastError();
      ARC_CORE_ERROR(L"Failed to find " + ARC::StringUtil::ToWString(funcName) + L" function in " + moduleName + L", error code: " + ARC::StringUtil::ToWString(error) + L" (" + GetErrorMessage(error) + L")");
      FreeLibrary(hModule);
      return nullptr;
   }
#else
   std::string libName(moduleName.begin(), moduleName.end());
   std::string funcNameStr(funcName.begin(), funcName.end());

   if (libName.substr(0, 3) != "lib")
      libName = "lib" + libName;
   if (libName.substr(libName.length() - 3) != ".so")
      libName += ".so";

   hModule = dlopen(libName.c_str(), RTLD_LAZY);
   if (!hModule)
   {
      const char* error = dlerror();
      std::cerr << "Failed to load " << libName << ": " << (error ? error : "Unknown error") << std::endl;
      return nullptr;
   }

   dlerror();

   void* symbol = dlsym(hModule, funcNameStr.c_str());
   const char* dlsymError = dlerror();
   if (dlsymError)
   {
      std::cerr << "Failed to find " << funcNameStr << " in " << libName << ": " << dlsymError << std::endl;
      dlclose(hModule);
      return nullptr;
   }

   runGameFunc = reinterpret_cast<EntryPointFunc>(symbol);
#endif

   return hModule;
}

void Module::Run(EntryPointFunc runGameFunc)
{
   try
   {
      runGameFunc();
   }
   catch (...)
   {
      ARC_CORE_ERROR(L"Exception occurred in client DLL.");
   }
}

void Module::Unload(LibraryHandle hModule)
{
   std::lock_guard<std::mutex> lock(moduleMutex);

   if (hModule)
   {
#ifdef ARC_PLATFORM_WINDOWS
      FreeLibrary(hModule);
#else
      dlclose(hModule);
#endif
   }
}

std::wstring Module::GetErrorMessage(ErrorCode errorCode)
{
#ifdef ARC_PLATFORM_WINDOWS
   LPWSTR messageBuffer = nullptr;
   size_t size = FormatMessageW
   (
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPWSTR>(&messageBuffer), 0, NULL
   );
   std::wstring message(messageBuffer, size);
   LocalFree(messageBuffer);
   return message;
#else
   const char* errorMsg = dlerror();
   if (!errorMsg)
      errorMsg = "Unknown error";
   std::string errorStr(errorMsg);
   return std::wstring(errorStr.begin(), errorStr.end());
#endif
}
