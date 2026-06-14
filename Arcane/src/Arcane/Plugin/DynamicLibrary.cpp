#include <Arcane/Plugin/DynamicLibrary.hpp>

#if defined(_WIN32)
    #include <windows.h>   // Arcane.dll already defines WIN32_LEAN_AND_MEAN + NOMINMAX

namespace Arcane::Detail
{
    LibHandle DLOpen(const std::filesystem::path& path)
    {
        return reinterpret_cast<LibHandle>(::LoadLibraryW(path.c_str()));
    }
    void* DLSym(LibHandle handle, const char* name)
    {
        return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
    }
    void DLClose(LibHandle handle)
    {
        if (handle) ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
    }
}
#else
    #include <dlfcn.h>

namespace Arcane::Detail
{
    LibHandle DLOpen(const std::filesystem::path& path)
    {
        return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    void* DLSym(LibHandle handle, const char* name) { return ::dlsym(handle, name); }
    void  DLClose(LibHandle handle) { if (handle) ::dlclose(handle); }
}
#endif
