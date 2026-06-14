#pragma once

// Runtime: the engine facade handed to plugins via EngineContext. Owns the substrate
// that MUST outlive plugin reloads -- the shared TypeContext (installed in THIS module),
// the persistent ComponentRegistry, the (swappable) Registry, the per-phase schedulers,
// the RunLoop, and the JobSystem. ARCANE_API: the plugin and the host both call it.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace Astra { class Registry; class ComponentRegistry; class TypeContext; class IWorkScheduler; }

namespace Arcane
{
    class Batcher2D;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr<Impl> member on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARCANE_API Runtime
    {
    public:
        // externalContext == null: Runtime creates+owns a TypeContext (production: Loom).
        // externalContext != null: install+use the caller's context so multiple modules
        // (a test exe + Arcane.dll + the plugin) share one component-ID space.
        explicit Runtime(Astra::TypeContext* externalContext = nullptr);
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // --- substrate the plugin registers into / the host drives ---
        Astra::Registry&        Registry()      noexcept;
        SystemSchedulers&       Schedulers()    noexcept;
        RunLoop&                Loop()          noexcept;
        Astra::TypeContext*     TypeContext()   noexcept;
        Astra::IWorkScheduler*  WorkScheduler() noexcept;
        std::shared_ptr<Astra::ComponentRegistry> Components() noexcept;

        // --- render bridge: the host sets the live batcher each frame, IN this module ---
        void SetRenderContext(Batcher2D* batcher, glm::vec2 cameraOffset);

        // --- hot-reload support (plugin Save/LoadState + the host call these) ---
        std::vector<std::byte> SnapshotRegistry() const;          // Registry::Save() -> bytes
        bool RestoreRegistry(std::span<const std::byte> bytes);   // Load(Config{sched}); swap; rebind RunLoop
        void ResetRegistry();                                     // swap in a fresh empty registry (fresh-boot reload)
        void ClearSystems();                                      // Clear() all three phase schedulers

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
