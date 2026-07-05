#include <Arcane/Base/Runtime.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Audio/AudioDevice.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Jobs/TaskExecutor.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // RenderContext2D (instantiated IN this module)
#include <Arcane/Serialization/RegistrySnapshot.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/TypeContext.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Serialization/SerializationError.hpp>

namespace Arcane
{
    namespace
    {
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
        std::shared_ptr<Astra::IWorkScheduler>      sched;
        std::unique_ptr<Astra::TypeContext>         ownedContext;   // null when an external one is injected
        Astra::TypeContext*                         context = nullptr;
        std::shared_ptr<Astra::ComponentRegistry>   components;
        std::unique_ptr<Astra::Registry>            registry;
        std::unique_ptr<SystemSchedulers>           schedulers;
        std::unique_ptr<RunLoop>                    loop;
        RunLoop::Config                             loopCfg;   // reused by Restore/ResetRegistry when rebuilding the loop
        InputSnapshot                               input{};   // latest host-supplied snapshot; plugins read via Input()
        std::unique_ptr<Assets>                     assets;
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

            Astra::Registry::Config cfg;
            cfg.workScheduler = sched;
            registry   = std::make_unique<Astra::Registry>(components, cfg);
            schedulers = std::make_unique<SystemSchedulers>(sched);
            loop       = std::make_unique<RunLoop>(*registry, *schedulers, loopCfg);

            // Headless gating for the real OS audio device. There is no headless signal
            // reachable here: the host-owned render device is wired via SetRenderResources
            // AFTER construction (always null now), and neither this ctor nor LoomConfig
            // carried a headless flag. So the host states its intent through a ctor flag --
            // enableAudioDevice (default false). Tests, servers, tools, and the scripted
            // "Loom --frames N" GPU-verify leave it false and get the noDevice null backend;
            // an interactive host passes true. AudioDeviceDesc::enableDevice defaults false
            // for the same reason, so a real device is always opt-in.
            audioDesc.enableDevice = enableAudioDevice;
            assets = Assets::Create(nullptr);
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
        : m_impl(std::make_unique<Impl>(externalContext, enableAudioDevice)) {}
    Runtime::~Runtime() = default;   // do not reset the module slot: a later Runtime re-installs

    Astra::Registry&  Runtime::Registry()   noexcept { return *m_impl->registry; }
    SystemSchedulers& Runtime::Schedulers() noexcept { return *m_impl->schedulers; }
    RunLoop&          Runtime::Loop()       noexcept { return *m_impl->loop; }
    Astra::TypeContext*    Runtime::TypeContext()   noexcept { return m_impl->context; }
    Astra::IWorkScheduler* Runtime::WorkScheduler() noexcept { return m_impl->sched.get(); }
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
        m_impl->registry->SetResource<RenderContext2D>(
            RenderContext2D{batcher, m_impl->cameraOffset, m_impl->cameraZoom});
    }

    Astra::Result<std::vector<std::byte>, Astra::SerializationError> Runtime::SnapshotRegistry() const
    {
        // A real Save failure must be named at its source: an empty-but-"ok"
        // snapshot would resurface much later as a generic "reload lost state"
        // with the root cause erased. FinishSnapshot propagates the exact
        // SerializationError; log it here so the hot-reload path names the cause.
        auto snap = Serialization::FinishSnapshot(m_impl->registry->Save());
        if (snap.IsErr())
        {
            const Astra::SerializationError err =
                snap.GetError() ? *snap.GetError() : Astra::SerializationError::None;
            ARC_ERROR("Runtime: SnapshotRegistry: registry Save failed ({})",
                      SerializationErrorName(err));
        }
        return snap;
    }

    bool Runtime::RestoreRegistry(std::span<const std::byte> bytes)
    {
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        auto r = Astra::Registry::Load(bytes, m_impl->components, cfg);   // 3.3 Config overload
        if (r.IsErr())
            return false;
        m_impl->registry = std::move(*r.GetValue());
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
        return true;
    }

    void Runtime::ResetRegistry()
    {
        // Fresh-boot reload: replace the registry with an empty one (same shared
        // ComponentRegistry + scheduler) so the plugin's Init rebuilds its scene.
        Astra::Registry::Config cfg;
        cfg.workScheduler = m_impl->sched;
        m_impl->registry = std::make_unique<Astra::Registry>(m_impl->components, cfg);
        m_impl->loop = std::make_unique<RunLoop>(*m_impl->registry, *m_impl->schedulers, m_impl->loopCfg);
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
}
