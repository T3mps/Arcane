#include <Arcane/Base/Runtime.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Plugin/ClientHooks.hpp>   // IClientHooks -- the ONE Core->Client reach-back (plan 1 P6)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>          // Transform / .../MeshRenderer (engine roster types)
#include <Arcane/Scene/EngineRoster.hpp>        // EngineComponentRoster -- THE roster list (registered below, verified in ProjectHost.hpp)
#include <Arcane/Scene/PhysicsComponents.hpp>   // RigidBody2D/Collider2D/PhysicsBodyRef (engine roster types)
#include <Arcane/Scene/PhysicsSystem.hpp>       // PhysicsSystem/PhysicsResource (instantiated IN this module)
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot (ResolvedGravity's scene-root lookup)
#include <Arcane/Scene/TransformSystems.hpp>    // TransformPropagationSystem (engine-owned, instantiated IN this module)
#include <Arcane/Serialization/RegistrySnapshot.hpp>
#include <Arcane/Serialization/ResourceSerialization.hpp>
#include <Arcane/Sim/NetDriver.hpp>   // INetDriver::IsActive (the hot-reload refusal asks it)

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentModule.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <optional>
#include <tuple>
#include <utility>
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
        // Register the named roster into `m`. The list itself lives in
        // Scene/EngineRoster.hpp so this registration and ProjectHost.hpp's
        // VerifySharedTypeContext expand the SAME pack and cannot drift.
        template<typename... Ts>
        void RegisterRoster(Astra::ComponentModule& m, TypeList<Ts...>)
        {
            m.Register<Ts...>();
        }

        // Directory of the running executable, so exe-relative engine assets (here the
        // shipped data/EngineConfig defaults) resolve regardless of CWD -- mirrors the
        // exe-relative pattern in Assets.cpp / Render/ShaderPaths.cpp.
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
        Astra::TypeContext*                         context = nullptr;   // the ProcessContext's -- Runtime owns nothing
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::optional<Astra::ComponentModule>       engineModule;   // the engine roster's RAII owner; destructs AFTER registry (declared before it), BEFORE components
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;   // reused by Restore/ResetRegistry when rebuilding the loop
        std::unique_ptr<Assets>                     assets;
        Config                                      config;          // layered engine+project config (Slice 3)
        std::filesystem::path                       engineConfigDir; // <exe>/data/EngineConfig (shipped defaults)
        std::optional<Project>                      project;   // open project (Slice 1b); empty = none
        // The client seam (spec s2, plan 1 P6). Both null on a headless host; a
        // ClientRuntime sets them from its own ctor and clears them from its dtor,
        // so neither can outlive the object it points at.
        ClientRuntime*                              client = nullptr;
        IClientHooks*                               hooks  = nullptr;
        // The network role of THIS world + the process object whose factory table it
        // instantiates from (spec s4). `net` is the replication driver, non-owning.
        ProcessContext*                             process = nullptr;
        NetMode                                     mode    = NetMode::Standalone;
        INetDriver*                                 net     = nullptr;

        // `sharedComponents` non-null = a SECONDARY world built on the PRIMARY's
        // ComponentRegistry (spec s4; Runtime.hpp's three-argument ctor explains
        // why a per-world registry would break every module-defined type).
        Impl(ProcessContext& proc, NetMode netMode, std::shared_ptr<Astra::ComponentRegistry> sharedComponents)
            : jobs(), sched(jobs.WorkScheduler()), process(&proc), mode(netMode)
        {
            context = &proc.TypeContext();

            // Install the shared context in THIS module (ArcaneCore.dll) BEFORE any
            // TypeID/Registry use. ProcessContext::Create (Base/ProcessContext.cpp)
            // already installed exactly this slot -- it is Core's TU too -- so this
            // call is idempotent and kept only so the invariant is stated where the
            // Registry work begins. ArcaneClient.dll's own slot is ClientRuntime's
            // ctor's (Client/ClientRuntime.cpp).
            Astra::SetTypeContext(context, Astra::ModuleResidency::Resident);
            components = sharedComponents ? std::move(sharedComponents)
                                          : std::make_shared<Astra::ComponentRegistry>();

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
            // Plugins register only the types they themselves implement, through
            // their own RAII Astra::ComponentModule (PluginHost.cpp /
            // HotReloadPlugin.cpp). The engine's roster is module-owned too, since
            // 2026-09-11 (spec docs/specs/2026-09-11-astra-adoption-design.md s5),
            // through the Runtime-held handle below -- which REVERSES the
            // 2026-08-10 ratification that kept it anonymous. That ratification
            // rested on ComponentModule.hpp's old "one registry per context"
            // caveat: ReleaseModule erased a type's TypeMeta from the SHARED
            // TypeContext whenever the releasing registry was its sole owner, so
            // the test suite's many short-lived Runtimes (73 construction sites)
            // wiped the metas out from under each other, and retiring the handle
            // into a longer-lived container traded that for a static-destruction
            // crash at exit. The 2026-09-10 binder-stack vendor (ABI v24) removed
            // the caveat -- several registries per context is now the supported
            // shape -- and the residency declaration at SetTypeContext above is
            // what makes the module-owned roster SAFE here: Arcane.dll never
            // unmaps, so its binders are PINNED and every Reset reports Retained;
            // registry-less GetMeta keeps resolving after the last Runtime dies
            // (pinned by RuntimeTest.cpp's "[residency]" case). No plugin-side
            // static is involved: Impl is pimpl-held and reset from ~Runtime.
            //
            // A plugin's InstallOwned shadows whatever is live for an id,
            // module-owned or not, and its own unload restores the shadow -- the
            // "plugin overrides it, unload restores it" behaviour is unchanged.
            // RegisterSceneComponents / RegisterPhysicsComponents survive for the
            // tests that register on bare registries; Runtime no longer calls them.
            //
            // ComponentID NUMBERING (corrected 2026-07-26 -- the previous comment
            // here claimed ids are resolved BY HASH and therefore order-
            // independent, which is FALSE): the type HASH is only the lookup key.
            // The id itself is a monotonic per-process counter assigned in
            // first-touch order -- TypeContext.hpp:93, `const ComponentID id =
            // m_next++`. Registration order fully determines numbering, and
            // registering the engine roster here DOES shift it (a plugin that
            // registered Transform + SpriteRenderer used to get 0,1; it now gets
            // 0,2). Ids are process-local; only the hash is stable across
            // processes. (2026-09-11: the roster below dropped its per-entity
            // previous-pose slot -- Astra adoption plan 2 deleted the component
            // end to end -- so every id after it shifted down by one; the
            // worked example above already reflects the post-drop numbering.)
            //
            // A SECONDARY world sharing the primary's registry opens its OWN roster
            // handle here too, and that is deliberate, not an oversight. VERIFIED
            // against the vendored Astra: ComponentRegistry::OpenModuleId always
            // mints a fresh owner id, so the second handle's Register<Ts...> hits
            // InstallOwned's "live owner != owner" case (ComponentRegistry.hpp
            // semantics 4) -- the primary's identical entry is PUSHED onto the id's
            // shadow stack and one meta ref is acquired on the same binder. Both
            // descriptors come from ArcaneCore.dll, which never unmaps, so the
            // shadowed copy can never dangle; and ReleaseModule drops "this owner's
            // SHADOWED entries wherever they sit" and otherwise restores the newest
            // shadow, so the two handles may Reset in EITHER order (a secondary
            // outliving its primary included). Benign, and it keeps every Runtime's
            // teardown symmetric -- which is worth more than saving one shadow slot.
            engineModule.emplace(Astra::ComponentModule::Open(components, "Arcane"));
            ARC_ASSERT(*engineModule, "Runtime: ComponentModule::Open refused -- the slot above must be installed first");
            // The roster -- and the ORDER that is the id numbering -- is named
            // once, in Scene/EngineRoster.hpp, because ProjectHost.hpp's
            // VerifySharedTypeContext must check the same twelve types this
            // registers (2026-09-16: a one-type probe missed the editor's
            // early WorldTransform resolve). Ids are a first-touch counter, so
            // same order == same numbering as before.
            RegisterRoster(*engineModule, EngineComponentRoster{});

            Astra::Registry::Config cfg;
            cfg.workScheduler = sched;
            registry   = std::make_unique<Astra::Registry>(components, cfg);
            schedulers = std::make_unique<SystemSchedulers>(sched);
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);

            assets = Assets::Create();
            // Engine-default config layer (shipped beside the exe). A host with no
            // project still gets this base (e.g. input bindings for bare ArcaneRuntime);
            // OpenProject re-layers the project + user files on top.
            engineConfigDir = ExeDir() / "data" / "EngineConfig";
            config.LoadEngineDefaults(engineConfigDir);
            // The audio device that used to be initialized here is ClientRuntime's
            // (its RuntimePresentation member, initialized from its own ctor with
            // the enableAudioDevice flag that moved there with it).
        }
    };

    Runtime::Runtime(ProcessContext& process, NetMode mode)
        : Runtime(process, mode, nullptr) {}

    Runtime::Runtime(ProcessContext& process, NetMode mode,
                     std::shared_ptr<Astra::ComponentRegistry> sharedComponents)
        : m_impl(std::make_unique<Impl>(process, mode, std::move(sharedComponents)))
    {
        // Mosaic diagnostics: install the log sink + assert handler into THIS module
        // (ArcaneCore.dll) so Astra/Manifold2D/Mosaic code running here routes to the
        // engine logger. Each module installs its own (per-module Mosaic storage);
        // ClientRuntime's ctor does the same for ArcaneClient.dll.
        Arcane::Log::InstallMosaicSink();
        Arcane::Assert::InstallMosaicHandler();
        InstallEngineSystems();
        // ...and then whatever the LOADED module already registered, for THIS mode:
        // a Runtime built after the module loaded (the editor's embedded server
        // world, a second PIE world) must not come up system-less. Empty table when
        // no module is loaded -- the common case -- so this costs nothing.
        InstantiateModuleSystems();
    }
    Runtime::~Runtime() = default;   // do not reset the module slot: a later Runtime re-installs

    ProcessContext& Runtime::Process()      noexcept { return *m_impl->process; }
    NetMode         Runtime::Mode()   const noexcept { return m_impl->mode; }
    bool            Runtime::HasAuthority() const noexcept { return m_impl->mode != NetMode::Client; }

    void Runtime::SetNetDriver(INetDriver* d) noexcept { m_impl->net = d; }
    INetDriver* Runtime::NetDriver() const noexcept { return m_impl->net; }

    std::size_t Runtime::InstantiateModuleSystems()
    {
        return m_impl->process->SystemFactories().InstantiateInto(*m_impl->schedulers, m_impl->mode);
    }

    void Runtime::SetNetMode(NetMode m)
    {
        if (m == m_impl->mode)
            return;
        ARC_INFO("Runtime: net mode {} -> {}", ToString(m_impl->mode), ToString(m));
        m_impl->mode = m;
        // ClearSystems reinstalls the engine pair and fires OnSystemsCleared (the
        // client's presentation systems); the module's systems then come back for
        // the NEW mode only -- which is the whole point of the re-role.
        ClearSystems();
        InstantiateModuleSystems();
    }

    void Runtime::AttachClient(ClientRuntime* client, IClientHooks* hooks) noexcept
    {
        m_impl->client = client;
        m_impl->hooks  = hooks;
    }
    ClientRuntime* Runtime::Client()      const noexcept { return m_impl->client; }
    IClientHooks*  Runtime::ClientHooks() const noexcept { return m_impl->hooks; }

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }
    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Mosaic::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
    ITaskExecutor*         Runtime::TaskExecutor()  noexcept { return m_impl->jobs.TaskExecutor(); }
    JobSystem&             Runtime::Jobs()          noexcept { return m_impl->jobs; }
    std::shared_ptr<Astra::ComponentRegistry> Runtime::Components() noexcept { return m_impl->components; }
    Assets& Runtime::AssetsFacade() noexcept { return *m_impl->assets; }
    // The presentation surface -- AudioSystem/SetInputSnapshot/Input/SetImGui/
    // ImGui*/SetCamera/CameraOffset/CameraZoom/SetRenderContext/SetSpriteMaterials/
    // SetSpriteTable/SetMeshTable/SetMeshMaterials/ResetAudio -- stood here. It moved
    // to ClientRuntime (Client/ClientRuntime.cpp) BODY-FOR-BODY at the Core-DLL split
    // (plan 1 Task 4): every SetResource call still runs in ArcaneClient.dll, whose
    // Astra slot that ctor installs, so the "scene TypeID resolves against the shared
    // context" rule each of them documented is unchanged.
    //
    // SetRenderResources / Device() / Shaders() stood here too. The setter also bound
    // the device into the Assets facade (Assets::SetDevice) so GetTexture could
    // resolve a texture; both hosts have passed nullptr since Task 6, so that
    // bind has been a no-op and the facade stays device-less for its whole life.
    // Deleted at Task 9 -- ABI 14.

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

        // PhysicsResource/PhysicsInterpBuffer are transient, host-owned resources
        // that must never survive a save/load -- re-established fresh by the next
        // EnsurePhysics, same as WorldTransform is re-derived by propagation. Astra's
        // Registry::Load (format v2) now serializes EVERY resource whose type offers
        // a Serialize(Archive&) method -- both types provide a no-op one only to
        // satisfy that concept (their own doc comments explain why) -- so the loaded
        // registry carries a DEFAULT-CONSTRUCTED, world-less zombie of each unless
        // stripped here. EnsurePhysics's own `res && res->world` guard already treats
        // that zombie as absent, so this is belt-and-suspenders for any OTHER caller
        // that queries HasResource<PhysicsResource>() directly.
        loaded->RemoveResource<PhysicsResource>();
        loaded->RemoveResource<PhysicsInterpBuffer>();

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
        InstallEngineSystems();   // the module's systems are gone; the engine's are back
        // ...and the client's presentation systems with them: RenderSubmissionSystem
        // is ClientRuntime's to (re)install, and this is the ONE place that knows a
        // clear just happened. Null on a headless host -- nothing to reinstall.
        if (m_impl->hooks)
            m_impl->hooks->OnSystemsCleared();
    }

    void Runtime::InstallEngineSystems()
    {
        // The engine's HEADLESS pair, owned here (spec docs/specs/2026-09-13-
        // game-module-boilerplate-design.md s4.1) -- the UE/DOTS shape: the engine
        // ticks the world; a game module registers only its own systems and
        // places them with Astra::Before/After against these types. Each behind
        // its own HasSystem guard: AlreadyRegistered is the only failure and
        // this runs from the ctor AND after every ClearSystems. Order within a
        // scheduler: PhysicsSystem declares Before<TransformPropagationSystem>;
        // insertion order carries the rest (Astra's reorder is stable).
        // RenderSubmissionSystem was the third; it is presentation, so it is
        // ClientRuntime's now (Client/ClientRuntime.cpp installs it at
        // construction and on every OnSystemsCleared) and a Core-only host has
        // exactly the systems it can execute.
        auto& fixed  = m_impl->schedulers->fixedUpdate;
        if (!fixed.HasSystem<PhysicsSystem>())
        {
            const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
            std::ignore = fixed.AddSystem<PhysicsSystem>(fixedDt, /*stepWorld*/ true);
        }
        if (!fixed.HasSystem<TransformPropagationSystem>())
            std::ignore = fixed.AddSystem<TransformPropagationSystem>();
    }

    glm::vec2 Runtime::ResolvedGravity() const
    {
        glm::vec2 g = ProjectManifest::PhysicsConfig{}.gravity;
        if (m_impl->project)
            g = m_impl->project->Manifest().physics.gravity;
        if (const SceneRoot* sr = m_impl->registry->GetResource<SceneRoot>())
            if (const PhysicsSettings* ps = std::as_const(*m_impl->registry).GetComponent<PhysicsSettings>(sr->entity))
                g = ps->gravity;
        return g;
    }

    void Runtime::EnsurePhysics()
    {
        Astra::Registry& reg = *m_impl->registry;
        const glm::vec2 g = ResolvedGravity();
        PhysicsResource* res = reg.GetResource<PhysicsResource>();
        if (res && res->world)
        {
            const auto cur = res->world->Gravity();
            if (static_cast<float>(cur.x) == g.x && static_cast<float>(cur.y) == g.y)
                return;
            // Gravity changed: replace the world. Bodies re-mint from their
            // current Transforms on the next pass (PASS 1 sees every handle
            // invalid against the new world; PASS 2 self-heals).
        }
        Manifold2D::Physics::WorldDef wd;
        wd.gravityX = g.x;
        wd.gravityY = g.y;
        reg.SetResource(PhysicsResource{ std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd), {} });
        reg.SetResource(PhysicsInterpBuffer{});
    }

    void Runtime::PhysicsEditPass()
    {
        const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
        PhysicsSystem{ fixedDt, /*stepWorld*/ false }(*m_impl->registry);
    }

    void Runtime::ResetPhysics()
    {
        // The two-resource strip RestoreRegistry performs, on the LIVE registry:
        // the next EnsurePhysics sees neither and mints both. PASS 2 then
        // re-mints every body from its components -- and PASS 1/2 clear any
        // PhysicsBodyRef the fresh world does not track, so nothing here can
        // leave a handle behind for the fresh world to reissue to someone else.
        m_impl->registry->RemoveResource<PhysicsResource>();
        m_impl->registry->RemoveResource<PhysicsInterpBuffer>();
    }

    bool Runtime::OpenProject(const std::filesystem::path& pathOrFile, AssetRegistry::ScanProgressFn onProgress,
                              ProjectOpenOptions opts)
    {
        auto proj = Project::Open(pathOrFile, onProgress, opts);
        if (!proj)
            return false;   // Project::Open already logged the cause

        // Engine/ABI stamp check: WARN, never refuse. This was a hard refusal
        // ("belt-and-suspenders over the DLL gate"), and it is what locked
        // every host out of every existing project at each engine ABI bump --
        // the v10 bump made it bite everywhere at once. The manifest stamp
        // only describes what the game DLL was built against; the project's
        // DATA is ABI-agnostic, and the one dangerous act -- loading that
        // stale DLL -- is refused by the plugin ABI gate (Plugin.cpp), which
        // reports plugin.abi.mismatch with both versions and the fix. Host
        // strictness is a STAGE policy, not this function's: the runtime host
        // keeps plugin_load Fatal (a game host whose module cannot load still
        // refuses to boot), the editor keeps it Optional (the editor is where
        // the developer goes to FIX a stale project). The editor additionally
        // self-heals content-only stamps before this runs (EditorAppProject).
        if (proj->Manifest().engineAbi != static_cast<int>(kGamePluginABIVersion))
        {
            ARC_WARN("Runtime::OpenProject: project '{}' targets engine ABI {} but this "
                     "engine is ABI {} -- opening; its game module will be refused "
                     "until rebuilt", proj->Manifest().name,
                     proj->Manifest().engineAbi, static_cast<int>(kGamePluginABIVersion));
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

    bool Runtime::RestampProjectEngineAbi(int abi)
    {
        if (!m_impl->project)
        {
            ARC_WARN("Runtime::RestampProjectEngineAbi: no project open -- manifest not restamped");
            return false;
        }
        return m_impl->project->RestampEngineAbi(abi);
    }

    Config& Runtime::Configuration() noexcept
    {
        return m_impl->config;
    }
}
