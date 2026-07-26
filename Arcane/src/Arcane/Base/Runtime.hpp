#pragma once

// Runtime: the engine facade handed to plugins via EngineContext. Owns the substrate
// that MUST outlive plugin reloads -- the shared TypeContext (installed in THIS module),
// the persistent ComponentRegistry, the (swappable) Registry, the per-phase schedulers,
// the RunLoop, and the JobSystem. ARCANE_API: the plugin and the host both call it.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Core/Result.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace nvrhi { class IDevice; }
namespace Astra { class Registry; class ComponentRegistry; class TypeContext; }
namespace Mosaic { struct IWorkScheduler; }   // the shared data-parallel seam (Astra aliases this)

namespace Arcane
{
    class Assets;
    struct ITaskExecutor;
    class Batcher2D;
    class ShaderLibrary;
    class Project;
    class Config;
    namespace Audio { class AudioDevice; }

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
        //
        // NO DEFAULT on externalContext, deliberately. This ctor resolves ten
        // TypeID statics while registering the engine roster, so the FIRST Runtime
        // in a process permanently pins Arcane.dll's component-ID numbering. A
        // test that constructed a bare `Runtime rt;` would install an UNSHARED
        // context and every later Edit:: op would silently report 0 changes --
        // the failure recorded in the ArcaneTests TypeContext-theft note. Every
        // caller already passes a context explicitly; making it required means the
        // compiler enforces that instead of convention.
        //
        // enableAudioDevice: opt into a real OS audio device. Defaults false (headless:
        // tests/servers/tools and the scripted "Loom --frames N" verify use the null
        // backend). An interactive host passes true; the real->null fallback still applies.
        explicit Runtime(Astra::TypeContext* externalContext, bool enableAudioDevice = false);
        ~Runtime();

        Runtime(const Runtime&) = delete;
        Runtime& operator=(const Runtime&) = delete;

        // --- substrate the plugin registers into / the host drives ---
        Astra::Registry&        Registry()      noexcept;
        SystemSchedulers&       Schedulers()    noexcept;
        RunLoop&                Loop()          noexcept;
        Astra::TypeContext*     TypeContext()   noexcept;
        Mosaic::IWorkScheduler* WorkScheduler() noexcept;
        ITaskExecutor*          TaskExecutor()  noexcept;   // enki pool, worker-index ParallelFor face
        std::shared_ptr<Astra::ComponentRegistry> Components() noexcept;
        Assets&                 AssetsFacade() noexcept;
        Config&                 Configuration() noexcept;   // layered engine+project config (Slice 3)
        Audio::AudioDevice&     AudioSystem() noexcept;

        // --- project (Slice 1b) ---
        // Open a project folder or .arcproj: validate-then-commit. On success the
        // Project is adopted and the Assets facade's content root is set to the
        // project's game:// mount (root/Content); returns false and leaves ALL state
        // untouched on a missing/invalid manifest OR an engineAbi that does not match
        // this engine (kGamePluginABIVersion). Both hosts open a project through here.
        bool OpenProject(const std::filesystem::path& pathOrFile);

        // The open project, or nullptr when none is open (no-project fallback mode).
        const Project* CurrentProject() const noexcept;

        // Register an editor-created asset file with the open project's registry
        // (Project::RegisterAsset). Idempotent. nullopt when no project is open or
        // the file lies outside every content root.
        std::optional<Guid> RegisterCreatedAsset(const std::filesystem::path& file);

        // --- render bridge: the host sets the live batcher each frame, IN this module ---
        // SetRenderContext writes RenderContext2D using the STORED camera (offset+zoom),
        // so the PLUGIN owns the camera (via SetCamera) and the host stays camera-agnostic.
        void SetRenderContext(Batcher2D* batcher);

        // Publish the sprite-material resolution map (Guid -> Batcher2D material
        // id, owned by the host's SpriteMaterialCache) into the registry's
        // SpriteMaterialTable resource. Runs IN this module so the scene TypeID
        // resolves against the shared context (SetRenderContext's rule). Null
        // clears the table (sprites fall back to the plain pipeline).
        void SetSpriteMaterials(const std::unordered_map<Guid, std::uint16_t>* materials);

        // --- render-resources bridge: device + shader library the host owns ---------
        // The plugin reaches the engine ONLY through this Runtime, but the nvrhi device
        // and the ShaderLibrary are created + owned by the host (Loom's main). A plugin
        // that needs to build its OWN engine render objects (e.g. an OffscreenCanvas for
        // a Minkowski-inset inspector) gets them here. The host calls SetRenderResources
        // ONCE after creating the device + shaders (before the plugin loads); both stay
        // null in a headless host (no device) so a plugin must null-check Device() before
        // creating GPU resources. Same homogenized contract as SetImGui / SetRenderContext.
        void            SetRenderResources(nvrhi::IDevice* device, ShaderLibrary* shaders) noexcept;
        nvrhi::IDevice* Device()  const noexcept;   // null in a headless host
        ShaderLibrary*  Shaders() const noexcept;   // null in a headless host

        // --- camera bridge: the plugin drives the 2D camera; the render bridge reads it ---
        // CANONICAL transform (matches Sandbox::Camera::WorldToScreen): screen = world * zoom + offset.
        // Defaults (offset (0,0), zoom 1) are the identity transform. RenderSubmissionSystem +
        // DrawPhysicsDebug apply the SAME camera so sprites + the debug overlay move together.
        void      SetCamera(glm::vec2 offset, float zoom) noexcept;
        glm::vec2 CameraOffset() const noexcept;
        float     CameraZoom()   const noexcept;

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
        // Registry::Save() -> framed snapshot bytes. Returns a Result so a Save
        // failure surfaces as an actionable error at the call site rather than an
        // empty-but-"ok" vector that masks data loss as a later reload failure.
        Astra::Result<std::vector<std::byte>, Astra::SerializationError> SnapshotRegistry() const;

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
        void ResetAudio() noexcept;                               // Drop plugin-created audio handles on reload

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
