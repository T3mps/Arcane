#include <Arcane/Base/Runtime.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>   // RegisterPhysicsComponents (engine roster)
#include <Arcane/Scene/SceneModule.hpp>         // RegisterSceneComponents   (engine roster)
#include <Arcane/Scene/SceneResources.hpp>   // RenderContext2D (instantiated IN this module)
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <optional>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Directory of the running executable, so exe-relative engine assets (here the
        // shipped EngineConfig/ defaults) resolve regardless of CWD -- mirrors the
        // exe-relative pattern in Assets.cpp / ShaderLibrary.cpp.
        std::filesystem::path ExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }

        // ASCII name for a SerializationError so a Save failure logs a readable
        // cause rather than an opaque integer (the enum has no library to_string).
        const char* SerializationErrorName(Astra::SerializationError e) noexcept
        {
            switch (e)
            {
                case Astra::SerializationError::None:               return "None";
                case Astra::SerializationError::InvalidMagic:       return "InvalidMagic";
                case Astra::SerializationError::UnsupportedVersion: return "UnsupportedVersion";
                case Astra::SerializationError::CorruptedData:      return "CorruptedData";
                case Astra::SerializationError::UnknownComponent:   return "UnknownComponent";
                case Astra::SerializationError::SizeMismatch:       return "SizeMismatch";
                case Astra::SerializationError::EndiannessMismatch: return "EndiannessMismatch";
                case Astra::SerializationError::ChecksumMismatch:   return "ChecksumMismatch";
                case Astra::SerializationError::IOError:            return "IOError";
                case Astra::SerializationError::OutOfMemory:        return "OutOfMemory";
            }
            return "Unknown";
        }
    }

    struct Runtime::Impl
    {
        JobSystem                                   jobs;
        std::shared_ptr<Mosaic::IWorkScheduler>     sched;
        std::unique_ptr<Astra::TypeContext>         ownedContext;   // null when an external one is injected
        Astra::TypeContext*                         context = nullptr;
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;   // reused by Restore/ResetRegistry when rebuilding the loop
        InputSnapshot                               input{};   // latest host-supplied snapshot; plugins read via Input()
        std::unique_ptr<Assets>                     assets;
        Config                                      config;          // layered engine+project config (Slice 3)
        std::filesystem::path                       engineConfigDir; // <exe>/EngineConfig (shipped defaults)
        std::optional<Project>                      project;   // open project (Slice 1b); empty = none
        Audio::AudioDeviceDesc                      audioDesc{};
        Audio::AudioDevice                          audio;

        // 2D camera the plugin drives (SetCamera) and the render bridge reads
        // (SetRenderContext writes it into RenderContext2D). Defaults to the
        // identity transform (offset (0,0), zoom 1).
        glm::vec2                                   cameraOffset{0.0f, 0.0f};
        float                                       cameraZoom = 1.0f;

        // ImGui cross-DLL handoff (v2): the host's context + allocators, forwarded
        // into the plugin's EngineContext by PluginHost. All null until the host
        // calls SetImGui (and in headless hosts that never create an ImGuiLayer).
        void* imguiContext  = nullptr;
        void* imguiAlloc    = nullptr;
        void* imguiFree     = nullptr;
        void* imguiUserData = nullptr;

        // Render-resources bridge: the host-owned nvrhi device + ShaderLibrary a plugin
        // may need to build its own engine render objects (e.g. an inspector's
        // OffscreenCanvas). Null until the host calls SetRenderResources (and in headless
        // hosts that never create a device). Non-owning: the host owns their lifetime.
        nvrhi::IDevice* device  = nullptr;
        ShaderLibrary*  shaders = nullptr;

        explicit Impl(Astra::TypeContext* external, bool enableAudioDevice) : jobs(), sched(jobs.WorkScheduler())
        {
            if (external) { context = external; }
            else { ownedContext = std::make_unique<Astra::TypeContext>(); context = ownedContext.get(); }

            // Install the shared context in THIS module BEFORE any TypeID/Registry use.
            Astra::SetTypeContext(context);
            components = std::make_shared<Astra::ComponentRegistry>();

            // The engine's OWN component roster, registered here so every host
            // has it before any plugin loads. Previously nothing registered it
            // outside tests: the live roster was whatever the hosted game
            // plugin happened to ReRegisterComponent<T>() in its Init, which
            // (a) left the editor's Add Component catalog offering only the
            // plugin's handful of types, and (b) silently DROPPED Identity /
            // Hidden when a runtime host loaded an editor-saved scene --
            // SceneSerializer skips a type that is reflected but not
            // REGISTERED as a component.
            //
            // Plugins now register only the types they themselves implement,
            // through their own RAII Astra::ComponentModule handle (see
            // PluginHost.cpp / HotReloadPlugin.cpp) -- the old "re-register
            // the roster after unload" host ritual is gone. The engine's OWN
            // roster deliberately stays on the PLAIN (anonymous, owner-less)
            // registration path below rather than ALSO going through its own
            // ComponentModule: a plugin's InstallOwned already shadows
            // WHATEVER is live for an id, anonymous or module-owned, and
            // restores that shadow on its own unload -- the anonymous
            // baseline gets the exact same "plugin overrides it, unload
            // restores it" behavior for free. Wrapping this roster in its own
            // ComponentModule was tried and reverted: ReleaseModule reports a
            // module's entries "cleared to empty" when it is the sole owner
            // within ITS OWN registry, which erases the type's TypeMeta from
            // the SHARED TypeContext (ComponentModule.hpp's own documented
            // scope caveat -- "one registry per context is the supported
            // shape"). Production only ever has one Runtime for the process's
            // life, so that never bites there, but the test suite runs MANY
            // short-lived Runtime instances (each with its own fresh
            // ComponentRegistry, by existing design) against ONE process-wide
            // TypeContext -- every one of those Runtimes tearing down its own
            // engine ComponentModule wiped the shared reflected meta out from
            // under every other registry/test using the same context, and a
            // follow-up attempt to keep that handle alive past the Runtime's
            // own lifetime (retiring it into a longer-lived container) traded
            // that bug for a cross-module static-destruction-order crash at
            // process exit. Anonymous registration was already correct and
            // is not module-owned, so nothing here is ever torn down early.
            //
            // ComponentID NUMBERING (corrected 2026-07-26 -- the previous comment
            // here claimed ids are resolved BY HASH and therefore order-
            // independent, which is FALSE): the type HASH is only the lookup key.
            // The id itself is a monotonic per-process counter assigned in
            // first-touch order -- TypeContext.hpp:93, `const ComponentID id =
            // m_next++`. Registration order fully determines numbering, and
            // registering the engine roster here DOES shift it (a plugin that
            // registered Transform + SpriteRenderer used to get 0,1; it now gets
            // 0,3). Ids are process-local; only the hash is stable across
            // processes.
            RegisterSceneComponents(*components);
            RegisterPhysicsComponents(*components);

            Astra::Registry::Config cfg;
            cfg.workScheduler = sched;
            registry   = std::make_unique<Astra::Registry>(components, cfg);
            schedulers = std::make_unique<SystemSchedulers>(sched);
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);

            // Headless gating for the real OS audio device. There is no headless signal
            // reachable here: the host-owned render device is wired via SetRenderResources
            // AFTER construction (always null now), and neither this ctor nor HostConfig
            // carried a headless flag. So the host states its intent through a ctor flag --
            // enableAudioDevice (default false). Tests, servers, tools, and the scripted
            // "ArcaneRuntime --frames N" GPU-verify leave it false and get the noDevice null backend;
            // an interactive host passes true. AudioDeviceDesc::enableDevice defaults false
            // for the same reason, so a real device is always opt-in.
            audioDesc.enableDevice = enableAudioDevice;
            assets = Assets::Create(nullptr);
            // Engine-default config layer (shipped beside the exe). A host with no
            // project still gets this base (e.g. input bindings for bare ArcaneRuntime);
            // OpenProject re-layers the project + user files on top.
            engineConfigDir = ExeDir() / "EngineConfig";
            config.LoadEngineDefaults(engineConfigDir);
            InitAudio();
        }

        ~Impl()
        {
            audio.Shutdown();
        }

        void InitAudio() noexcept
        {
            if (!assets)
                return;

            if (audio.Init(assets.get(), audioDesc))
                return;

            if (audioDesc.enableDevice)
            {
                ARC_WARN("Runtime: audio device init failed; falling back to null backend");
                audioDesc.enableDevice = false;
                if (audio.Init(assets.get(), audioDesc))
                    return;
            }

            ARC_WARN("Runtime: audio subsystem is unavailable");
        }

        void ResetAudio() noexcept
        {
            audio.Shutdown();
            InitAudio();
        }
    };

    Runtime::Runtime(Astra::TypeContext* externalContext, bool enableAudioDevice)
        : m_impl(std::make_unique<Impl>(externalContext, enableAudioDevice))
    {
        // Mosaic diagnostics: install the log sink + assert handler into THIS module
        // (Arcane.dll) so Astra/Manifold2D/Mosaic code running here routes to the
        // engine logger. Each module installs its own (per-module Mosaic storage).
        Arcane::Log::InstallMosaicSink();
        Arcane::Assert::InstallMosaicHandler();
    }
    Runtime::~Runtime() = default;   // do not reset the module slot: a later Runtime re-installs

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }
    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Mosaic::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
    ITaskExecutor*         Runtime::TaskExecutor()  noexcept { return m_impl->jobs.TaskExecutor(); }
    std::shared_ptr<Astra::ComponentRegistry> Runtime::Components() noexcept { return m_impl->components; }
    Assets& Runtime::AssetsFacade() noexcept { return *m_impl->assets; }
    Audio::AudioDevice& Runtime::AudioSystem() noexcept { return m_impl->audio; }

    void Runtime::SetInputSnapshot(const InputSnapshot& snap) noexcept { m_impl->input = snap; }
    const InputSnapshot& Runtime::Input() const noexcept { return m_impl->input; }

    void Runtime::SetImGui(void* context, void* alloc, void* freeFn, void* userData) noexcept
    {
        m_impl->imguiContext  = context;
        m_impl->imguiAlloc    = alloc;
        m_impl->imguiFree     = freeFn;
        m_impl->imguiUserData = userData;
    }
    void* Runtime::ImGuiContext()  const noexcept { return m_impl->imguiContext; }
    void* Runtime::ImGuiAlloc()    const noexcept { return m_impl->imguiAlloc; }
    void* Runtime::ImGuiFree()     const noexcept { return m_impl->imguiFree; }
    void* Runtime::ImGuiUserData() const noexcept { return m_impl->imguiUserData; }

    void Runtime::SetCamera(glm::vec2 offset, float zoom) noexcept
    {
        m_impl->cameraOffset = offset;
        m_impl->cameraZoom   = zoom;
    }
    glm::vec2 Runtime::CameraOffset() const noexcept { return m_impl->cameraOffset; }
    float     Runtime::CameraZoom()   const noexcept { return m_impl->cameraZoom; }

    void Runtime::SetRenderResources(nvrhi::IDevice* device, ShaderLibrary* shaders) noexcept
    {
        m_impl->device  = device;
        m_impl->shaders = shaders;
        // Bind the device into the Assets facade too: the facade is created
        // device-less in the ctor (the host-owned device does not exist yet),
        // so without this GetTexture would forever see a null device. SetDevice
        // is a no-op when the device is unchanged.
        if (m_impl->assets)
            m_impl->assets->SetDevice(device);
    }
    nvrhi::IDevice* Runtime::Device()  const noexcept { return m_impl->device; }
    ShaderLibrary*  Runtime::Shaders() const noexcept { return m_impl->shaders; }

    void Runtime::SetRenderContext(Batcher2D* batcher)
    {
        // SetResource<RenderContext2D> runs IN Arcane.dll so TypeID<RenderContext2D>
        // resolves here against the shared context; the host never touches a scene TypeID.
        // The camera comes from the STORED plugin camera (SetCamera) -- the host is
        // camera-agnostic. Defaults (offset (0,0), zoom 1) are the identity transform.
        // Epic 04.2: carry the render alpha so RenderSubmissionSystem +
        // DrawPhysicsDebug can interpolate poses between fixed steps. Runtime owns
        // the RunLoop, so this needs no plugin-ABI surface.
        m_impl->registry->SetResource<RenderContext2D>(
            RenderContext2D{batcher, m_impl->cameraOffset, m_impl->cameraZoom,
                            static_cast<float>(Loop().Alpha())});
    }

    void Runtime::SetSpriteMaterials(const std::unordered_map<Guid, std::uint16_t>* materials)
    {
        // Same rule as SetRenderContext: SetResource<SpriteMaterialTable> runs
        // IN Arcane.dll so the scene TypeID resolves against the shared context.
        m_impl->registry->SetResource<SpriteMaterialTable>(SpriteMaterialTable{materials});
    }

    void Runtime::SetSpriteTable(const std::unordered_map<Guid, SpriteEntry>* sprites)
    {
        // Same rule as SetRenderContext: SetResource<SpriteTable> runs IN
        // Arcane.dll so the scene TypeID resolves against the shared context.
        m_impl->registry->SetResource<SpriteTable>(SpriteTable{sprites});
    }

    Astra::Result<std::vector<std::byte>, Astra::SerializationError> Runtime::SnapshotRegistry() const
    {
        // A real Save failure must be named at its source: an empty-but-"ok"
        // snapshot would resurface much later as a generic "reload lost state"
        // with the root cause erased. FinishSnapshot propagates the exact
        // SerializationError; log it here so the hot-reload path names the cause.
        auto save = Serialization::FinishSnapshot(m_impl->registry->Save());
        if (save.IsErr())
        {
            const Astra::SerializationError err =
                save.GetError() ? *save.GetError() : Astra::SerializationError::None;
            ARC_ERROR("Runtime: SnapshotRegistry: registry Save failed ({})",
                      SerializationErrorName(err));
            return save;
        }

        // Astra's Save drops all resources; frame the serializable-resource
        // section (SceneRoot + any host/test-registered types) alongside the blob.
        std::vector<std::byte> section =
            Serialization::WriteResourceSection(*m_impl->registry, Serialization::SerializableResources());
        return Serialization::SnapshotResult::Ok(
            Serialization::FrameBytes(*save.GetValue(), section));
    }

    bool Runtime::RestoreRegistry(std::span<const std::byte> bytes)
    {
        // Split the frame into the registry blob + resource section. Transactional:
        // load into a local registry, apply resources, and only swap the live
        // registry + rebind the loop on FULL success, so a corrupt frame leaves
        // the running world untouched.
        auto frame = Serialization::ParseSnapshot(bytes);
        if (frame.IsErr())
            return false;

        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        auto r = Astra::Registry::Load(frame.GetValue()->registry, m_impl->components, cfg);   // 3.3 Config overload
        if (r.IsErr())
            return false;
        std::unique_ptr<Astra::Registry> loaded = std::move(*r.GetValue());

        auto resources = Serialization::ReadResourceSection(
            *loaded, frame.GetValue()->resources, Serialization::SerializableResources());
        if (resources.IsErr())
            return false;

        m_impl->registry = std::move(loaded);
        // Rebind the EXISTING loop to the swapped registry rather than recreating it:
        // a cached RunLoop* (a plugin that stored Loop() at init, a host toolbar) must
        // not dangle across a restore. Same observable loop state as a fresh loop.
        m_impl->loop->Rebind(*m_impl->registry);
        return true;
    }

    void Runtime::ResetRegistry()
    {
        // Fresh-boot reload: replace the registry with an empty one (same shared
        // ComponentRegistry + scheduler) so the plugin's Init rebuilds its scene.
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        m_impl->registry = std::make_unique<Astra::Registry>(m_impl->components, cfg);
        // Rebind the existing loop (keep the object stable so cached RunLoop* holders
        // do not dangle) -- see RestoreRegistry.
        m_impl->loop->Rebind(*m_impl->registry);
    }

    void Runtime::ClearSystems()
    {
        m_impl->schedulers->fixedUpdate.Clear();
        m_impl->schedulers->update.Clear();
        m_impl->schedulers->render.Clear();
    }

    void Runtime::ResetAudio() noexcept
    {
        m_impl->ResetAudio();
    }

    bool Runtime::OpenProject(const std::filesystem::path& pathOrFile, AssetRegistry::ScanProgressFn onProgress)
    {
        auto proj = Project::Open(pathOrFile, onProgress);
        if (!proj)
            return false;   // Project::Open already logged the cause

        // Engine/ABI binding: refuse a project built against a different engine ABI.
        // Belt-and-suspenders over PluginHost's own DLL-ABI gate -- this catches a
        // stale manifest before we even try to load the game module.
        if (proj->Manifest().engineAbi != static_cast<int>(kGamePluginABIVersion))
        {
            ARC_ERROR("Runtime::OpenProject: project '{}' targets engine ABI {} but this "
                      "engine is ABI {}", proj->Manifest().name,
                      proj->Manifest().engineAbi, static_cast<int>(kGamePluginABIVersion));
            return false;   // leave state untouched
        }

        m_impl->project = std::move(*proj);
        // Route loose-file content loads under the project's game:// mount (Content/).
        m_impl->assets->SetContentRoot(m_impl->project->Root() / "Content");
        // GUID loads resolve through THIS project's registry (Assets AssetId seam).
        // The raw pointer is safe: the optional's storage is stable, Runtime owns
        // both objects, and every OpenProject reinstalls the resolver.
        m_impl->assets->SetAssetResolver(
            [proj = &*m_impl->project](const AssetId& id) { return proj->ResolveAsset(id); });
        // Re-layer config: engine defaults (kept) + each enabled plugin's Config/ + this
        // project's Config/ + user overrides (Saved/Config/). Precedence engine -> plugins ->
        // project -> user (a project overrides the plugins it enables). Rebuild-from-defaults
        // so re-opening a project never accumulates a previous project's layers.
        m_impl->config.LoadEngineDefaults(m_impl->engineConfigDir);
        for (const auto& pluginRoot : m_impl->project->ActivePluginRoots())
            m_impl->config.LayerDir(pluginRoot / "Config");
        m_impl->config.LayerProject(m_impl->project->Root() / "Config",
                                    m_impl->project->Root() / "Saved" / "Config");
        return true;
    }

    const Project* Runtime::CurrentProject() const noexcept
    {
        return m_impl->project ? &*m_impl->project : nullptr;
    }

    void Runtime::CloseProject()
    {
        // Mirrors exactly what Impl's ctor leaves Assets/Config in (see above:
        // `assets = Assets::Create(nullptr)` installs no content root and no
        // resolver; `config.LoadEngineDefaults(engineConfigDir)` is the only
        // config call the ctor makes) -- so a project-less Runtime looks the
        // same whether it never opened a project or just closed one.
        m_impl->project.reset();
        m_impl->assets->SetContentRoot({});
        m_impl->assets->SetAssetResolver({});
        // Rebuild-from-defaults (LoadEngineDefaults clears m_categories first),
        // same call OpenProject makes before layering -- discards the plugin/
        // project/user layers entirely rather than leaving them shadowed by
        // nothing once nothing re-layers over them.
        m_impl->config.LoadEngineDefaults(m_impl->engineConfigDir);
    }

    std::optional<Guid> Runtime::RegisterCreatedAsset(const std::filesystem::path& file)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::RegisterCreatedAsset: no project open -- '{}' not registered",
                     file.generic_string());
            return std::nullopt;
        }
        return m_impl->project->RegisterAsset(file);
    }

    bool Runtime::SetProjectBootScene(const Guid& id)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::SetProjectBootScene: no project open -- boot scene not set");
            return false;
        }
        return m_impl->project->SetBootScene(id);
    }

    Config& Runtime::Configuration() noexcept
    {
        return m_impl->config;
    }
}
