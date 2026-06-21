#pragma once

// Runtime: the engine facade handed to plugins via EngineContext. Owns the substrate
// that MUST outlive plugin reloads -- the shared TypeContext (installed in THIS module),
// the persistent ComponentRegistry, the (swappable) Registry, the per-phase schedulers,
// the RunLoop, and the JobSystem. ARCANE_API: the plugin and the host both call it.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
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

        // --- input bridge ---
        // Latest per-frame input snapshot. The host (Loom) stores it each frame
        // via SetInputSnapshot; plugins read it via Input() in their update hooks.
        void                 SetInputSnapshot(const InputSnapshot& snap) noexcept;
        const InputSnapshot& Input() const noexcept;

        // --- ImGui handoff (ABI v2) ---
        // The host installs its ImGui context + allocators here (once, after creating
        // the ImGuiLayer); PluginHost copies them into the EngineContext so the plugin
        // can adopt the host's GImGui across the DLL boundary. Stored as void* to keep
        // this header imgui-include-free (ImGuiContext* / ImGuiMemAllocFunc / ...).
        // All null in a headless host (no ImGuiLayer) -> plugins skip the install.
        void  SetImGui(void* context, void* alloc, void* freeFn, void* userData) noexcept;
        void* ImGuiContext()  const noexcept;
        void* ImGuiAlloc()    const noexcept;
        void* ImGuiFree()     const noexcept;
        void* ImGuiUserData() const noexcept;

        // --- hot-reload support (plugin Save/LoadState + the host call these) ---
        std::vector<std::byte> SnapshotRegistry() const;          // Registry::Save() -> bytes

        // Swaps in a registry deserialized from bytes (3.3 Load keeps the workScheduler) and rebinds the
        // RunLoop. The SystemSchedulers are KEPT; the host clears + re-registers systems around a reload
        // (ClearSystems before the plugin's Init). Engine systems receive Registry& per Execute, so running
        // the kept schedulers against the swapped registry is safe.
        bool RestoreRegistry(std::span<const std::byte> bytes);

        // Swaps in a FRESH empty registry (same shared ComponentRegistry + scheduler) and rebinds the
        // RunLoop; SystemSchedulers are kept. Used for a fresh-boot reload so the plugin's Init rebuilds
        // its scene. Caller clears systems first (ClearSystems).
        void ResetRegistry();

        void ClearSystems();                                      // Clear() all three phase schedulers

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
