// EditorApp: Init -> MainLoop -> Shutdown. Reuses Loom's host-boot helpers
// (GpuContext/FramePerf/LoomConfig) by source-compile and hosts Sandbox.dll via
// the lifted Arcane::PluginHost. The frame loop advances the sim through the
// RunLoop, renders the scene into an OffscreenCanvas (the same canvas->batcher->
// tonemap path Loom drives, into a panel texture instead of the backbuffer), and
// draws an editor shell -- a full-viewport dockspace (Arcane::Editor::BeginDockSpace)
// hosting a Sim toolbar (play/pause/step + time-scale), a Console panel fed by a
// callback sink on Arcane::Log::Engine(), and a dockable Viewport panel showing
// the scene texture. Scene input (camera pan/zoom, click-pick) is gated on the
// Viewport panel's hover/focus and the cursor is remapped into viewport-local
// pixels before the plugin sees it (see ViewportInput.hpp). The render plumbing
// + teardown order live in GpuContext (m_gpu). The teardown CONTRACT is encoded
// in the EditorApp member declaration order -- see EditorApp.hpp.

#include "EditorApp.hpp"
#include "EditorFonts.hpp"
#include "EditorPanels.hpp"
#include "ViewportImGuiInput.hpp"

#include <ProjectBoot.hpp>
#include <Arcane/Audio/AudioDevice.hpp>  // complete type for AudioSystem().Update (per-frame voice reap)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Edit/Gizmo.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (pre-teardown ABI gate)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/PickBuffer.hpp>   // Arcane::PickBuffer (GPU hit-proxy viewport pick)
#include <Arcane/Render/SelectionOutline.hpp>   // Arcane::SelectionOutline (Edit-mode viewport outline)
#include <Arcane/Scene/Components.hpp>   // Arcane::Transform (gizmo drag target)

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>   // Registry::InspectEntity/GetComponent (gizmo descriptor resolve)

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>

namespace Arcane::Editor
{
    namespace
    {
        // Undo/redo keybind scancodes (see MainLoop's input block). Hoisted to file
        // scope -- pure cleanup, values unchanged (verified against SDL_scancode.h).
        constexpr uint32_t kScLCtrl  = 224;  // SDL_SCANCODE_LCTRL
        constexpr uint32_t kScRCtrl  = 228;  // SDL_SCANCODE_RCTRL
        constexpr uint32_t kScLShift = 225;  // SDL_SCANCODE_LSHIFT
        constexpr uint32_t kScRShift = 229;  // SDL_SCANCODE_RSHIFT
        constexpr uint32_t kScY      = 28;   // SDL_SCANCODE_Y
        constexpr uint32_t kScZ      = 29;   // SDL_SCANCODE_Z

        // Gizmo mode-switch keybind scancodes (see MainLoop's input block). No
        // conflict with the Sandbox plugin's camera (RMB-pan + wheel-zoom only).
        constexpr uint32_t kScW = 26;   // SDL_SCANCODE_W
        constexpr uint32_t kScE = 8;    // SDL_SCANCODE_E
        constexpr uint32_t kScR = 21;   // SDL_SCANCODE_R
        constexpr uint32_t kScQ = 20;   // SDL_SCANCODE_Q

        // Resolve the ComponentDescriptor for Transform on `e`, for bracketing
        // a gizmo drag into the undo stack via CommandStack::SnapshotComponent.
        // Mirrors EditorPanels.cpp's Inspector loop (InspectEntity + meta->typeName
        // match) -- namespace-qualified, matching how ASTRA_REFLECT_TYPE registers it.
        const Astra::ComponentDescriptor* FindTransformDescriptor(Astra::Registry& reg, Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            {
                if (ci.meta && ci.meta->typeName == "Arcane::Transform")
                    return ci.descriptor;
            }
            return nullptr;
        }

        // Window title: the project name when a project is open, else the bare
        // editor name (no --project => legacy data/-next-to-exe boot, unchanged).
        std::string EditorTitle(const Arcane::Project* project)
        {
            if (project)
                return "Arcane Editor -- " + project->Manifest().name;
            return "Arcane Editor";
        }
    }

    EditorApp::EditorApp(LoomConfig cfg)
        : m_config(std::move(cfg)), m_perf(m_config.perf) {}

