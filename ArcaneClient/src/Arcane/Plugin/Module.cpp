#include <Arcane/Plugin/Module.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace
{
    // Last load failure reason, for the diagnostic. Thread-local so a worker
    // load never clobbers the main thread's pending message.
    thread_local std::string t_lastLoadError;

#if defined(_WIN32)
    bool EqualsNoCase(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    bool StartsWithNoCase(std::string_view s, std::string_view prefix) noexcept
    {
        return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
    }

    // ANY of these in the import table marks the image Debug-CRT -- one debug
    // import poisons the whole module regardless of what else it links.
    constexpr std::string_view kDebugCrtImports[] = {
        "ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll",
    };
    // Positive release evidence (the ucrt also surfaces as api-ms-win-crt-*
    // apiset forwarders). Only consulted when no debug import was seen.
    constexpr std::string_view kReleaseCrtImports[] = {
        "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
    };
#endif
}

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

    const std::string& Module::LastLoadError() noexcept { return t_lastLoadError; }

    std::optional<Module> Module::Load(std::filesystem::path path)
    {
        t_lastLoadError.clear();
#if defined(_WIN32)
        NativeHandle handle = reinterpret_cast<NativeHandle>(::LoadLibraryW(path.c_str()));
        if (!handle)
        {
            const DWORD err = ::GetLastError();
            char buf[512] = {};
            ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
            t_lastLoadError = "error " + std::to_string(err) + ": " + buf;
        }
#else
        NativeHandle handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle)
        {
            const char* err = ::dlerror();
            t_lastLoadError = err ? err : "dlopen failed";
        }
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

    CrtFlavor Module::DetectCrtFlavorFromImage(const unsigned char* data, std::size_t size,
                                               std::string* matchedImport) noexcept
    {
#if defined(_WIN32)
        // Parses the FILE layout (RVAs resolved through section headers), not a
        // loaded image -- the whole point is a verdict before LoadLibrary. Every
        // read is bounds-checked; any inconsistency is Unknown, never a fault.
        const auto in = [&](std::size_t off, std::size_t n) noexcept
        { return data && off <= size && n <= size - off; };

        if (!in(0, sizeof(IMAGE_DOS_HEADER)))
            return CrtFlavor::Unknown;
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, data, sizeof dos);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return CrtFlavor::Unknown;

        const auto ntOff = static_cast<std::size_t>(dos.e_lfanew);
        if (!in(ntOff, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)))
            return CrtFlavor::Unknown;
        DWORD sig = 0;
        std::memcpy(&sig, data + ntOff, sizeof sig);
        if (sig != IMAGE_NT_SIGNATURE)
            return CrtFlavor::Unknown;
        IMAGE_FILE_HEADER fh{};
        std::memcpy(&fh, data + ntOff + sizeof(DWORD), sizeof fh);

        const std::size_t optOff = ntOff + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!in(optOff, fh.SizeOfOptionalHeader) || fh.SizeOfOptionalHeader < sizeof(WORD))
            return CrtFlavor::Unknown;
        WORD magic = 0;
        std::memcpy(&magic, data + optOff, sizeof magic);

        DWORD importRva = 0;
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
                return CrtFlavor::Unknown;
            IMAGE_OPTIONAL_HEADER64 oh{};
            std::memcpy(&oh, data + optOff, sizeof oh);
            if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
                return CrtFlavor::Unknown;
            importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
                return CrtFlavor::Unknown;
            IMAGE_OPTIONAL_HEADER32 oh{};
            std::memcpy(&oh, data + optOff, sizeof oh);
            if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
                return CrtFlavor::Unknown;
            importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        }
        else
            return CrtFlavor::Unknown;
        if (importRva == 0)
            return CrtFlavor::Unknown;   // no import table at all -- no verdict

        const std::size_t secOff = optOff + fh.SizeOfOptionalHeader;
        if (!in(secOff, static_cast<std::size_t>(fh.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER)))
            return CrtFlavor::Unknown;
        const auto rvaToOff = [&](DWORD rva) noexcept -> std::size_t
        {
            for (WORD i = 0; i < fh.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER sh{};
                std::memcpy(&sh, data + secOff + i * sizeof sh, sizeof sh);
                const DWORD span = std::max(sh.Misc.VirtualSize, sh.SizeOfRawData);
                if (rva >= sh.VirtualAddress && rva - sh.VirtualAddress < span)
                    return static_cast<std::size_t>(sh.PointerToRawData) + (rva - sh.VirtualAddress);
            }
            return SIZE_MAX;
        };

        std::string releaseMatch;
        // Hard iteration cap: a corrupt descriptor array must terminate anyway.
        for (std::size_t idx = 0; idx < 4096; ++idx)
        {
            const std::size_t dOff =
                rvaToOff(importRva + static_cast<DWORD>(idx * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (dOff == SIZE_MAX || !in(dOff, sizeof(IMAGE_IMPORT_DESCRIPTOR)))
                break;
            IMAGE_IMPORT_DESCRIPTOR desc{};
            std::memcpy(&desc, data + dOff, sizeof desc);
            if (desc.Name == 0)
                break;   // all-zero terminator (Name==0 is the canonical check)

            const std::size_t nOff = rvaToOff(desc.Name);
            if (nOff == SIZE_MAX || nOff >= size)
                continue;
            std::string dllName;
            for (std::size_t p = nOff; p < size && data[p] != 0 && dllName.size() < 256; ++p)
                dllName.push_back(static_cast<char>(data[p]));

            for (const std::string_view dbg : kDebugCrtImports)
                if (EqualsNoCase(dllName, dbg))
                {
                    if (matchedImport) *matchedImport = std::move(dllName);
                    return CrtFlavor::Debug;
                }
            if (releaseMatch.empty())
            {
                for (const std::string_view rel : kReleaseCrtImports)
                    if (EqualsNoCase(dllName, rel))
                        releaseMatch = dllName;
                if (releaseMatch.empty() && StartsWithNoCase(dllName, "api-ms-win-crt-"))
                    releaseMatch = dllName;
            }
        }
        if (!releaseMatch.empty())
        {
            if (matchedImport) *matchedImport = std::move(releaseMatch);
            return CrtFlavor::Release;
        }
        return CrtFlavor::Unknown;
#else
        (void)data; (void)size; (void)matchedImport;
        return CrtFlavor::Unknown;
#endif
    }

    CrtFlavor Module::ScanFileCrtFlavor(const std::filesystem::path& path,
                                        std::string* matchedImport) noexcept
    {
#if defined(_WIN32)
        try
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return CrtFlavor::Unknown;
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
            return DetectCrtFlavorFromImage(bytes.data(), bytes.size(), matchedImport);
        }
        catch (...)
        {
            return CrtFlavor::Unknown;   // I/O or allocation failure: no verdict
        }
#else
        (void)path; (void)matchedImport;
        return CrtFlavor::Unknown;
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
