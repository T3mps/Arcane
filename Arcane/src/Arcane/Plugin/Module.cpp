#include <Arcane/Plugin/Module.hpp>

#include <utility>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace Arcane
{
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

    Module::ImageSpan Module::Image() const noexcept
    {
        if (!m_handle)
            return {};

#if defined(_WIN32)
        // On Windows an HMODULE IS the image base. SizeOfImage is read straight
        // out of the mapped PE headers rather than via GetModuleInformation so
        // this costs no psapi link. Both signatures are checked because a bad
        // read here would hand back a range that disowns the wrong module's
        // descriptors -- far worse than returning "unknown".
        const auto* base = reinterpret_cast<const unsigned char*>(m_handle);
        const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return {};
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return {};
        return ImageSpan{m_handle, static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage)};
#else
        // POSIX: dladdr/link_map would give this, but no host ships here yet.
        // Returning "unknown" makes callers skip disowning rather than guess.
        return {};
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
}
