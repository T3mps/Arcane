#pragma once

#ifdef ARC_PLATFORM_WINDOWS
   #include "windows.h"
#else
   #include "dlfcn.h"
#endif

#include "Core/Result.h"

class DynamicLibrary
{
public:
#ifdef ARC_PLATFORM_WINDOWS
   using Handle = HMODULE;
#else
   using Handle = void*;
#endif

   explicit DynamicLibrary(const std::filesystem::path& path);
   ~DynamicLibrary();

   DynamicLibrary(const DynamicLibrary&) = delete;
   DynamicLibrary& operator=(const DynamicLibrary&) = delete;

   DynamicLibrary(DynamicLibrary&& other) noexcept;
   DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

   explicit operator bool() const noexcept { return IsLoaded(); }

   ARC::Result<void> Load();
   ARC::Result<void> Unload();
   ARC::Result<void*> GetFunction(const std::string& name) const;

   bool IsLoaded() const { return m_handle != nullptr; }

private:
   std::filesystem::path m_libraryPath;
   Handle m_handle;
};
