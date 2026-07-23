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
    class ARCANE_API PluginHost
    {
    public:
        PluginHost(Runtime& runtime, std::filesystem::path sourceDllPath);
        ~PluginHost();

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        // Register a SECONDARY plugin module (a project Plugins/<name>/Binaries DLL) to
        // load alongside the primary game module. Call BEFORE Load(). Load order = call
        // order. Secondaries share the Runtime, load once (no independent hot-reload), are
        // torn down with the primary, and are re-established across a primary hot-reload.
        void AddPlugin(std::filesystem::path dll);

        bool Load();                       // initial load: copy+load+ABI+Init primary, then secondaries
        void Unload();                     // Shutdown + ClearSystems + ResetRegistry + unload + delete copies
        bool Reload(bool restoreState);    // full sequence; rollback on any failure (re-establishes secondaries)
        void Poll();                       // debounced mtime watch -> Reload(true) on a stable change

        bool ForceReload() { return Reload(true);  }
        bool ReloadFresh() { return Reload(false); }

        // Drive an entry point across the primary module AND every loaded secondary
        // (primary first). Each call is null-guarded, so a disengaged primary or a plugin
        // that omits DrawUI is simply skipped. A multi-module host calls these instead of
        // Vtable()->FixedUpdate/Update/DrawUI directly.
        void FixedUpdateAll(double dt);
        void UpdateAll(double dt, double alpha);
        void DrawUIAll();

        [[nodiscard]] bool                IsLoaded()   const noexcept;
        [[nodiscard]] const PluginVTable* Vtable()     const noexcept;
        [[nodiscard]] std::uint32_t       Generation() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
