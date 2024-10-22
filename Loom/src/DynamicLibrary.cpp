#include "arcpch.h"
#include "DynamicLibrary.h"

#include "ErrorCode.h"

DynamicLibrary::DynamicLibrary(const std::filesystem::path& path) :
   m_libraryPath(path),
   m_handle(nullptr)
{}

DynamicLibrary::~DynamicLibrary()
{
   Unload();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept :
   m_libraryPath(std::move(other.m_libraryPath)),
   m_handle(other.m_handle)
{
   other.m_handle = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
   if (this != &other)
   {
      Unload();
      m_libraryPath = std::move(other.m_libraryPath);
      m_handle = other.m_handle;
      other.m_handle = nullptr;
   }
   return *this;
}

ARC::Result<void> DynamicLibrary::Load()
{
   if (m_handle)
      ARC::Error::Create(LoomError::LibraryLoadFailure, "Library already loaded.");

#ifdef ARC_PLATFORM_WINDOWS
   m_handle = LoadLibraryW(m_libraryPath.wstring().c_str());
   if (!m_handle)
      ARC::Error::Create(LoomError::LibraryLoadFailure, "Error occured while loading: " + m_libraryPath.string());
#else
   m_handle = dlopen(m_libraryPath.c_str(), RTLD_LAZY);
   if (!m_handle)
      ARC::Error::Create(LoomError::LibraryLoadFailure, "Error occured while loading: " + m_libraryPath.string());
#endif
   return {};
}

ARC::Result<void> DynamicLibrary::Unload()
{
   if (m_handle)
   {
#ifdef ARC_PLATFORM_WINDOWS
      if (!FreeLibrary(m_handle))
         ARC::Error::Create(LoomError::LibraryUnloadFailure, "Failed to load library: " + m_libraryPath.string());
#else
      dlclose(m_handle);
#endif
      m_handle = nullptr;
   }

   return {};
}

ARC::Result<void*> DynamicLibrary::GetFunction(const std::string& name) const
{
   if (!m_handle)
      return ARC::Error::Create(LoomError::LibraryUnloadFailure, "Attempted to access function '" + name + "' in unloaded library " + m_libraryPath.string());

#ifdef ARC_PLATFORM_WINDOWS
   void* func = reinterpret_cast<void*>(GetProcAddress(m_handle, name.c_str()));
#else
   void* func = dlsym(m_handle, name.c_str());
#endif
   if (!func)
      return ARC::Error::Create(LoomError::LibraryUnloadFailure, "Failed to get function: " + name);

   return func;
}