    bool EditorApp::Init()
    {
        // The whole platform/render/input stack, booted in order. Owned by m_gpu and
        // declared BEFORE m_runtime/m_plugin in EditorApp -- so it destructs AFTER
        // them: the render resources it owns (window/device/swapchain/shaders/canvas/
        // batcher/tonemap/imgui/input + commandList/framebuffers) must outlive runtime
        // + plugin.
        m_gpu = GpuContext::Create(m_config);
        if (!m_gpu)
        {
            ARC_ERROR("Arcane Editor: GPU context create failed");
            return false;
        }

        ARC_INFO("{} -- Arcane Editor host, backend {}", Arcane::BuildInfo(), Arcane::ToString(m_config.backend));

        // Title the window as the editor. GpuContext defaults to "Arcane Loom" (the
        // shared host helper Loom also uses); override it here so only this host reads
        // "Arcane Editor" -- Loom keeps its own title.
        m_gpu->Win().SetTitle("Arcane Editor");

        // Editor branding (Arcane Editor only -- Loom does neither): the Arcane logo as the
        // OS window/taskbar icon, and the SAME art as a display-referred texture for the
        // transport toolbar's top-left mark (Unity-style). One shared source PNG, copied
        // next to the exe at build. A missing file degrades quietly (default icon / no mark).
        constexpr const char* kLogoPath = "data/images/arcane_logo.png";
        m_gpu->Win().SetIcon(kLogoPath);
        // maxSize 64 ~= 2x the toolbar mark's on-screen size (~32px = 1.35x the button row):
        // the area-average makes a clean 64px texture, then ImGui's own bilinear only minifies
        // ~2x (a clean box). A much larger texture would leave ImGui doing a >2x single-tap
        // minify -> the aliased outline the raw 550px source produced.
        m_toolbarLogo = Arcane::LoadDisplayTexture(m_gpu->Device().Nvrhi(), kLogoPath, 64);

        // Editor shell: enable ImGui docking (the placeholder single window becomes
        // a dockspace + panels in MainLoop) and route the engine logger into the
        // Console panel's ring buffer.
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        InstallConsoleSink();

        // Editor fonts: Roboto base + merged lucide icons, on the editor context
        // (current here -- the only ImGui context created so far, see GpuContext::
        // Create's ImGuiLayer::Create above), before the first frame and before the
        // game ImGui context is created below. Zero engine change.
        Arcane::Editor::InstallEditorFonts();

        // The TypeContext is the process-wide type-identity singleton shared across
        // ArcaneEditor.exe, Arcane.dll, and every loaded plugin. It is intentionally
        // heap-allocated and never freed: TypeMeta entries registered by the plugin
        // (via ASTRA_REFLECT in Components.hpp) hold std::function thunks compiled
        // into the plugin DLL. After PluginHost::Unload -> DLClose, those thunks
        // point to unmapped memory. If the TypeContext (and its MetaRegistry) were
        // ever destructed, ~std::function() would invoke those thunks -> crash.
        // Heap-leaking is the correct production pattern for a long-running host;
        // the OS reclaims all process memory on exit anyway.
        m_typeContext = new Astra::TypeContext();
        // Install the shared context in THIS module too (ArcaneEditor.exe is a separate
        // binary from Arcane.dll -- Astra::GetTypeContext()/SetTypeContext() resolve
        // through a PER-MODULE static slot, by design; Runtime::Impl's ctor installs
        // the same m_typeContext for Arcane.dll's own slot, see Runtime.cpp). Required
        // BEFORE the gizmo interaction code's TypeID<Arcane::Transform>::Value()
        // lookups (Registry::GetComponent<Transform> in MainLoop) -- without this,
        // ArcaneEditor.exe's first TypeID<T>::Value() call would silently fall back to its
        // own empty module-local DefaultTypeContext() instead of the shared one, so
        // GetComponent<Transform> would resolve against the WRONG ComponentID
        // (always-miss at best, aliasing a different component's bytes at worst).
        Astra::SetTypeContext(m_typeContext);
        // Opt into a real audio device only for an INTERACTIVE run (maxFrames == 0 = run
        // until quit). The scripted "ArcaneEditor --frames N" GPU-verify is headless -> false
        // -> miniaudio's null backend (no real device grabbed on a CI box).
        m_runtime.emplace(m_typeContext, m_config.maxFrames == 0);

        // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
        // Runtime so a plugin can build its own engine render objects (e.g. the
        // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
        // plugin (m_gpu is declared before the runtime/plugin in EditorApp). Null in
        // a headless host -> the plugin skips its GPU-resource creation.
        m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

        // Open the project (if any) BEFORE loading input + the game module (mirrors Loom).
        if (!m_config.projectPath.empty())
        {
            if (!m_runtime->OpenProject(m_config.projectPath))
            {
                ARC_WARN("Arcane Editor: --project '{}' failed to open; using data/ + --plugin fallback",
                         m_config.projectPath);
                // Surface at first frame -- the console line alone was missed twice.
                m_projectOpenError = "--project '" + m_config.projectPath +
                                     "' failed to open.\nRunning with the data/ + --plugin "
                                     "fallback instead (see Console).";
            }
        }
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
            ARC_WARN("Arcane Editor: input actions failed to load");
        m_gpu->Win().SetTitle(EditorTitle(m_runtime->CurrentProject()));

        // The hosted plugin draws its debug UI into its OWN "game" ImGui context,
        // composited INTO the viewport texture (see MainLoop), instead of the
        // editor context where a HUD would float over the editor chrome. Created
        // here, AFTER the editor ImGui layer is up (GpuContext::Create) and BEFORE
        // the plugin is loaded/adopts it below. Uses the SAME GPU device +
        // ShaderLibrary the editor ImGui layer was built from. (Create leaves the
        // current ImGui context null; harmless -- the editor's ImGuiLayer re-pins
        // its own context on every BeginFrame/WantCapture*.)
        m_gameImgui = Arcane::OffscreenImGuiLayer::Create(m_gpu->Device(), m_gpu->Shaders());
        if (!m_gameImgui)
        {
            ARC_ERROR("Arcane Editor: OffscreenImGuiLayer creation failed");
            return false;
        }

        // ABI v2: install the ImGui context + allocators on the Runtime BEFORE the
        // plugin loads. PluginHost::RefreshContext copies these into the EngineContext
        // at Init time, so the plugin's Init adopts THIS GImGui across the DLL boundary.
        // The context is the GAME context (not the editor's), so the plugin's DrawUI
        // renders into the viewport pass in MainLoop.
        {
            // Same process-global ImGui allocators the editor context uses (ImGui's
            // allocator functions are global, not per-context); only the context
            // pointer differs from the editor redirect.
            ImGuiMemAllocFunc allocFn = nullptr; ImGuiMemFreeFunc freeFn = nullptr; void* ud = nullptr;
            ImGui::GetAllocatorFunctions(&allocFn, &freeFn, &ud);
            m_runtime->SetImGui(m_gameImgui->Context(),
                                reinterpret_cast<void*>(allocFn),
                                reinterpret_cast<void*>(freeFn),
                                ud);
        }

        // The editor loads a game module only when one is specified -- a project's
        // gameModule, or an explicit --plugin. Bare `ArcaneEditor` (no --project, no
        // --plugin) starts with NO game loaded (an empty editor) rather than the physics
        // Sandbox: pluginPath defaults empty (LoomConfig), so GameModule returns empty
        // here and the plugin host is left disengaged. Every m_plugin-> use in MainLoop
        // is optional-guarded, so a disengaged plugin is safe. Sandbox stays available on
        // demand via --plugin Sandbox.dll or --project SampleProject.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
        if (!gameModule.empty() || !pluginModules.empty())
        {
            // A game module OR just project plugin modules is enough to host: an empty
            // gameModule makes a plugins-only host (open a plugin-only project to work on it
            // before its game DLL exists). PluginHost handles the primary-less case.
            m_plugin.emplace(*m_runtime,
                gameModule.empty() ? std::filesystem::path{} : std::filesystem::path(gameModule));
            for (const auto& dll : pluginModules)
                m_plugin->AddPlugin(dll);
            if (!m_plugin->Load())
            {
                ARC_ERROR("Arcane Editor: failed to load the game module / project plugins");
                return false;
            }
        }
        else
        {
            ARC_INFO("Arcane Editor: no --project/--plugin -- starting with no game loaded");
        }

        // Task 8: Arcane Editor boots in Edit mode -- the sim starts paused. Play (m_play)
        // unpauses it; Stop restores the snapshot and re-pauses.
        m_runtime->Loop().SetPaused(true);

        // Scene-in-a-panel viewport: an OffscreenCanvas running the SAME
        // canvas->batcher->tonemap path Loom drives, into a panel texture instead
        // of the backbuffer. The device is up by here in both the interactive host
        // and a headless `--frames N` run (which only differs in the audio backend).
        m_viewport = Arcane::OffscreenCanvas::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(), 1280, 720);
        if (!m_viewport)
        {
            ARC_ERROR("Arcane Editor: OffscreenCanvas create failed");
            return false;
        }

