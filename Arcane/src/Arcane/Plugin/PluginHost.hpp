#pragma once

#include <Arcane/Base/Api.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>

namespace Arcane
{
    class Runtime;

    // Watches a game DLL, loads versioned copies (PDB-lock dodge), checks the ABI,
    // and rolls back to the last-good image on any failure (never a lost session).
    // Holds a Runtime& -- a PluginHost must NOT outlive its Runtime (the dtor calls into it).
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr<Impl> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API PluginHost
    {
    public:
        PluginHost(Runtime& runtime, std::filesystem::path sourceDllPath);
        ~PluginHost();

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        bool Load();                       // initial load: copy+load+ABI+Init (no state restore)
        void Unload();                     // Shutdown + ClearSystems + ResetRegistry + FreeLibrary + delete copies
        bool Reload(bool restoreState);    // full sequence; rollback on any failure
        void Poll();                       // debounced mtime watch -> Reload(true) on a stable change

        bool ForceReload() { return Reload(true);  }
        bool ReloadFresh() { return Reload(false); }

        bool                IsLoaded()   const noexcept;
        const PluginVTable* Vtable()     const noexcept;
        uint32_t            Generation() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
