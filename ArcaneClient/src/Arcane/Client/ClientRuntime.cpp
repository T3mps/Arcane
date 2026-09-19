#include <Arcane/Client/ClientRuntime.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Plugin/PluginABI.hpp>
#include <Arcane/Render/RenderSystems.hpp>       // RenderSubmissionSystem -- instantiated IN this module
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <imgui.h>

#include <tuple>

namespace Arcane
{
    ClientRuntime::ClientRuntime(ProcessContext& process, bool enableAudioDevice)
        : m_core(process)
    {
        // THIS module's (ArcaneClient.dll's) Astra slot + Mosaic routing -- the same
        // three installs Runtime's ctor performed when it lived here. Resident: this
        // DLL never unmaps either.
        Astra::SetTypeContext(&process.TypeContext(), Astra::ModuleResidency::Resident);
        Log::InstallMosaicSink();
        Assert::InstallMosaicHandler();
        m_core.AttachClient(this, this);
        InstallRenderSystems();
        // Device-less gating for the real OS audio device. There is no device-less
        // signal reachable here, so the host states its intent through a ctor flag --
        // enableAudioDevice (default false). Tests, servers, tools, and the scripted
        // "ArcaneRuntime --frames N" GPU-verify leave it false and get the noDevice
        // null backend; an interactive host passes true. AudioDeviceDesc::enableDevice
        // defaults false for the same reason, so a real device is always opt-in.
        m_pres.InitAudio(&m_core.AssetsFacade(), enableAudioDevice);
    }

    // Detach BEFORE either member destructs: Core must not hold a hooks pointer into
    // a half-destroyed client (its Runtime member is torn down right after this body).
    ClientRuntime::~ClientRuntime() { m_core.AttachClient(nullptr, nullptr); }

    void ClientRuntime::InstallRenderSystems()
    {
        auto& render = m_core.Schedulers().render;
        if (!render.HasSystem<RenderSubmissionSystem>())
            std::ignore = render.AddSystem<RenderSubmissionSystem>();
    }

    // --- IClientHooks ------------------------------------------------------------
    void* ClientRuntime::SaveUiContext() noexcept              { return ImGui::GetCurrentContext(); }
    void  ClientRuntime::RestoreUiContext(void* s) noexcept    { ImGui::SetCurrentContext(static_cast<::ImGuiContext*>(s)); }
    void  ClientRuntime::OnModuleTeardown() noexcept           { ResetAudio(); }
    void  ClientRuntime::OnSystemsCleared() noexcept           { InstallRenderSystems(); }
    void  ClientRuntime::FillEngineContext(EngineContext& ctx) noexcept
    {
        ctx.imguiContext  = m_pres.imguiContext;
        ctx.imguiAlloc    = m_pres.imguiAlloc;
        ctx.imguiFree     = m_pres.imguiFree;
        ctx.imguiUserData = m_pres.imguiUserData;
    }

    // --- presentation surface ----------------------------------------------------
    // The bodies that stood in Runtime.cpp before the Core-DLL split, on m_pres /
    // m_core. Every SetResource call below still runs in ArcaneClient.dll, whose
    // Astra slot this class's ctor installs -- so each method's own header comment
    // about the scene TypeID resolving against the shared context still holds.
    Audio::AudioDevice& ClientRuntime::AudioSystem() noexcept { return m_pres.audio; }
    void ClientRuntime::ResetAudio() noexcept { m_pres.ResetAudio(&m_core.AssetsFacade()); }

    void ClientRuntime::SetInputSnapshot(const InputSnapshot& snap) noexcept { m_pres.input = snap; }
    const InputSnapshot& ClientRuntime::Input() const noexcept { return m_pres.input; }

    void ClientRuntime::SetImGui(void* context, void* alloc, void* freeFn, void* userData) noexcept
    {
        m_pres.imguiContext  = context;
        m_pres.imguiAlloc    = alloc;
        m_pres.imguiFree     = freeFn;
        m_pres.imguiUserData = userData;
    }
    void* ClientRuntime::ImGuiContext()  const noexcept { return m_pres.imguiContext; }
    void* ClientRuntime::ImGuiAlloc()    const noexcept { return m_pres.imguiAlloc; }
    void* ClientRuntime::ImGuiFree()     const noexcept { return m_pres.imguiFree; }
    void* ClientRuntime::ImGuiUserData() const noexcept { return m_pres.imguiUserData; }

    void ClientRuntime::SetView(const ViewTransform& view) noexcept { m_pres.view = view; }
    const ViewTransform& ClientRuntime::View() const noexcept { return m_pres.view; }

    void ClientRuntime::SetRenderContext(Batcher2D* batcher)
    {
        // Epic 04.2: carry the render alpha so RenderSubmissionSystem +
        // DrawPhysicsDebug can interpolate poses between fixed steps. The Runtime
        // owns the RunLoop, so this needs no plugin-ABI surface.
        m_core.Registry().SetResource<RenderContext2D>(
            RenderContext2D{batcher, m_pres.view,
                            static_cast<float>(m_core.Loop().Alpha())});
    }

    void ClientRuntime::SetSpriteMaterials(const std::unordered_map<Guid, std::uint16_t>* materials)
    {
        m_core.Registry().SetResource<SpriteMaterialTable>(SpriteMaterialTable{materials});
    }

    void ClientRuntime::SetSpriteTable(const std::unordered_map<Guid, SpriteEntry>* sprites,
                                       const std::uint64_t* generation)
    {
        m_core.Registry().SetResource<SpriteTable>(SpriteTable{sprites, generation});
    }

    void ClientRuntime::SetMeshTable(const std::unordered_map<Guid, MeshEntry>* meshes,
                                     const std::uint64_t* generation)
    {
        m_core.Registry().SetResource<MeshTable>(MeshTable{meshes, generation});
    }

    void ClientRuntime::SetMeshMaterials(const std::unordered_map<Guid, ResolvedMeshMaterial>* materials)
    {
        m_core.Registry().SetResource<MeshMaterialTable>(MeshMaterialTable{materials});
    }
}
