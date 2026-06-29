#include "Module.hpp"

#include <utility>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

Module::Module(std::filesystem::path path, NativeHandle handle) noexcept
    : m_path(std::move(path)), m_handle(handle)
{
}

Module::~Module()
{
    Unload();
}

Module::Module(Module&& other) noexcept
    : m_path(std::move(other.m_path)), m_handle(other.m_handle)
{
    other.m_handle = nullptr;
}

Module& Module::operator=(Module&& other) noexcept
{
    if (this != &other)
    {
        Unload();
        m_path = std::move(other.m_path);
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

std::optional<Module> Module::Load(std::filesystem::path path)
{
#if defined(_WIN32)
    NativeHandle handle = reinterpret_cast<NativeHandle>(::LoadLibraryW(path.c_str()));
#else
    NativeHandle handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle)
        return std::nullopt;

    return Module(std::move(path), handle);
}

void* Module::Symbol(const char* name) const noexcept
{
    if (!m_handle || !name)
        return nullptr;

#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
    return ::dlsym(m_handle, name);
#endif
}

void Module::Unload() noexcept
{
    if (!m_handle)
        return;

#if defined(_WIN32)
    ::FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
    ::dlclose(m_handle);
#endif
    m_handle = nullptr;
}
