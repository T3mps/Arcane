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
//
// EditorApp is ONE class across four translation units:
//   EditorApp.cpp         boot + teardown (this file): Init, Shutdown, Run,
//                         the console sink, the window title.
//   EditorAppFrame.cpp    MainLoop and its per-frame phases. The frame ORDER
//                         there is load-bearing and untested -- read its header.
//   EditorAppScene.cpp    the .arcscene effects + scene dialogs + framing.
//   EditorAppProject.cpp  Open Project, material/instance creation, the watcher.

#include "EditorApp.hpp"
#include "EditorFonts.hpp"
#include "EditorTheme.hpp"
#include "SpriteDocument.hpp"

#include <ProjectBoot.hpp>
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/Device.hpp>      // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/PickBuffer.hpp>   // Arcane::PickBuffer (GPU hit-proxy viewport pick)
#include <Arcane/Render/SelectionOutline.hpp>   // Arcane::SelectionOutline (Edit-mode viewport outline)
#include <Arcane/Sprite/SpriteAsset.hpp>  // Save/LoadSpriteAsset (SpriteDocument factory + peek)

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <nvrhi/nvrhi.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
        // Window title: the project name when a project is open, else the bare
        // editor name. Since the no-project gate landed (main.cpp), a
        // project-less session is reachable ONLY via an explicit --plugin (the
        // engine-dev path) or a --project that failed to open -- never from a
        // bare launch. `scene` is SceneSession::DisplayName ("Untitled" until the
        // scene has been saved somewhere); the trailing * is the unsaved marker,
        // matching the one on File -> Save Scene.
        std::string EditorTitle(const Arcane::Project* project,
                                const std::string& scene, bool sceneDirty)
        {
            std::string title = "Arcane Editor";
            if (project)
                title += " -- " + project->Manifest().name;
            title += " -- " + scene;
            if (sceneDirty)
                title += "*";
            return title;
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
        // The editor's own look, on the editor's context and nothing else: the
        // three-tone monochrome ramp with near-black field wells (EditorTheme.hpp).
        // Before this call the editor ran on ImGui's stock dark style, whose whole
        // interactive family is bright blue. It must run before the first frame --
        // ImGuiStyle is read live during widget submission, not latched.
        Arcane::Editor::ApplyEditorTheme(ImGui::GetStyle());
        // The shader editor's pane splits are a persisted editor preference, and
        // they ride the editor's imgui.ini through an ImGuiSettingsHandler. It
        // has to be registered HERE -- after the context exists (GpuContext::
        // Create's ImGuiLayer::Create above) and before the first NewFrame,
        // which is where ImGui reads the ini; a handler added later would never
        // see the saved entry.
        ShaderEditorDocument::RegisterLayoutSettings();
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
        // lookups (Registry::GetComponent<Transform> in the frame loop) -- without this,
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
        // Claim the editor lock the moment a project is OURS: pid + process
        // start time into <root>/Saved/editor.lock. The Hub reads it to
        // focus this editor instead of spawning a rival -- across ITS
        // restarts, which the in-memory pid map cannot survive. Cleared in
        // Shutdown and on project switches; a crash's stale lock is defeated
        // by ReadLive's start-time validation, never trusted.
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
            Arcane::EditorLock::Write(proj->Root());

        if (!Arcane::HostBoot::LoadInputConfig(m_gpu->Input(), m_runtime->Configuration()))
            ARC_WARN("Arcane Editor: input actions failed to load");
        UpdateWindowTitle();

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

        // Structural-edit binding: whole-registry snapshot/restore through the
        // SAME Runtime the resolver reads, so the memento survives registry swaps.
        m_editBinding.snapshot = [rt = &*m_runtime]() -> std::vector<std::byte>
        {
            auto r = rt->SnapshotRegistry();
            return r.IsOk() ? std::move(*r) : std::vector<std::byte>{};
        };
        m_editBinding.restore = [rt = &*m_runtime](std::span<const std::byte> bytes)
        {
            return rt->RestoreRegistry(bytes);
        };

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

        // Sprite-asset arc, Task 5: .arcsprite -> SpriteDocument routing,
        // registered right beside the .arcmat route above (same
        // factory+peek shape). `this`-captures resolve m_sprites at CALL
        // time, not here -- m_sprites itself isn't constructed until the
        // block below, but nothing calls Save() (the only path that reaches
        // invalidateSprite) until well after Init() returns, by which point
        // it exists. Without this route, a double-clicked/minted .arcsprite
        // hit DocumentHost's "no editor registered" warn-and-no-op
        // (DocumentHost.cpp:53) -- EditorAppFrame.cpp:1151 already calls
        // m_documents.OpenPath on a freshly minted sprite and expected this.
        const auto spriteFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadSpriteAsset(p);
                if (!data)
                    return nullptr;
                Arcane::Editor::SpriteDocument::Services spriteDocServices;
                spriteDocServices.assets = &m_runtime->AssetsFacade();
                // The SAME shared stack MakeDocServices hands the material
                // documents (EditorAppProject.cpp:39) and the Inspector/gizmo
                // push to -- one editor-wide history, so Ctrl+Z walks back
                // through sprite field edits in the order they happened
                // alongside everything else.
                spriteDocServices.undo = m_undo ? &*m_undo : nullptr;
                spriteDocServices.invalidateSprite = [this](const Arcane::Guid& g)
                {
                    if (m_sprites)
                    {
                        // Invalidate THEN Request synchronously, not Invalidate alone:
                        // RenderSceneToViewport (EditorAppFrame.cpp:127) runs BEFORE
                        // PumpShaderEditor's re-Request (EditorAppFrame.cpp:130), so an
                        // Invalidate-only callback would erase the entry and let the
                        // very next frame draw the 1x1 placeholder before anything
                        // re-resolved it. SpriteCache::Request is synchronous (no async
                        // compile step like SpriteMaterialCache), so calling it here
                        // re-resolves the entry before this callback returns -- no
                        // frame ever renders without it. Same no-gap property
                        // SpriteMaterialCache gets from its needsRefresh/last-good
                        // scheme (SpriteMaterialCache.hpp:67-70), just achieved
                        // synchronously here instead of keeping the stale entry bound
                        // until a fresh compile lands.
                        m_sprites->Invalidate(g);
                        m_sprites->Request(g);
                    }
                };
                return std::make_unique<Arcane::Editor::SpriteDocument>(
                    std::move(spriteDocServices), p, std::move(*data));
            };
        const auto spritePeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadSpriteAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcsprite", spriteFactory, spritePeek);

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

            // Sprite-asset arc, Task 3: SpriteRenderer::sprite's .arcsprite
            // Guids resolve into SpriteEntry records through the same Assets
            // facade as the material cache above, plus the viewport's scene
            // batcher for texture eviction (RemoveTexture --
            // Batcher2D.hpp:181-191). The resolver lambda is AssetId-shaped
            // (not Guid-shaped like resolveAsset above) because
            // Project::ResolveAsset already takes an AssetId -- no Guid
            // round-trip needed.
            Arcane::Editor::SpriteCache::Services spriteServices;
            spriteServices.assets = &m_runtime->AssetsFacade();
            spriteServices.batcher = &m_viewport->Batch();
            spriteServices.resolveAsset =
                [rt = &*m_runtime](const Arcane::AssetId& assetId)
                    -> std::optional<std::filesystem::path>
            {
                const Arcane::Project* project = rt->CurrentProject();
                return project ? project->ResolveAsset(assetId) : std::nullopt;
            };
            m_sprites =
                std::make_unique<Arcane::Editor::SpriteCache>(std::move(spriteServices));

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

        // Sprite-asset arc, Task 4: built once here rather than per-frame in
        // DrawSelectionPanels -- the callback itself is stable (always routes
        // through MintOrReuseSpriteForTexture), only the argument changes.
        m_inspectorServices.mintSpriteForTexture =
            [this](const Arcane::Guid& textureGuid) { return MintOrReuseSpriteForTexture(textureGuid); };

        // Task 7: open into the project's boot scene, now that the plugin has
        // loaded (a scene naming a component the game module registers would
        // otherwise silently drop it) and m_undo exists (Adopt records the
        // clean baseline against it). A project with no boot scene, or one
        // that fails to resolve/load, keeps whatever the plugin's Init built --
        // code-spawned scenes are legacy, but nothing clears them unless a
        // scene actually takes ownership (BootScene already logged the reason).
        if (const Arcane::Project* proj = m_runtime->CurrentProject())
        {
            if (const auto boot = Arcane::HostBoot::BootScene(*m_runtime, *proj))
            {
                m_scene.Adopt(boot->file, boot->id, *m_undo);
                m_frameOnSceneOpen = true;
            }
        }
        EnsureScene();

        return true;
    }

    void EditorApp::UpdateWindowTitle()
    {
        // m_undo is built later in Init than the first title push; a session with no
        // command stack yet has nothing authored, so it reads as clean.
        const bool dirty = m_undo && m_scene.IsDirty(*m_undo);
        std::string title = EditorTitle(m_runtime ? m_runtime->CurrentProject() : nullptr,
                                        m_scene.DisplayName(), dirty);
        if (title == m_windowTitle)
            return;
        m_windowTitle = std::move(title);
        m_gpu->Win().SetTitle(m_windowTitle);
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

    // The Hub reads <root>/Saved/AutoScreenshot.png as the project's cover
    // (a hand-placed <Name>.png beside the .arcproj outranks it -- see the
    // Hub's resolve.rs). Refreshed on scene save and clean exit, so the tile
    // shows roughly what the project looked like when it was last worked on.
    // The capture is the viewport output AS COMPOSITED: post-tonemap LDR,
    // including the selection outline in Edit mode and the game HUD in Play.
    // Accepted deliberately -- an outline-free shot would need a second scene
    // render, and this is a memory aid, not a render product. Saved/ sits
    // OUTSIDE Content/ on purpose: RegisterCreatedAsset would (correctly)
    // refuse to register it, and the asset registry has no business knowing.
    void EditorApp::WriteAutoScreenshot()
    {
        if (!m_runtime || !m_viewport || !m_gpu) return;
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj) return;

        // TextureId() round-trips the output texture pointer -- the same seam
        // ImGui::Image consumes (precedent: OffscreenCanvasTest.cpp).
        auto* tex = reinterpret_cast<nvrhi::ITexture*>(
            static_cast<uintptr_t>(m_viewport->TextureId()));
        if (!tex) return;

        const std::filesystem::path file = proj->Root() / "Saved" / "AutoScreenshot.png";
        // 512 wide: the Hub tile draws ~230px, so ~2x for clean minification
        // (LoadDisplayTexture's own sizing rule), far under the Hub's 2 MB
        // cover cap. Failure is already WARN-logged inside; a screenshot that
        // cannot be written must not turn a save or a shutdown into an error.
        if (Arcane::SaveTexturePng(m_gpu->Device().Nvrhi(), tex, file, 512))
            ARC_INFO("Auto-screenshot {}", file.generic_string());
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

        // While the device and viewport are still alive (m_gpu destructs after
        // Run returns): leave the Hub a fresh cover of the last thing seen.
        WriteAutoScreenshot();

        // Release the editor lock: this project is no longer open anywhere.
        if (m_runtime)
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
                Arcane::EditorLock::Clear(proj->Root());

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