        // GPU hit-proxy picker, sized to match the viewport (resized together).
        // Supersampled 2x: the id target feeds the JFA outline below, which needs
        // sub-pixel silhouette coverage to seed a smooth distance field.
        m_pick = Arcane::PickBuffer::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                            1280, 720, /*supersample*/ 2);
        if (!m_pick)
        {
            ARC_ERROR("Arcane Editor: PickBuffer create failed");
            return false;
        }

        // Selection + hover outline (Edit-mode viewport pass), a sibling of
        // m_pick: created and resized at the same viewport size (its own targets
        // are 1x -- it derives the id buffer's supersample factor internally).
        m_outline = Arcane::SelectionOutline::Create(m_gpu->Device().Nvrhi(), m_gpu->Shaders(),
                                                     1280, 720);
        if (!m_outline)
        {
            ARC_ERROR("Arcane Editor: SelectionOutline creation failed");
            return false;
        }

        // Editor undo/redo history. The resolver re-reads Runtime::Registry()
        // EVERY call rather than capturing a Registry& up front: Runtime swaps
        // out the registry object on Play/Stop (PlaySession -> Runtime::
        // SnapshotRegistry/RestoreRegistry) and on plugin hot-reload, so a
        // cached reference would dangle. `rt` is a raw Runtime* into m_runtime
        // (stable across that swap -- only the registry INSIDE it is replaced),
        // safe because m_undo destructs before m_runtime (declaration order).
        m_undo.emplace([rt = &*m_runtime]() -> Astra::Registry& { return rt->Registry(); });

        // Shader-editor services (Slice 5): the shared compile service, the
        // template source root, and the .arcmat -> ShaderEditorDocument routing.
        // A missing dxcompiler.dll degrades to a warn (documents show status).
        m_shaderCompiler = std::make_unique<Arcane::ShaderCompiler>();
        if (!m_shaderCompiler->Initialize(/*debounceSeconds=*/0.2))
            ARC_WARN("Arcane Editor: dxcompiler.dll unavailable -- material editing disabled");
        m_shaderSources.AddRoot("shaders");
        const auto materialFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadMaterialAsset(p);
                if (!data)
                    return nullptr;
                return std::make_unique<Arcane::Editor::ShaderEditorDocument>(
                    MakeDocServices(), p, std::move(*data));
            };
        // Peek: focus an already-open doc WITHOUT constructing a duplicate
        // (whose ctor would submit compiles on the live doc's coalesce keys).
        const auto materialPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadMaterialAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcmat", materialFactory, materialPeek);

        // Scene sprite materials (Slice 8): SAVED .arcmat assets referenced by
        // SpriteRenderer::material compile through the same service and
        // register with the viewport's scene batcher.
        {
            const auto resolveAsset = [rt = &*m_runtime](const Arcane::Guid& g)
                -> std::optional<std::filesystem::path>
            {
                const Arcane::Project* project = rt->CurrentProject();
                return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(g))
                               : std::nullopt;
            };
            Arcane::SpriteMaterialCache::Services cacheServices;
            cacheServices.compiler = m_shaderCompiler.get();
            cacheServices.sources = &m_shaderSources;
            cacheServices.assets = &m_runtime->AssetsFacade();
            cacheServices.device = m_gpu->Device().Nvrhi();
            cacheServices.backend = m_gpu->Device().Backend();
            cacheServices.resolveAsset = resolveAsset;
            m_spriteMaterials =
                std::make_unique<Arcane::SpriteMaterialCache>(std::move(cacheServices));

            // Post-chain twin (post arc, slice 2): same services, same drain
            // site; slice 3's PostProcess sweep drives Request.
            Arcane::PostChainCache::Services postServices;
            postServices.compiler = m_shaderCompiler.get();
            postServices.sources = &m_shaderSources;
            postServices.assets = &m_runtime->AssetsFacade();
            postServices.device = m_gpu->Device().Nvrhi();
            postServices.backend = m_gpu->Device().Backend();
            postServices.resolveAsset = resolveAsset;
            m_postChains =
                std::make_unique<Arcane::PostChainCache>(std::move(postServices));
        }

        return true;
    }

    Arcane::Editor::DocServices EditorApp::MakeDocServices()
    {
        Arcane::Editor::DocServices s;
        s.device   = m_gpu->Device().Nvrhi();
        s.shaders  = &m_gpu->Shaders();
        s.compiler = m_shaderCompiler.get();
        s.sources  = &m_shaderSources;
        s.runtime  = &*m_runtime;
        s.undo     = m_undo ? &*m_undo : nullptr;
        s.clock    = &m_editorClock;
        s.backend  = m_gpu->Device().Backend();
        s.onAssetSaved = [this](const Arcane::Guid& id)
        {
            if (m_spriteMaterials)
                m_spriteMaterials->Invalidate(id);
            if (m_postChains)
                m_postChains->Invalidate(id);
            // Re-baseline the file watcher: our own save is not an external
            // edit and must not bounce back as a reload.
            if (const Arcane::Project* p = m_runtime ? m_runtime->CurrentProject()
                                                     : nullptr)
                if (const auto path = p->ResolveAsset(Arcane::AssetId::FromGuid(id)))
                {
                    std::error_code ec;
                    const auto t = std::filesystem::last_write_time(*path, ec);
                    if (!ec)
                        m_materialMtimes[path->generic_string()] = t;
                }
        };
        s.onParamRenamed = [this](const Arcane::Guid& id, const std::string& oldName,
                                  const std::string& newName)
        {
            // Assisted rename rewrote this instance's file; an OPEN document
            // for it gets patched in memory (never stomped -- re-key only).
            if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(
                    m_documents.FindByGuid(id)))
                doc->PatchParamRename(oldName, newName);
        };
        return s;
    }

    void EditorApp::PollMaterialWatch()
    {
        if (m_editorClock < m_materialWatchNext)
            return;
        m_materialWatchNext = m_editorClock + 1.0;
        const Arcane::Project* project =
            m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!project)
            return;

        for (const Arcane::Editor::AssetEntry& e :
             Arcane::Editor::BuildAssetEntries(project->Registry()))
        {
            if (e.kind != Arcane::Editor::AssetKind::Material)
                continue;
            const auto path = project->ResolveAsset(Arcane::AssetId::FromGuid(e.guid));
            if (!path)
                continue;
            std::error_code ec;
            const auto mtime = std::filesystem::last_write_time(*path, ec);
            if (ec)
                continue;   // deleted/unreadable -- documents keep last-good
            const auto [it, inserted] =
                m_materialMtimes.try_emplace(path->generic_string(), mtime);
            if (inserted || it->second == mtime)
            {
                it->second = mtime;
                continue;   // first sighting is the baseline, not an event
            }
            it->second = mtime;

            // An EXTERNAL edit landed: scene sprites re-resolve, an open
            // document for the asset reloads (clean) or keeps its edits with
            // a warn (dirty -- never stomped), and open documents whose
            // PARENT chain contains it re-resolve + recompile.
            ARC_INFO("material '{}' changed on disk", e.name);
            if (m_spriteMaterials)
                m_spriteMaterials->Invalidate(e.guid);
            if (m_postChains)
                m_postChains->Invalidate(e.guid);
            m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
            {
                auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d);
                if (!doc)
                    return;
                if (doc->AssetGuid() == e.guid)
                {
                    if (doc->Dirty())
                        ARC_WARN("'{}' changed on disk but has unsaved edits here -- "
                                 "keeping yours (Save overwrites the disk version)",
                                 e.name);
                    else
                        doc->ReloadFromDisk();
                }
                else if (doc->DependsOn(e.guid))
                    doc->RefreshParentChain();
            });
        }
    }

    void EditorApp::MaterialNewPickedThunk(const char* path, void* user)
    {
        // SDL dialog callback thread (see ProjectPickedThunk).
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingMaterialNewPath = path;
    }

    void EditorApp::MaterialOpenPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingMaterialOpenPath = path;
    }

    void EditorApp::InstanceNewPickedThunk(const char* path, void* user)
    {
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingMaterialMutex);
        self->m_pendingInstanceNewPath = path;
    }

    void EditorApp::CreateInstanceAt(std::filesystem::path path, Arcane::Guid parent)
    {
        if (!parent.IsValid())
            return;
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.parent = parent;
        data.name = path.stem().string();
        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create instance at '{}'", path.generic_string());
            return;
        }
        // Register immediately -- ResolveParentChain on the new document needs the
        // registry to know BOTH this instance and its parent right now.
        m_runtime->RegisterCreatedAsset(path);
        m_documents.OpenPath(path);
    }

    void EditorApp::CreateMaterialAt(std::filesystem::path path)
    {
        if (path.extension() != ".arcmat")
            path += ".arcmat";

        // UE-model: every new material is GRAPH-owned (freeform HLSL lives in
        // Custom nodes; legacy text-owned .arcmat files still open fine).
        // Starter = a Color wired to the Output -- never an empty canvas.
        Arcane::MaterialAssetData data;
        data.id = Arcane::Guid::Generate();
        data.name = path.stem().string();
        data.kind = "fullscreen";
        Arcane::MaterialGraph g;
        Arcane::GraphNode out;
        out.id = 1;
        out.type = Arcane::GraphNodeType::Output;
        out.posX = 420.0f;
        out.posY = 200.0f;
        Arcane::GraphNode color;
        color.id = 2;
        color.type = Arcane::GraphNodeType::ConstColor;
        color.posX = 160.0f;
        color.posY = 200.0f;
        color.value[0] = 0.2f; color.value[1] = 0.8f;
        color.value[2] = 1.0f; color.value[3] = 1.0f;
        g.nodes = { out, color };
        Arcane::GraphLink l;
        l.fromNode = 2;
        l.toNode = 1;
        g.links.push_back(l);
        g.nextId = 3;
        auto gen = Arcane::GenerateGraphSnippet(g, Arcane::MaterialSurface::Fullscreen);
        data.snippet = std::move(gen.snippet);
        data.graph = std::move(g);
        if (!Arcane::SaveMaterialAsset(path, data))
        {
            ARC_WARN("Arcane Editor: could not create material at '{}'", path.generic_string());
            return;
        }
        // Register with the open project's registry so the new asset appears in
        // the browser and resolves by GUID IMMEDIATELY (not on next project open).
        m_runtime->RegisterCreatedAsset(path);
        m_documents.OpenPath(path);
    }

    void EditorApp::ProjectPickedThunk(const char* path, void* user)
    {
        // Runs on an SDL-owned BACKGROUND thread (the Windows dialog backend
        // fires the callback from a detached worker, not PumpEvents/the main thread) --
        // m_pendingProjectPath must be synchronized against MainLoop's top-of-frame read.
        auto* self = static_cast<EditorApp*>(user);
        if (!path) return;
        std::lock_guard<std::mutex> lk(self->m_pendingProjectMutex);
        self->m_pendingProjectPath = path;
    }

    void EditorApp::SwitchProject(const std::filesystem::path& path)
    {
        // Validate FIRST -- never tear down a live session for a project we cannot
        // fully open. This mirrors BOTH checks Runtime::OpenProject will do (open +
        // ABI gate) so the post-teardown OpenProject below cannot fail for those
        // reasons, and a bad pick leaves the current session completely untouched.
        auto probe = Arcane::Project::Open(path);
        if (!probe)
        {
            ARC_ERROR("Open Project: '{}' is not a valid Arcane project", path.generic_string());
            m_projectOpenError = "'" + path.generic_string() +
                                 "' is not a valid Arcane project (no readable .arcproj).";
            return;
        }
        if (probe->Manifest().engineAbi != static_cast<int>(Arcane::kGamePluginABIVersion))
        {
            ARC_ERROR("Open Project: '{}' targets engine ABI {} but this engine is ABI {}",
                      path.generic_string(), probe->Manifest().engineAbi,
                      static_cast<int>(Arcane::kGamePluginABIVersion));
            m_projectOpenError = "'" + path.generic_string() + "' targets engine ABI " +
                                 std::to_string(probe->Manifest().engineAbi) +
                                 " but this editor is ABI " +
                                 std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) +
                                 ".\nRebuild the project's game DLL against this engine "
                                 "and update its manifest.";
            return;
        }

        // Documents belong to the outgoing project (their texture params and
        // parent chains resolve through ITS registry). Refuse to switch over
        // unsaved edits -- no silent loss -- and close the rest (review m5).
        if (m_documents.AnyDirty())
        {
            ARC_ERROR("Open Project: unsaved material documents -- save or close them "
                      "before switching projects");
            m_projectOpenError = "There are unsaved material documents.\n"
                                 "Save or close them before switching projects.";
            return;
        }
        m_documents.CloseAll();
        // Materials resolved against the outgoing project's registry.
        if (m_spriteMaterials)
            m_spriteMaterials->Clear();
        if (m_postChains)
            m_postChains->Clear();

        // Return to Edit + clear editor state that references the outgoing scene.
        if (m_play.IsPlaying())
            m_play.Stop(*m_runtime, m_plugin ? m_plugin->Vtable() : nullptr);
        m_selection.Clear();
        if (m_undo) m_undo->Clear();

        // Idle the GPU before freeing plugin-owned GPU resources, then unload the plugin
        // (dtor: Unload -> ClearSystems + ResetRegistry, DLL still mapped).
        m_gpu->Device().Nvrhi()->waitForIdle();
        m_plugin.reset();

        // Commit the new project (sets Assets content-root) + reload input via its mount.
        if (!m_runtime->OpenProject(path))
        {
            ARC_ERROR("Open Project: OpenProject('{}') failed after validation", path.generic_string());
            m_projectOpenError = "Opening '" + path.generic_string() +
                                 "' failed after validation.\nThe previous session was torn "
                                 "down -- open another project to continue (see Console).";
            return;   // editor left with no plugin; user can Open another project
        }
        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
            ARC_WARN("Open Project: input actions failed to load");

        // Load the new game module (and/or the project's plugin modules) through the same
        // ABI-versioned plugin host. An empty gameModule with plugins = a plugins-only host.
        const std::string gameModule =
            Arcane::HostBoot::GameModule(m_runtime->CurrentProject(), m_config.pluginPath);
        const auto pluginModules = Arcane::HostBoot::PluginModules(m_runtime->CurrentProject());
        if (!gameModule.empty() || !pluginModules.empty())
        {
            m_plugin.emplace(*m_runtime,
                gameModule.empty() ? std::filesystem::path{} : std::filesystem::path(gameModule));
            for (const auto& dll : pluginModules)
                m_plugin->AddPlugin(dll);
            if (!m_plugin->Load())
            {
                ARC_ERROR("Open Project: failed to load the game module / project plugins");
                m_projectOpenError = "The project opened, but its game module / plugins "
                                     "failed to load (see Console).\nCheck the DLL paths in "
                                     "the manifest and that they are built against ABI " +
                                     std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) + ".";
            }
        }

        m_runtime->Loop().SetPaused(true);   // back to Edit
        m_gpu->Win().SetTitle(EditorTitle(m_runtime->CurrentProject()));
    }

    void EditorApp::InstallConsoleSink()
    {
        auto cb = std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& m)
            {
                m_console.Push(std::string(m.payload.data(), m.payload.size()));
            });
        m_consoleSink = cb;
        Arcane::Log::Engine()->sinks().push_back(cb);
    }

    void EditorApp::MainLoop()
    {
        auto simPrev = std::chrono::steady_clock::now();
        auto lastFrameTime = simPrev;
        bool running = true;

        while (running)
        {
            auto events = m_gpu->Win().PumpEvents();
            if (events.quitRequested) break;
            if (events.resized) m_gpu->OnResize(events.width, events.height);
            if (m_gpu->Win().IsMinimized())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // File->Open Project: run a pending soft-restart at a safe point (top of
            // frame, never mid-render). Set by ProjectPickedThunk, which runs on an
            // SDL-owned background thread -- take the path out under the lock, then
            // switch on the local copy outside the lock (SwitchProject itself never
            // touches m_pendingProjectPath/m_pendingProjectMutex).
            std::string pending;
            {
                std::lock_guard<std::mutex> lk(m_pendingProjectMutex);
                pending.swap(m_pendingProjectPath);
            }
            if (!pending.empty())
                SwitchProject(pending);

            // Material file-dialog results (same background-thread stash
            // pattern): create/open at the top of the frame, never mid-render.
            std::string materialNew, materialOpen, instanceNew;
            {
                std::lock_guard<std::mutex> lk(m_pendingMaterialMutex);
                materialNew.swap(m_pendingMaterialNewPath);
                materialOpen.swap(m_pendingMaterialOpenPath);
                instanceNew.swap(m_pendingInstanceNewPath);
            }
            if (!materialNew.empty())
                CreateMaterialAt(materialNew);
            if (!materialOpen.empty())
                m_documents.OpenPath(materialOpen);
            if (!instanceNew.empty())
                CreateInstanceAt(instanceNew, m_pendingInstanceParent);

            // Input sample (before ImGui BeginFrame so capture flags are set).
            // Set inside the block below once inViewport + the game context's
            // last-frame WantCaptureMouse are known; stays in scope past the block
            // so the click-pick further down this same frame can also honor it.
            bool gameUiClaims = false;
            {
                // Cleared here, set below if a gizmo drag starts or ends THIS frame --
                // the click-pick block (later, after ImGui builds the Viewport panel)
                // checks it to avoid treating a gizmo-handle click as a selection change.
                m_gizmoCapturedClick = false;

                const auto now = std::chrono::steady_clock::now();
                const double frameDt = std::chrono::duration<double>(now - lastFrameTime).count();
                lastFrameTime = now;
                const Arcane::InputSnapshot snap =
                    m_gpu->InDevices().Sample(m_gpu->Imgui().WantCaptureKeyboard(),
                                              m_gpu->Imgui().WantCaptureMouse());

                // The plugin only sees scene-relevant input when the Viewport panel
                // is active (hovered/focused), with the cursor remapped into
                // viewport-local pixels (m_viewportRect/m_viewportActive are set at
                // the end of the PREVIOUS frame's ImGui pass -- see below). Otherwise
                // zero the mouse buttons/scroll so the plugin's camera does not pan
                // and spawn/drag does not fire while editing panels.
                Arcane::InputSnapshot pluginSnap = snap;
                float lx = 0, ly = 0;
                const bool inViewport =
                    m_viewportActive &&
                    Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, lx, ly);
                if (inViewport)
                {
                    pluginSnap.mouseX = lx;      // plugin camera works in viewport-local px
                    pluginSnap.mouseY = ly;      // (panel size == offscreen size => scale 1)
                    // The Viewport panel IS the world surface: from the scene's point of
                    // view the cursor is in the world, not over a HUD widget. The sampled
                    // wantCaptureMouse is true (the cursor is over an ImGui window -- the
                    // Viewport), which the plugin uses to SUPPRESS camera pan/zoom + world
                    // interaction. Clear it so RMB-pan and wheel-zoom work in the viewport.
                    // (When the plugin's own HUD floats over the viewport and is hovered,
                    // ImGui's hover z-order makes the Viewport window not-hovered ->
                    // inViewport is false -> the suppression below still applies.)
                    pluginSnap.wantCaptureMouse = false;
                }
                else
                {
                    pluginSnap.mouseButtons = 0;
                    pluginSnap.wheelY       = 0.0f;
                }

                // The game's viewport debug UI (its own ImGui context, composited into
                // the viewport texture -- see the DrawUI block below) sits over the
                // scene, so a click on one of its widgets must claim the pointer ahead
                // of both the plugin's gameplay input and the editor gizmo/click-pick.
                // Uses the game context's LAST-frame WantCaptureMouse: this frame's
                // game ImGui pass runs later (after this input block closes -- see
                // MainLoop), so its capture state is not known yet this frame. That
                // 1-frame lag matches the one already inherent to inViewport/
                // m_viewportActive (both computed from the previous frame's panel
                // hover/focus).
                gameUiClaims = Arcane::Editor::GameUiClaimsPointer(
                    m_play.IsPlaying(), inViewport, m_gameImgui->WantCaptureMouse());

                // Snapshot the viewport-local cursor + RAW buttons/wheel + dt for the
                // game ImGui pass, which composites into the viewport AFTER this input
                // block's scope closes (see MainLoop). Only the few values the game
                // context needs are hoisted -- the input block stays narrow. Off the
                // viewport, hasInput is false so the game context reads the cursor as
                // off-target (BeginFrame injects -FLT_MAX).
                m_lastViewportMouse = glm::vec2(lx, ly);
                m_lastInViewport    = inViewport;
                m_lastMouseButtons  = snap.mouseButtons;
                m_lastWheel         = snap.wheelY;
                m_lastFrameDt       = frameDt;

                // Edit mode: the editor owns the left mouse button in the viewport
                // (click-pick + gizmo), so the hosted plugin must not also see it --
                // otherwise its LMB interactions (e.g. Sandbox spawn/drag/throw) fire
                // while editing. RMB + wheel stay live so plugin camera pan/zoom still
                // navigates the scene. (LMB=bit0; InputSnapshot.hpp.) ALSO clear it
                // when the game's viewport debug UI claims the pointer this frame
                // (Play + cursor over a game HUD widget) -- otherwise a HUD click
                // would fall through and spawn/drag gameplay underneath it.
                if (!m_play.IsPlaying() || gameUiClaims)
                    pluginSnap.mouseButtons &= ~static_cast<uint8_t>(0x1u);
                m_runtime->SetInputSnapshot(pluginSnap);
                m_gpu->Input().Update(frameDt, snap);

                // Undo/redo keybinds: Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo.
                // Edge-triggered off the raw hardware snapshot (InputSnapshot has
                // no built-in press-edge tracking -- InputActions.Pressed() layers
                // that on named/JSON-configured actions, which would need a
                // separate "undo"/"redo" chord binding; hand-rolled here instead
                // to avoid a same-frame double-fire: an InputActions chord match
                // is a pure AND of its keys, so a bare "ctrl+z" binding would also
                // match while Shift is held, firing Undo alongside Redo).
                // snap.wantCaptureKeyboard is already baked from
                // m_gpu->Imgui().WantCaptureKeyboard() above, so this naturally
                // suppresses the keybind while typing in an ImGui text field --
                // the same suppression point InputActions::ResolveControl uses.
                // Edit-mode only: Play routes all input to the plugin.
                {
                    const bool ctrl  = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);
                    const bool shift = snap.ScancodeDown(kScLShift) || snap.ScancodeDown(kScRShift);
                    const bool undoKeyDown = ctrl && !shift && snap.ScancodeDown(kScZ);
                    const bool redoKeyDown = ctrl && ((shift && snap.ScancodeDown(kScZ)) ||
                                                       (!shift && snap.ScancodeDown(kScY)));

                    const bool active = !m_play.IsPlaying() && !snap.wantCaptureKeyboard;
                    if (active && undoKeyDown && !m_prevUndoKeyDown) m_undo->Undo();
                    if (active && redoKeyDown && !m_prevRedoKeyDown) m_undo->Redo();
                    m_prevUndoKeyDown = undoKeyDown;
                    m_prevRedoKeyDown = redoKeyDown;
                }

                // Gizmo mode keys: W=Translate, E=Rotate, R=Scale (SDL_SCANCODE_W/E/R --
                // no conflict with the Sandbox plugin's camera, which uses RMB-pan +
                // wheel-zoom only). Edge-triggered off the raw hardware snapshot, same
                // pattern as the undo/redo keybinds above. Edit-mode + viewport-focus
                // only, so typing/clicking in another panel never changes the gizmo mode.
                {
                    const bool wDown = snap.ScancodeDown(kScW);
                    const bool eDown = snap.ScancodeDown(kScE);
                    const bool rDown = snap.ScancodeDown(kScR);
                    const bool qDown = snap.ScancodeDown(kScQ);
                    const bool keysActive = !m_play.IsPlaying() && !snap.wantCaptureKeyboard && m_viewportActive;
                    // Q = Select (no gizmo); W/E/R activate a transform gizmo (UE5 tools).
                    if (keysActive && qDown && !m_prevKeyQ) m_gizmoEnabled = false;
                    if (keysActive && wDown && !m_prevKeyW) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Translate; }
                    if (keysActive && eDown && !m_prevKeyE) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Rotate; }
                    if (keysActive && rDown && !m_prevKeyR) { m_gizmoEnabled = true; m_gizmoMode = Arcane::GizmoMode::Scale; }
                    m_prevKeyW = wDown;
                    m_prevKeyE = eDown;
                    m_prevKeyR = rDown;
                    m_prevKeyQ = qDown;
                }

                // Transform-gizmo interaction: hit-test + drag against the selected
                // entity's Transform, bracketed into the undo stack (one drag =
                // one undo step -- Begin/SnapshotComponent on press, Commit on release;
                // a no-move drag self-drops since Commit only pushes if bytes changed).
                // mouseScreen is viewport-local px (lx/ly computed above), the same
                // space CameraOffset/CameraZoom register in, so the gizmo aligns
                // pixel-for-pixel with the scene (mirrors the click-pick's PickView
                // below). LMB edges are tracked unconditionally each frame (like the
                // mode keys above) so a button already held before the cursor enters
                // the viewport is never misread as a fresh press. Deliberately does
                // NOT gate on snap.wantCaptureMouse -- it is true over the viewport
                // image by design (see the pluginSnap comment above); `inViewport`
                // already folds in m_viewportActive.
                {
                    const bool lmbDown          = (snap.mouseButtons & 0x1u) != 0;
                    const bool mousePressedLeft  = lmbDown && !m_prevLmbDown;
                    const bool mouseReleasedLeft = !lmbDown && m_prevLmbDown;
                    const glm::vec2 mouseScreen(lx, ly);
                    const bool ctrlHeld = snap.ScancodeDown(kScLCtrl) || snap.ScancodeDown(kScRCtrl);

                    // An in-progress drag must keep tracking (and commit on release)
                    // even after the cursor leaves the viewport rect -- only a FRESH
                    // drag requires the cursor in-viewport. Aborting a live drag on
                    // viewport-exit would strand the Transform at a mid-drag value
                    // with no undo record (CommandStack::Cancel does not revert), so
                    // viewport-exit is deliberately NOT an abort. !gameUiClaims is
                    // defensive: gameUiClaims is Play-only and this is already
                    // !IsPlaying()-gated, so it is a no-op today, but it keeps this
                    // gate correct if the gizmo is ever allowed to run in Play.
                    const bool gizmoActive = !m_play.IsPlaying() && !gameUiClaims &&
                                             m_gizmoEnabled && m_selection.HasSelection() &&
                                             (m_gizmoDrag.active || inViewport);
                    Astra::Registry*        regPtr = nullptr;
                    Arcane::Transform* lt     = nullptr;
                    if (gizmoActive)
                    {
                        regPtr = &m_runtime->Registry();
                        lt = regPtr->GetComponent<Arcane::Transform>(m_selection.Primary());
                    }

                    if (lt)
                    {
                        const Astra::Entity sel = m_selection.Primary();
                        const Arcane::GizmoTransform gt{ lt->position, lt->rotation, lt->scale };
                        const Arcane::GizmoView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };

                        if (!m_gizmoDrag.active)
                        {
                            // Hover + drag-start only when the cursor is over the viewport.
                            if (inViewport)
                            {
                                m_gizmoHovered = Arcane::HitTest(m_gizmoMode, m_gizmoSpace, gt, view, mouseScreen);
                                if (m_gizmoHovered != Arcane::GizmoAxis::None && mousePressedLeft)
                                {
                                    // A press on a handle owns the click regardless of
                                    // whether the descriptor resolves, so it never falls
                                    // through to the click-pick below.
                                    m_gizmoCapturedClick = true;
                                    const Astra::ComponentDescriptor* desc =
                                        FindTransformDescriptor(*regPtr, sel);
                                    if (desc)
                                    {
                                        m_undo->Begin("Gizmo");
                                        m_undo->SnapshotComponent(sel, desc);
                                        m_gizmoDrag.active           = true;
                                        m_gizmoDrag.axis             = m_gizmoHovered;
                                        m_gizmoDrag.start            = gt;
                                        m_gizmoDrag.mouseStartScreen = mouseScreen;
                                    }
                                }
                            }
                            else
                            {
                                m_gizmoHovered = Arcane::GizmoAxis::None;
                            }
                        }
                        else
                        {
                            // Current cursor in viewport-local px, EXTRAPOLATED past the
                            // rect edges (ToViewportLocal writes lx/ly even when it returns
                            // false) so the drag keeps tracking while the cursor is outside
                            // the viewport. Recomputed here because the shared lx/ly are
                            // only written when m_viewportActive is set.
                            float dragLx = 0.0f, dragLy = 0.0f;
                            Arcane::Editor::ToViewportLocal(m_viewportRect, snap.mouseX, snap.mouseY, dragLx, dragLy);
                            const glm::vec2 dragMouse(dragLx, dragLy);

                            Arcane::GizmoSnap gsnap;
                            gsnap.enabled = ctrlHeld;
                            const Arcane::GizmoTransform nt = Arcane::ApplyDrag(
                                m_gizmoMode, m_gizmoSpace, m_gizmoDrag.axis, m_gizmoDrag.start, view,
                                m_gizmoDrag.mouseStartScreen, dragMouse, gsnap);
                            lt->position = nt.position;
                            lt->rotation = nt.rotation;
                            lt->scale    = nt.scale;
                            m_gizmoHovered = m_gizmoDrag.axis;   // keep the active handle highlighted

                            if (mouseReleasedLeft)
                            {
                                m_undo->Commit();   // no-move drag self-drops (after == before)
                                m_gizmoDrag          = {};
                                m_gizmoCapturedClick = true;
                            }
                        }
                    }
                    else
                    {
                        m_gizmoHovered = Arcane::GizmoAxis::None;
                        // Only reached with an active drag when the entity/component
                        // genuinely vanished (deleted/deselected mid-drag) -- discard the
                        // now-meaningless transaction.
                        if (m_gizmoDrag.active)
                        {
                            m_undo->Cancel();
                            m_gizmoDrag = {};
                        }
                    }

                    m_prevLmbDown = lmbDown;
                }
            }

            // Sim advance through the RunLoop with the plugin callbacks interleaved.
            {
                const auto now = std::chrono::steady_clock::now();
                double simDt = std::chrono::duration<double>(now - simPrev).count();
                simPrev = now;
                if (simDt > 0.25) simDt = 0.25;
                m_runtime->Loop().Advance(simDt,
                    [&](double dt)          { if (m_plugin) m_plugin->FixedUpdateAll(dt); },
                    [&](double dt, double a){ if (m_plugin) m_plugin->UpdateAll(dt, a); });
                m_runtime->AudioSystem().Update(simDt);
            }

            // Apply the viewport panel's size from LAST frame BEFORE rendering the scene,
            // so this frame's ImGui::Image() captures a texture that is not destroyed later
            // in the same frame (OffscreenCanvas::Resize synchronously frees the old texture).
            if (m_pendingViewportW != 0 && m_pendingViewportH != 0 &&
                (m_pendingViewportW != m_viewport->Width() || m_pendingViewportH != m_viewport->Height()))
            {
                m_viewport->Resize(m_pendingViewportW, m_pendingViewportH);
                m_pick->Resize(m_pendingViewportW, m_pendingViewportH);
                m_outline->Resize(m_pendingViewportW, m_pendingViewportH);
            }

            // Scene post chain (post arc, slice 3): sweep for the assignment and
            // feed the viewport hook from the CURRENT cache state. The FIRST
            // entity with a valid PostProcess.material wins (>1 warns once);
            // none/unresolved leaves the hook null -- today's path. Chain/
            // Instance are re-fetched every frame BEFORE Draw: last frame's
            // drain may have swapped the bound instance under a re-save.
            {
                Arcane::Guid postId{};
                int postCount = 0;
                m_runtime->Registry().CreateView<Arcane::PostProcess>().ForEach(
                    [&](Astra::Entity, Arcane::PostProcess& pp)
                {
                    if (!pp.material.IsValid())
                        return;
                    if (postCount++ == 0)
                        postId = pp.material;
                });
                if (postCount > 1)
                {
                    if (!m_warnedMultiPost)
                        ARC_WARN("scene carries {} PostProcess assignments -- "
                                 "the first found wins", postCount);
                    m_warnedMultiPost = true;
                }
                else
                {
                    m_warnedMultiPost = false;
                }

                Arcane::FullscreenMaterialChain* postChain = nullptr;
                const Arcane::MaterialInstance* postInst = nullptr;
                if (postId.IsValid() && m_postChains)
                {
                    m_postChains->Request(postId, m_editorClock);
                    postChain = m_postChains->Chain(postId);
                    postInst = m_postChains->Instance(postId);
                }
                Arcane::GlobalParams postGlobals;
                postGlobals.time = (float)m_editorClock;
                postGlobals.deltaTime = (float)m_lastFrameDt;
                postGlobals.viewportWidth = (float)m_viewport->Width();
                postGlobals.viewportHeight = (float)m_viewport->Height();
                m_viewport->SetPostGlobals(postGlobals);
                m_viewport->SetPostChain(postChain, postInst,
                                         &m_runtime->AssetsFacade());
            }

            // Scene -> offscreen canvas (the SAME canvas->batcher->tonemap path Loom
            // drives, but into a panel texture). SetRenderContext writes RenderContext2D
            // in Arcane.dll and applies the plugin's stored camera; SubmitRender runs the
            // render scheduler (sprite submission + physics debug overlay) into this
            // batcher. Runs BEFORE ImGui builds the Viewport panel's Image so the texture
            // is ready when ImGui samples it at backbuffer-render time.
            m_viewport->Draw(
                [&](Arcane::Batcher2D& b)
                {
                    // Globals for registered sprite materials (Time/Delta/
                    // Viewport); built-in pipelines ignore them.
                    Arcane::GlobalParams sceneGlobals;
                    sceneGlobals.time = (float)m_editorClock;
                    sceneGlobals.deltaTime = (float)m_lastFrameDt;
                    sceneGlobals.viewportWidth = (float)m_viewport->Width();
                    sceneGlobals.viewportHeight = (float)m_viewport->Height();
                    b.SetGlobals(sceneGlobals);

                    m_runtime->SetRenderContext(&b);
                    m_runtime->Loop().SubmitRender();

                    // Transform gizmo, drawn AFTER the scene submit so it renders on
                    // top. Frame ordering guarantees the input block above already ran
                    // this frame, so m_gizmoHovered/m_gizmoDrag and the live
                    // Transform read here are current. Edit-mode + has-selection
                    // only (mirrors the interaction gate; no viewport-hover requirement
                    // here -- the gizmo should stay visible while e.g. the mouse is over
                    // the Inspector, just not be interactable there).
                    if (!m_play.IsPlaying() && m_gizmoEnabled && m_selection.HasSelection())
                    {
                        Arcane::Transform* lt = m_runtime->Registry().GetComponent<Arcane::Transform>(
                            m_selection.Primary());
                        if (lt)
                        {
                            const Arcane::GizmoTransform gt{ lt->position, lt->rotation, lt->scale };
                            const Arcane::GizmoView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                            Arcane::Draw(b, m_gizmoMode, m_gizmoSpace, gt, view, m_gizmoHovered,
                                        m_gizmoDrag.active ? m_gizmoDrag.axis : Arcane::GizmoAxis::None);
                        }
                    }
                },
                glm::vec4(0.02f, 0.02f, 0.04f, 1.0f));

            // Game debug UI -> the plugin's OWN ImGui context, composited into the
            // viewport's output texture (Play only). Runs the plugin's DrawUI in the
            // GAME context and blends it OVER the tonemapped scene -- so the HUD lives
            // inside the Viewport panel, never over the editor chrome. Runs AFTER
            // m_viewport->Draw (scene -> tonemap -> output texture) and BEFORE the
            // editor's BeginFrame below. The game context is fully bracketed by the
            // OffscreenImGuiLayer calls (each SetCurrentContext internally), so the
            // editor context is untouched. Edit mode: not run (clean viewport); the
            // input is reset so a stray draw sees no cursor/buttons.
            if (!m_play.IsPlaying())
                m_gameImgui->SetInput({});
            const Arcane::PluginVTable* vtGame = m_plugin ? m_plugin->Vtable() : nullptr;
            if (m_play.IsPlaying() && vtGame && vtGame->DrawUI)
            {
                Arcane::OffscreenImGuiLayer::Input gi;
                gi.displaySize  = glm::vec2((float)m_viewport->Width(), (float)m_viewport->Height());
                gi.deltaTime    = (float)m_lastFrameDt;
                gi.hasInput     = m_lastInViewport;
                gi.mousePos     = m_lastViewportMouse;
                gi.mouseDown[0] = (m_lastMouseButtons & 0x1u) != 0;   // LMB
                gi.mouseDown[1] = (m_lastMouseButtons & 0x2u) != 0;   // RMB
                gi.mouseDown[2] = (m_lastMouseButtons & 0x4u) != 0;   // MMB
                gi.wheel        = m_lastWheel;
                m_gameImgui->SetInput(gi);
                m_gameImgui->BeginFrame();
                m_plugin->DrawUIAll();   // game module + any secondary plugins, into the game context
                // Composite the game HUD into the viewport output texture on a one-off
                // command list (mirrors the backbuffer pass below). The OffscreenCanvas
                // owns a SEPARATE command list for its scene pass, so m_gpu->Cmd() is
                // idle here; NVRHI auto-transitions the output texture (RenderTarget for
                // this pass, back to ShaderResource for the editor's ImGui::Image).
                m_gpu->Cmd()->open();
                m_gameImgui->Render(m_gpu->Cmd(), m_viewport->OutputFramebuffer());
                m_gpu->Cmd()->close();
                m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            }

            // Selection + hover outline -> the viewport's own layer (Edit only). Refreshes
            // the hit-proxy id buffer, then edge-detects it into the post-tonemap output
            // texture (amber selected, cyan hovered). Skipped entirely when there is nothing
            // to outline. Play mode: not run (the game-imgui overlay owns this slot instead,
            // see above -- the two are mutually exclusive by mode).
            if (!m_play.IsPlaying() && (m_selection.HasSelection() || m_lastInViewport))
            {
                const Arcane::PickView view{ m_runtime->CameraOffset(), m_runtime->CameraZoom() };
                m_pick->RenderIdPass(m_runtime->Registry(), view);

                Arcane::SelectionOutline::Params op;
                op.selectedId = m_selection.HasSelection()
                              ? m_pick->PassIdOf(m_selection.Primary()) : 0u;
                op.cursorPx   = m_lastInViewport
                              ? glm::ivec2((int)m_lastViewportMouse.x, (int)m_lastViewportMouse.y)
                              : glm::ivec2(-1, -1);

                m_gpu->Cmd()->open();
                m_outline->Render(m_gpu->Cmd(), m_pick->IdTarget(),
                                  m_viewport->OutputFramebuffer(), op);
                m_gpu->Cmd()->close();
                m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            }

            // Shader-editor pump (Slice 5): advance the compile clock, dispatch
            // due jobs, and route drained results to their documents AND the
            // sprite-material cache -- the ONE drain site where compile results
            // become NVRHI shaders. Then tick every document (preview render on
            // its own OffscreenCanvas).
            m_editorClock += m_lastFrameDt;
            if (m_shaderCompiler && m_shaderCompiler->IsAvailable())
            {
                // Sweep the scene for referenced sprite materials (Slice 8):
                // Request no-ops once a material is known, so this is a cheap
                // per-frame guarantee that whatever the scene references is
                // compiling or bound.
                if (m_spriteMaterials)
                {
                    auto sprites = m_runtime->Registry().CreateView<Arcane::SpriteRenderer>();
                    sprites.ForEach([&](Astra::Entity, Arcane::SpriteRenderer& s)
                    {
                        if (s.material.IsValid())
                            m_spriteMaterials->Request(s.material, m_editorClock);
                    });
                }

                m_shaderCompiler->Poll(m_editorClock);
                for (const Arcane::ShaderCompileResult& r : m_shaderCompiler->Drain())
                {
                    bool consumed = false;
                    m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
                    {
                        if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d))
                            consumed = doc->ConsumeResult(r) || consumed;
                    });
                    if (!consumed && m_spriteMaterials)
                        consumed = m_spriteMaterials->ConsumeResult(r, m_viewport->Batch());
                    if (!consumed && m_postChains)
                        m_postChains->ConsumeResult(r);
                }
            }
            // Publish the resolution table every frame (the map's address is
            // stable; re-setting keeps the resource honest across project
            // switches and registry swaps).
            if (m_spriteMaterials)
                m_runtime->SetSpriteMaterials(&m_spriteMaterials->Table());
            PollMaterialWatch();   // external .arcmat edits (~1 Hz mtime sweep)
            m_documents.TickAll(m_lastFrameDt);

            // ImGui: editor shell -- full-viewport dockspace + Sim toolbar + Console panel
            // + the Viewport panel showing the scene texture just rendered above.
            m_gpu->Imgui().BeginFrame();
            Arcane::Editor::MenuRequests menuReq;
            Arcane::Editor::BeginDockSpace(*m_undo, menuReq);
            Arcane::Editor::DrawSimTimeToolbar(m_play, *m_runtime, m_plugin ? m_plugin->Vtable() : nullptr,
                                               (uint64_t)(intptr_t)m_toolbarLogo.Get());
            Arcane::Editor::EndDockSpace();
            if (menuReq.openProject)
                m_gpu->Win().ShowOpenFileDialog(&EditorApp::ProjectPickedThunk, this,
                                                "Arcane Project", "arcproj");
            if (menuReq.newMaterial || menuReq.openMaterial)
            {
                // Material dialogs start in the project's Content/ (the only place
                // a saved asset can register + resolve by GUID); no project = OS default.
                const Arcane::Project* proj = m_runtime->CurrentProject();
                const std::string contentDir =
                    proj ? (proj->Root() / "Content").string() : std::string();
                const char* defaultPath = contentDir.empty() ? nullptr : contentDir.c_str();
                if (menuReq.newMaterial)
                    m_gpu->Win().ShowSaveFileDialog(&EditorApp::MaterialNewPickedThunk, this,
                                                    "Arcane Material", "arcmat", defaultPath);
                if (menuReq.openMaterial)
                    m_gpu->Win().ShowOpenFileDialog(&EditorApp::MaterialOpenPickedThunk, this,
                                                    "Arcane Material", "arcmat", defaultPath);
            }
            const Arcane::Editor::AssetBrowserActions browserActions =
                Arcane::Editor::DrawAssetBrowserPanel(m_assetBrowser,
                                                      m_runtime->CurrentProject(), m_documents);
            if (browserActions.createInstanceOf.IsValid())
            {
                m_pendingInstanceParent = browserActions.createInstanceOf;
                const Arcane::Project* proj = m_runtime->CurrentProject();
                const std::string contentDir =
                    proj ? (proj->Root() / "Content").string() : std::string();
                m_gpu->Win().ShowSaveFileDialog(&EditorApp::InstanceNewPickedThunk, this,
                                                "Arcane Material", "arcmat",
                                                contentDir.empty() ? nullptr : contentDir.c_str());
            }
            Arcane::Editor::DrawConsolePanel(m_console);
            // New documents tab into the Viewport's node (captured last frame).
            m_documents.DrawAll(m_viewportDockId);

            // Project-open failure modal: any refusal in SwitchProject/Init lands
            // here (drawn at the dockspace level, outside any panel window). The
            // string stays set until the user dismisses it, so OpenPopup re-arms
            // across frames even if another popup momentarily owned the stack.
            if (!m_projectOpenError.empty() && !ImGui::IsPopupOpen("Open Project Failed"))
                ImGui::OpenPopup("Open Project Failed");
            if (ImGui::BeginPopupModal("Open Project Failed", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
                ImGui::TextUnformatted(m_projectOpenError.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(120, 0)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter))
                {
                    m_projectOpenError.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            Arcane::Editor::ViewportPanelResult vp =
                Arcane::Editor::DrawViewportPanel(m_viewport->TextureId(),
                                            m_viewport->Width(), m_viewport->Height(),
                                            m_gizmoEnabled, m_gizmoMode, m_gizmoSpace);
            m_viewportDockId = vp.dockId;
            m_pendingViewportW = vp.desiredW;
            m_pendingViewportH = vp.desiredH;
            m_viewportRect     = vp.imageRect;
            m_viewportActive   = Arcane::Editor::SceneInputActive(vp.hovered, vp.focused);

            // Viewport click-pick: GPU hit-proxy. Render every pickable entity's
            // silhouette into the id buffer and read back the pixel under the
            // viewport-local click. `view` is the SAME world->canvas transform the
            // scene render uses (m_runtime's camera == RenderContext2D), so the id
            // silhouettes register pixel-for-pixel with what is drawn. An invalid
            // result (background / outside the viewport) clears the selection.
            // Suppressed when this frame's click was already consumed by the gizmo
            // (pressed/released a handle) or a drag is still in progress -- otherwise
            // clicking/using a handle would also re-pick and disturb the selection.
            // Also suppressed when the game's viewport debug UI claimed the pointer
            // this frame (gameUiClaims, computed in the input block above) so a click
            // on a game HUD widget does not also re-pick the editor selection.
            if (vp.clicked && !m_gizmoCapturedClick && !m_gizmoDrag.active && !gameUiClaims)
            {
                const Arcane::PickView view{ m_runtime->CameraOffset(),
                                             m_runtime->CameraZoom() };
                const Astra::Entity picked = m_pick->Pick(
                    m_runtime->Registry(), view,
                    glm::vec2(vp.clickLocalX, vp.clickLocalY));
                if (picked.IsValid())
                    m_selection.Select(picked);
                else
                    m_selection.Clear();
            }

            Arcane::Editor::DrawHierarchyPanel(m_runtime->Registry(), m_selection);
            Arcane::Editor::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo,
                                               !m_play.IsPlaying(), m_runtime->CurrentProject());

            // (The hosted plugin's DrawUI now renders into its OWN ImGui context,
            // composited into the viewport texture above -- not the editor context.)

            nvrhi::ITexture* backbuffer = m_gpu->Swap().BeginFrame();
            if (!backbuffer) { ImGui::EndFrame(); continue; }

            m_gpu->Cmd()->open();
            // Clear the backbuffer directly (Arcane Editor's scene will live in a panel,
            // so there is no scene->tonemap->backbuffer pass as in Loom).
            m_gpu->Cmd()->clearTextureFloat(backbuffer, nvrhi::AllSubresources,
                                            nvrhi::Color(0.06f, 0.06f, 0.08f, 1.0f));
            nvrhi::FramebufferHandle& fb = m_gpu->FramebufferFor(backbuffer);
            m_gpu->Imgui().Render(m_gpu->Cmd(), fb);
            m_gpu->Cmd()->close();
            m_gpu->Device().Nvrhi()->executeCommandList(m_gpu->Cmd());
            m_gpu->Swap().Present();

            if (m_plugin) m_plugin->Poll();

            ++m_frameCount;
            if (m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames) running = false;
        }
    }

    void EditorApp::Shutdown()
    {
        // Deregister the console sink FIRST, before anything below can log through
        // Arcane::Log::Engine(). m_console is declared before m_runtime/m_plugin/m_gpu
        // in EditorApp.hpp, so it destructs BEFORE them; if the sink outlived this
        // point, a log emitted during ~GpuContext's Vulkan device teardown (validation
        // messages) would invoke the callback and Push into an already-destroyed
        // m_console deque.
        if (m_consoleSink)
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), m_consoleSink), sinks.end());
            m_consoleSink.reset();
        }

        // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
        // the guard covers a future partial-init/destructor path.
        if (m_gpu) m_gpu->Device().Nvrhi()->waitForIdle();
        ARC_INFO("Arcane Editor exiting after {} frames", m_frameCount);

        // The member destructors then run (after Run returns + ~EditorApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped.
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
        //                cache release their NVRHI handles before the device), window
        //                LAST. So gpu outlives runtime + plugin exactly as Loom's did.
        //                See GpuContext's header.
        // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
    }

    int EditorApp::Run()
    {
        if (!Init()) return 1;
        MainLoop();
        Shutdown();
        return 0;
    }
}
