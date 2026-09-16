#pragma once

#include <Arcane/Core/Api.hpp>
#include <Arcane/Plugin/PluginABI.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace Arcane
{
    class ProcessContext;
    class Runtime;

    // Watches a game DLL, loads versioned copies (PDB-lock dodge), checks the ABI,
    // and rolls back to the last-good image on any failure (never a lost session).
    //
    // N RUNTIMES, ONE MODULE IMAGE (Core-DLL split, spec docs/specs/2026-09-15-core-
    // dll-split-design.md s4/s5, plan 1 P9): a host attaches every world this module
    // serves. The FIRST attached is the PRIMARY -- the world handed to the module as
    // EngineContext::engine, the one whose SaveState/LoadState carries state across a
    // hot reload; every other attached Runtime is snapshot/restored through its
    // registry alone and repopulated from the module's re-registered system factories.
    // A PluginHost must NOT outlive its ProcessContext or any attached Runtime (the
    // dtor calls into them); DetachRuntime is the seam for a world that dies first.
    class ARCANE_CORE_API PluginHost
    {
    public:
        // sourceDllPath is the PRIMARY game module (watched + hot-reloaded). Pass an EMPTY
        // path for a plugins-only host: Load() then brings up only the AddPlugin() secondaries
        // (no primary, no hot-reload watch) -- used by the editor when a project has plugin
        // modules but no gameModule. Vtable()/IsLoaded() report no primary; the *All drivers
        // and Unload() already handle a primary-less host.
        //
        // `process` is the process's ONE ProcessContext: the module's TypeContext and
        // the system-factory table the module registers into. At least one Runtime
        // must be attached before Load().
        PluginHost(ProcessContext& process, std::filesystem::path sourceDllPath);
        ~PluginHost();

        PluginHost(const PluginHost&) = delete;
        PluginHost& operator=(const PluginHost&) = delete;

        // Attach a world this module serves. The FIRST attach is the PRIMARY. Attaching
        // to an already-loaded host instantiates the module's matching factories into
        // the new Runtime immediately, so a world can join mid-session.
        void AttachRuntime(Runtime& rt);
        // Detach a world: its module-registered systems are cleared and it is dropped.
        // Detaching the PRIMARY of a loaded host is refused (ARC_ERROR, no-op) -- it is
        // the module's own world; Unload() first.
        void DetachRuntime(Runtime& rt) noexcept;
        [[nodiscard]] std::span<Runtime* const> Runtimes() const noexcept;

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
