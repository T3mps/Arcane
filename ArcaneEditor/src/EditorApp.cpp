// EditorApp: Init -> MainLoop -> Shutdown. Consumes the engine's host-boot
// helpers (Arcane::GpuContext/FramePerf/HostConfig, exported from Arcane.dll)
// and hosts Sandbox.dll via the lifted Arcane::PluginHost. The frame loop
// advances the sim through the RunLoop, renders the scene into an
// OffscreenCanvas (the same canvas->batcher->
// tonemap path ArcaneRuntime drives, into a panel texture instead of the backbuffer), and
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
#include "PanelRegistry.hpp"
#include "SpriteDocument.hpp"

#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Base/Assert.hpp>   // ARC_ASSERT (StageEditorShell's context tripwire)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <ConsoleModel.hpp>   // ConsoleEntry / CategoryForMessage (InstallConsoleSink)
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (StagePluginLoad's failure banner)
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
// AddSettingsHandler / FindSettingsHandler, ImGuiSettingsHandler and
// ImHashStr are internal-only -- ImGui's ini extension point has never been
// in the public header (same cite as ShaderEditorDocument.cpp, which
// registers its own handler the same way).
#include <imgui_internal.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/callback_sink.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
        std::string EditorTitle(const Arcane::Project* project, const std::string& scene,
                                bool sceneDirty, const char* backend)
        {
            std::string title;
            if (project)
            {
                title += project->Manifest().name;
            }

            if (!scene.empty())
            {
                if (!title.empty())
                {
                    title += " - ";
                }
                title += scene;

                if (sceneDirty)
                {
                    title += "*";
                }
            }

            if (!title.empty())
            {
                title += " - ";
            }

            // BuildInfo is the DLL's own "<version> [Debug|Release|Dist]" --
            // compiled into Arcane.dll (Engine.cpp), so the title reports the
            // engine actually loaded, never this exe's header copy.
            title += "Arcane Editor ";
            title += Arcane::BuildInfo();
            if (backend && *backend)
            {
                title += " <";
                title += backend;
                title += ">";
            }
            return title;
        }

        // Persistence for EditorApp::m_playMode: an ImGuiSettingsHandler section
        // "[EditorPlayMode][State]", one "Mode=%d" line -- same shape as
        // ShaderEditorDocument's "[ArcaneEditorLayout][MaterialPanel]" handler
        // (ShaderEditorDocument.cpp:757-815), registered at the same site
        // (Init, right after ImGui's context exists and before the first
        // NewFrame reads the ini).
        constexpr const char* kPlayModeIniType = "EditorPlayMode";
        constexpr const char* kPlayModeIniName = "State";

        using StageFn = bool (Arcane::Editor::EditorApp::*)(Arcane::HostBoot::BootContext&);
        struct HostStagePatch { std::string_view id; StageFn fn; };
    }

    EditorApp::EditorApp(HostConfig cfg, Arcane::BootSplashWindow* splash)
        : m_config(std::move(cfg)), m_perf(m_config.perf), m_splash(splash),
          m_splashPresenter(m_splash) {}

    void* EditorApp::PlayModeSettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                              const char* name)
    {
        // handler->UserData is `this` (set in RegisterPlayModeSettings) -- there
        // is exactly one EditorApp per process, so this doubles as the "entry"
        // ReadLine receives, same as LayoutSettingsReadOpen returning
        // &ShaderEditorDocument::Layout().
        return std::strcmp(name, kPlayModeIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::PlayModeSettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                             void* entry, const char* line)
    {
        auto* self = static_cast<EditorApp*>(entry);
        int mode = -1;
        // Malformed or out-of-range: m_playMode keeps its Viewport default --
        // never trust an ini line a hand edit (or a future enumerator's
        // rollback) could have left in a bogus state. Viewport is also the
        // safe fallback: it is today's behavior, unchanged.
        if (std::sscanf(line, "Mode=%d", &mode) == 1 && mode >= 0 &&
            mode <= static_cast<int>(Arcane::Editor::PlayLaunchMode::SeparateWindow))
        {
            self->m_playMode = static_cast<Arcane::Editor::PlayLaunchMode>(mode);
        }
    }

    void EditorApp::PlayModeSettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                             ImGuiTextBuffer* buf)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        buf->reserve(buf->size() + 48);
        buf->appendf("[%s][%s]\n", handler->TypeName, kPlayModeIniName);
        buf->appendf("Mode=%d\n", static_cast<int>(self->m_playMode));
        buf->append("\n");
    }

    void EditorApp::RegisterPlayModeSettings()
    {
        // No context (headless) or already registered: nothing to do -- same
        // idempotency guard as ShaderEditorDocument::RegisterLayoutSettings.
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kPlayModeIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kPlayModeIniType;
        handler.TypeHash   = ImHashStr(kPlayModeIniType);
        handler.UserData   = this;   // one EditorApp per process (see m_playMode's decl)
        handler.ReadOpenFn = &EditorApp::PlayModeSettingsReadOpen;
        handler.ReadLineFn = &EditorApp::PlayModeSettingsReadLine;
        handler.WriteAllFn = &EditorApp::PlayModeSettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
    }

    namespace
    {
        constexpr const char* kPanelsIniType = "EditorPanels";
        constexpr const char* kPanelsIniName = "Visibility";
    }

    void* EditorApp::PanelVisibilitySettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                     const char* name)
    {
        return std::strcmp(name, kPanelsIniName) == 0 ? handler->UserData : nullptr;
    }

    void EditorApp::PanelVisibilitySettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*,
                                                    void* entry, const char* line)
    {
        auto* self = static_cast<EditorApp*>(entry);
        // Unknown names and junk are ignored (never trust a hand-edited ini);
        // missing lines keep the default (visible).
        if (const auto parsed = Arcane::Editor::ParsePanelVisibilityLine(line))
            self->m_panelVis.visible[static_cast<std::size_t>(parsed->first)] = parsed->second;
    }

    void EditorApp::PanelVisibilitySettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* handler,
                                                    ImGuiTextBuffer* buf)
    {
        auto* self = static_cast<EditorApp*>(handler->UserData);
        buf->reserve(buf->size() + 128);
        buf->appendf("[%s][%s]\n", handler->TypeName, kPanelsIniName);
        for (const Arcane::Editor::PanelInfo& p : Arcane::Editor::kPanels)
            if (!p.permanent)
                buf->appendf("%s=%d\n", p.name,
                             self->m_panelVis.visible[static_cast<std::size_t>(p.id)] ? 1 : 0);
        buf->append("\n");
    }

    void EditorApp::RegisterPanelVisibilitySettings()
    {
        if (ImGui::GetCurrentContext() == nullptr ||
            ImGui::FindSettingsHandler(kPanelsIniType) != nullptr)
            return;

        ImGuiSettingsHandler handler;
        handler.TypeName   = kPanelsIniType;
        handler.TypeHash   = ImHashStr(kPanelsIniType);
        handler.UserData   = this;
        handler.ReadOpenFn = &EditorApp::PanelVisibilitySettingsReadOpen;
        handler.ReadLineFn = &EditorApp::PanelVisibilitySettingsReadLine;
        handler.WriteAllFn = &EditorApp::PanelVisibilitySettingsWriteAll;
        ImGui::AddSettingsHandler(&handler);
    }

    // ---- Boot stages (Task 8: EditorApp::Init folded into CoreStages) -------
    // Each method below is one block lifted verbatim (or near-verbatim; noted
    // where not) out of the old monolithic Init(). Run() wires each into the
    // matching id on the vector Arcane::HostBoot::EditorStages(ctx) returns --
    // see that function's header comment in ProjectBoot.cpp for why the body
    // has to live here rather than in Arcane.dll (host-owned members,
    // editor-exe-only types).

    bool EditorApp::StageRuntimeCreate(Arcane::HostBoot::BootContext& ctx)
    {
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

        // Populate ctx for the SHARED type_context_install / project_open /
        // input_config / editor_lock stage bodies (ProjectBoot.cpp), which only
        // have `ctx`, not `this` -- "stages populate as they go".
        ctx.runtime = &*m_runtime;
        return true;
    }

    bool EditorApp::StageGpuCore(Arcane::HostBoot::BootContext& ctx)
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
        ctx.gpu = m_gpu.get();

        // Capture the editor's own ImGui context right after GpuContext::
        // Create built it -- the ONLY ImGui context in existence at this
        // point (the plugin has not loaded yet; StageEditorShell asserts
        // against this, see its own body).
        m_editorImguiContext = ImGui::GetCurrentContext();

        // Does NOT construct m_presenter here (Task 8c, 2026-07-30
        // correction): the swapchain-backed BootPresenter is needed exactly
        // once now, for the single reveal frame StageSplashReady draws right
        // before Show()/Close() -- there is no more "next automatic
        // present() call" to worry about landing on an unready ImGui context,
        // because BootSequence's per-stage pump is driven by the pre-device
        // splash (Arcane::BootSplashPresenter, bound for the whole Run() call
        // in Run() below) for the ENTIRE boot, never by this swapchain
        // presenter. m_presenter is emplaced lazily inside StageSplashReady,
        // the one place it is used.
        return true;
    }

    bool EditorApp::StageEditorFonts(Arcane::HostBoot::BootContext&)
    {
        // Editor fonts: Roboto base + merged lucide icons, on the editor context
        // (the only ImGui context created so far, see StageGpuCore's GpuContext::
        // Create -> ImGuiLayer::Create), before the first frame and before the
        // game ImGui context is created in StageRenderBridge. Zero engine change.
        Arcane::Editor::InstallEditorFonts();
        return true;
    }

    bool EditorApp::StageEditorShell(Arcane::HostBoot::BootContext&)
    {
        // Regression tripwire (2026-07-30 review, Fix 1's verification
        // requirement): everything below touches ImGui global state (fonts,
        // style, ConfigFlags, settings handlers) and MUST land on the
        // editor's own context, captured in StageGpuCore right after
        // GpuContext::Create built it. If this ever fires, some boot-stage
        // reordering let plugin code (PluginHost::Load -> the plugin's Init,
        // e.g. Sandbox.cpp:102's ImGui::SetCurrentContext) run before this
        // stage again -- see EditorStages' ordering comment in
        // ProjectBoot.cpp for the full incident this guards against.
        // PluginHost itself now also brackets every entry point against
        // leaking a context change (Fix 3, PluginHost.cpp) -- this assert is
        // the second, independent line of defense, verifying the OUTCOME
        // rather than trusting that guard alone.
        ARC_ASSERT(ImGui::GetCurrentContext() == m_editorImguiContext,
                   "StageEditorShell is not running on the editor's own ImGui "
                   "context -- something switched GImGui (a plugin's Init?) "
                   "before this stage ran. See ProjectBoot.hpp's EditorStages "
                   "ordering comment.");

        // Title the window as the editor. GpuContext defaults to "Arcane Runtime" (the
        // shared host helper ArcaneRuntime also uses); override it here so only this host
        // reads "Arcane Editor" -- ArcaneRuntime keeps its own title.
        //
        // Updates m_windowTitle too, not just the OS title (2026-07-30 review,
        // Fix 4): UpdateWindowTitle's `if (title == m_windowTitle) return;`
        // (below) compares against this cache, and StageFinalize's own
        // UpdateWindowTitle() call runs at a DIFFERENT point in the boot
        // sequence depending on how editor_shell and finalize happen to be
        // scheduled. If this call set the OS title WITHOUT updating the
        // cache, whichever of these two stages ran SECOND would either
        // silently stomp the other's title (cache never noticed the OS title
        // changed under it) or -- if finalize's real title had already been
        // cached first -- the OS title would get stomped to "Arcane Editor"
        // by this line and then STAY that way forever, since the cache still
        // held the (now wrong) real title and every subsequent per-frame
        // UpdateWindowTitle call (EditorAppFrame.cpp) would recompute the
        // same real title, compare equal to the stale-but-matching cache,
        // and never call SetTitle again. Keeping cache and OS title in sync
        // at every write site makes this self-correcting on the very next
        // frame regardless of which stage runs first.
        m_windowTitle = "Arcane Editor";
        m_gpu->Win().SetTitle(m_windowTitle);

        // Editor branding (Arcane Editor only -- ArcaneRuntime does neither): the Arcane logo as the
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
        // Create's ImGuiLayer::Create in StageGpuCore) and before the first
        // NewFrame, which is where ImGui reads the ini; a handler added later
        // would never see the saved entry.
        ShaderEditorDocument::RegisterLayoutSettings();
        RegisterPlayModeSettings();
        RegisterPanelVisibilitySettings();

        // Does NOT construct or bind the swapchain-backed m_presenter (Task
        // 8c, 2026-07-30 correction): that presenter's ImGui::NewFrame() now
        // happens for the first time ever inside StageSplashReady, which runs
        // strictly after finalize -- i.e. after fonts/theme/docking-flag/
        // settings-handlers above AND everything else boot does. The old
        // concern this comment used to describe (binding too early lets the
        // NEXT automatic present() call reach an unready ImGui context) no
        // longer applies: BootSequence's per-stage pump is driven by the
        // pre-device splash for this entire run (see Run() below), which
        // never touches ImGui at all.
        return true;
    }

    bool EditorApp::StageRenderBridge(Arcane::HostBoot::BootContext&)
    {
        // Render-resources bridge: hand the host-owned device + ShaderLibrary to the
        // Runtime so a plugin can build its own engine render objects (e.g. the
        // narrowphase inspector's OffscreenCanvas). Non-owning; the host outlives the
        // plugin (m_gpu is declared before the runtime/plugin in EditorApp). Null in
        // a headless host -> the plugin skips its GPU-resource creation.
        m_runtime->SetRenderResources(m_gpu->Device().Nvrhi(), &m_gpu->Shaders());

        // The hosted plugin draws its debug UI into its OWN "game" ImGui context,
        // composited INTO the viewport texture (see MainLoop), instead of the
        // editor context where a HUD would float over the editor chrome. Created
        // here, AFTER the editor ImGui layer is up (StageGpuCore) and BEFORE
        // the plugin is loaded/adopts it in StagePluginLoad. Uses the SAME GPU
        // device + ShaderLibrary the editor ImGui layer was built from. (Create
        // leaves the current ImGui context null; harmless -- the editor's
        // ImGuiLayer re-pins its own context on every BeginFrame/WantCapture*.)
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

        // Scene-in-a-panel viewport: an OffscreenCanvas running the SAME
        // canvas->batcher->tonemap path ArcaneRuntime drives, into a panel texture instead
        // of the backbuffer. The device is up by here in both the interactive host
        // and a headless `--frames N` run (which only differs in the audio backend).
        //
        // DEVIATION from the brief's literal Init-block mapping: moved here from
        // its original, later position in Init() (it used to run right after
        // the plugin loaded) because StageSpriteTables's SceneRenderResolver
        // needs m_viewport->Batch() to already exist, and sprite_tables's DAG
        // dependency is on render_bridge (not on plugin_load or a separate
        // "viewport" stage that does not exist). Pure reordering -- no field
        // gains or loses a dependency it did not already have; m_viewport only
        // ever needed m_gpu.
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
        return true;
    }

    bool EditorApp::StageEditCore(Arcane::HostBoot::BootContext&)
    {
        // edit_core (2026-07-30 review, Fix 5): m_undo/m_editBinding need
        // nothing but m_runtime, so this stage depends only on runtime_create
        // -- moved OUT of the old monolithic StageSpriteTables, which gated
        // them behind project_open AND render_bridge for no reason and, being
        // Optional-policy at the time, could leave m_undo never constructed
        // while StageFinalize still unconditionally dereferenced it.
        //
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
        return true;
    }

    bool EditorApp::StageSpriteTables(Arcane::HostBoot::BootContext&)
    {
        // Shader-editor services (Slice 5): the shared compile service, the
        // template source root, and the .arcmat -> ShaderEditorDocument routing.
        // A missing dxcompiler.dll degrades to a warn (documents show status).
        m_shaderCompiler = std::make_unique<Arcane::ShaderCompiler>();
        if (!m_shaderCompiler->Initialize(/*debounceSeconds=*/0.2))
            ARC_WARN("Arcane Editor: dxcompiler.dll unavailable -- material editing disabled");
        m_shaderSources.AddRoot("data/shaders");
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
        // invalidateSprite) until well after this stage returns, by which
        // point it exists. Without this route, a double-clicked/minted
        // .arcsprite hit DocumentHost's "no editor registered" warn-and-no-op
        // (DocumentHost.cpp:56) -- EditorAppFrame.cpp:1165 already calls
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
                // Evict-then-re-resolve on a sprite re-save. The no-gap
                // requirement (a frame must never render the 1x1 placeholder in
                // between) is the resolver's contract now, so this is one call:
                // SceneRenderResolver::InvalidateSprite.
                spriteDocServices.invalidateSprite = [this](const Arcane::Guid& g)
                {
                    if (m_resolver)
                        m_resolver->InvalidateSprite(g);
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

        // Scene asset resolution (sprite-resolution lift): ONE engine-side
        // service resolves everything a scene references into what the
        // submission path can bind -- sprites (.arcsprite -> texture/UVs/size/
        // pivot), sprite materials and the post chain (.arcmat -> a registered
        // batcher material / a bound FullscreenMaterialChain). It owns the three
        // caches, the asset resolver, and the compile drain site; the editor
        // keeps only the compile SERVICE (documents submit through it) and hands
        // the resolver the document routing below.
        {
            Arcane::SceneRenderResolver::Services rs;
            rs.runtime  = &*m_runtime;
            rs.batcher  = &m_viewport->Batch();   // scene batcher: material binds + texture eviction
            rs.device   = m_gpu->Device().Nvrhi();
            rs.backend  = m_gpu->Device().Backend();
            rs.compiler = m_shaderCompiler.get();
            rs.sources  = &m_shaderSources;
            // Open shader documents get first refusal on every drained result,
            // the order the editor has always used: a document's compiles and a
            // cache's compiles for the SAME asset ride disjoint coalesce keys,
            // so neither claims the other's.
            rs.consumeFirst = [this](const Arcane::ShaderCompileResult& r) -> bool
            {
                bool consumed = false;
                m_documents.ForEach([&](Arcane::Editor::EditorDocument& d)
                {
                    if (auto* doc = dynamic_cast<Arcane::Editor::ShaderEditorDocument*>(&d))
                        consumed = doc->ConsumeResult(r) || consumed;
                });
                return consumed;
            };
            m_resolver =
                std::make_unique<Arcane::SceneRenderResolver>(std::move(rs));
        }

        // Sprite-asset arc, Task 4: built once here rather than per-frame in
        // DrawSelectionPanels -- the callback itself is stable (always routes
        // through MintOrReuseSpriteForTexture), only the argument changes.
        m_inspectorServices.mintSpriteForTexture =
            [this](const Arcane::Guid& textureGuid) { return MintOrReuseSpriteForTexture(textureGuid); };
        return true;
    }

    bool EditorApp::StagePluginLoad(Arcane::HostBoot::BootContext&)
    {
        // The editor loads a game module only when one is specified -- a project's
        // gameModule, or an explicit --plugin. Bare `ArcaneEditor` (no --project, no
        // --plugin) starts with NO game loaded (an empty editor) rather than the physics
        // Sandbox: pluginPath defaults empty (HostConfig), so GameModule returns empty
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
                // Defined-state failure, not a bare stage-failed abort (2026-07-30
                // review, boot-corestages Task 9b): plugin_load is Optional for the
                // editor (ProjectBoot.cpp's EditorStages -- see that override's
                // comment for the full "Optional for the editor, Fatal for the
                // runtime" ruling), so a `return false;` here would no longer abort
                // boot, but it would still leave m_plugin holding a PluginHost whose
                // Load() failed partway through and only a generic "optional stage
                // 'plugin_load' failed; continuing" line in the log -- worse than
                // the Fatal path it replaced, which at least failed loudly even
                // though it never let the developer in to fix anything. Instead this
                // mirrors SwitchProject's switch_plugin_load stage exactly
                // (EditorAppProject.cpp): m_plugin.reset() leaves EXACTLY the same
                // safe, disengaged state the "no game module" branch below produces
                // on purpose (every m_plugin-> use in MainLoop is optional-guarded,
                // per this function's opening comment), and a detailed banner --
                // naming the required ABI, the same wording switch_plugin_load uses
                // -- surfaces through m_projectOpenError as the "Open Project
                // Failed" modal (EditorAppFrame.cpp) once MainLoop starts, rather
                // than only a Console line.
                ARC_ERROR("Arcane Editor: failed to load the game module / project plugins");
                m_projectOpenError = "The project opened, but its game module / plugins "
                                     "failed to load (see Console).\nCheck the DLL paths in "
                                     "the manifest and that they are built against ABI " +
                                     std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) + ".";
                m_plugin.reset();
            }
        }
        else
        {
            ARC_INFO("Arcane Editor: no --project/--plugin -- starting with no game loaded");
        }

        // Task 8: Arcane Editor boots in Edit mode -- the sim starts paused. Play (m_play)
        // unpauses it; Stop restores the snapshot and re-pauses.
        m_runtime->Loop().SetPaused(true);
        return true;
    }

    bool EditorApp::StageFinalize(Arcane::HostBoot::BootContext&)
    {
        // Surface a failed --project open as a blocking modal -- the console
        // line alone was missed twice. project_open's SHARED body (ProjectBoot.cpp)
        // already warned and kept booting project-less; this derives the same
        // fact from STATE rather than duplicating the OpenProject call:
        // Runtime::OpenProject leaves CurrentProject() null on any failure and
        // never clears an already-open one, so "a project was requested and
        // none is open now" is exactly "the open failed".
        if (!m_config.projectPath.empty() && m_runtime && !m_runtime->CurrentProject())
        {
            m_projectOpenError = "--project '" + m_config.projectPath +
                                 "' failed to open.\nRunning with the data/ + --plugin "
                                 "fallback instead (see Console).";
        }

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

        // Last: now that project/scene state is final, compute the real title
        // rather than the "Untitled" placeholder a mid-boot call would have
        // shown for one MainLoop tick (UpdateWindowTitle also runs every
        // frame -- EditorAppFrame.cpp -- so this is a courtesy, not the only
        // call site).
        UpdateWindowTitle();
        // Boot SUCCEEDED with this project -- same recording the switch path
        // does, so a project reached by --project (the Hub's normal launch)
        // lands in the list exactly like one opened from the menu.
        NoteProjectOpened();
        ReloadSceneRecents();
        // Point ImGui's ini at this project's appdata layout file BEFORE the
        // first NewFrame reads it (MainLoop starts after boot) -- ImGui then
        // auto-loads the per-project layout on frame one.
        RetargetLayoutIni();
        return true;
    }

    void EditorApp::RetargetLayoutIni()
    {
        // %LOCALAPPDATA%\Arcane\editor\layouts\<project-guid>.ini ("default"
        // for a project-less session) -- the editor's slot under the same
        // family root the Hub already uses (%LOCALAPPDATA%\Arcane\hub,
        // RecentProjects.cpp). With LOCALAPPDATA unset or unwritable, ImGui's
        // exe-dir imgui.ini default stands -- degraded, never broken.
        std::filesystem::path dir;
        if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA"); localAppData && *localAppData)
            dir = std::filesystem::path(localAppData) / L"Arcane" / L"editor" / L"layouts";
        if (dir.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return;

        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        const std::string key = (proj && !proj->Manifest().guid.empty())
                                    ? proj->Manifest().guid : std::string("default");
        const std::filesystem::path target = dir / (key + ".ini");
        if (m_layoutIniPath == target.string())
            return;   // same project (or a re-open of it): nothing to retarget

        ImGuiIO& io = ImGui::GetIO();
        // Flush the OUTGOING layout first -- but only on a real project
        // SWITCH (m_layoutIniPath already set). At boot nothing has drawn
        // yet, and saving here would write an EMPTY settings file over the
        // legacy exe-dir imgui.ini before the seed below could read it.
        if (!m_layoutIniPath.empty() && io.IniFilename && *io.IniFilename)
            ImGui::SaveIniSettingsToDisk(io.IniFilename);

        // One-time migration: seed a project's first appdata layout from the
        // legacy exe-dir imgui.ini so a hand-tuned layout survives the move.
        // The legacy file is left in place (bin/ is untracked scratch).
        if (!std::filesystem::exists(target, ec))
            if (std::filesystem::exists("imgui.ini", ec))
                std::filesystem::copy_file("imgui.ini", target, ec);

        // io.IniFilename is a BORROWED pointer (ImGui never copies it) -- the
        // member string is its stable storage for the context's lifetime.
        m_layoutIniPath = target.string();
        io.IniFilename  = m_layoutIniPath.c_str();
    }

    void EditorApp::RefreshRecents()
    {
        const std::filesystem::path file = Recents::DefaultFile();
        const std::string current =
            m_runtime && m_runtime->CurrentProject()
                ? m_runtime->CurrentProject()->Root().string()
                : std::string();

        m_recents = Recents::Select(
            Recents::Load(file),
            static_cast<std::uint32_t>(Arcane::kGamePluginABIVersion),
            current,
            [](const std::string& p) {
                std::error_code ec;
                return std::filesystem::exists(p, ec);
            });
    }

    void EditorApp::NoteProjectOpened()
    {
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!proj)
        {
            // Project-less boot (File -> Open Project is about to be raised).
            // Nothing to record, but the cache still wants filling so the very
            // first opening of the File menu is already correct.
            RefreshRecents();
            return;
        }

        const std::filesystem::path root = proj->Root();
        Recents::TouchFile(Recents::DefaultFile(),
                           root.string(),
                           root.filename().string(),
                           static_cast<std::uint32_t>(Arcane::kGamePluginABIVersion));
        RefreshRecents();
    }

    void EditorApp::ReloadSceneRecents()
    {
        m_sceneRecents = {};
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!proj)
            return;
        m_sceneRecents = Arcane::Editor::SceneRecents::LoadFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()));
        Arcane::Editor::SceneRecents::Prune(m_sceneRecents,
            [](const std::string& p)
            {
                std::error_code ec;
                return std::filesystem::exists(p, ec);
            });
    }

    void EditorApp::NoteSceneOpened(const std::filesystem::path& file)
    {
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        if (!proj)
            return;   // project-less session: nowhere durable to record
        Arcane::Editor::SceneRecents::Push(m_sceneRecents, file);
        Arcane::Editor::SceneRecents::SaveFile(
            Arcane::Editor::SceneRecents::FileFor(proj->Root()), m_sceneRecents);
    }

    bool EditorApp::StageSplashReady(Arcane::HostBoot::BootContext&)
    {
        // Task 8c (2026-07-30 correction, "the splash carries the loading UI,
        // not the editor window"): depends on "finalize" (ProjectBoot.cpp's
        // EditorStages), so this is the LAST stage the editor runs -- the
        // window is revealed only once boot is actually finished, matching
        // UnrealEdGlobals.cpp:215-236 ("Hide the splash screen now that
        // everything is ready to go" -> Hide() -> "Do final set up on the
        // editor frame and show it" -> CreateDefaultMainFrame).
        //
        // Two constraints still bind even though the reveal moved to the end:
        //   1. Never reveal an undrawn window. m_gpu's window has existed
        //      (hidden) since StageGpuCore, but nothing has ever presented a
        //      frame into its swapchain -- BootSequence's per-stage pump has
        //      been driven by the pre-device splash (Arcane::
        //      BootSplashPresenter, see Run() below) for this WHOLE boot, and
        //      that presenter never touches the swapchain. So: construct the
        //      swapchain-backed BootPresenter here (first and only use) and
        //      Present() ONE real frame at fraction=1.0 before doing anything
        //      else -- stageId/detail are left empty, matching BootProgress's
        //      own documented "terminal tick" shape (BootSequence.hpp).
        //   2. Never leave a gap with neither window on screen. Show() the
        //      real window -- which now holds that just-drawn frame, not
        //      garbage -- BEFORE Close()ing the pre-device splash. Reversing
        //      these two leaves a frame with neither on screen.
        //
        // 2026-07-30 review round 2, finding 2's second half: Present()'s
        // return alone cannot distinguish "drew and presented" from "no
        // backbuffer this call, drew nothing" -- both return true.
        // HasPresentedFrame() is the added signal that lets this stage tell
        // them apart, so it never Show()s a window nothing was drawn into.
        m_presenter.emplace(*m_gpu, Arcane::BootPresenterMode::Fullscreen);
        Arcane::BootProgress done;   // stageId/detail empty: the terminal tick
        done.fraction = 1.0f;

        bool ok = m_presenter->Present(done);
        if (ok && !m_presenter->HasPresentedFrame())
        {
            // Transient no-backbuffer (zero-size window mid-resize, surface
            // out of date). Every OTHER Present() call in this class's life
            // gets a "next frame" to self-correct on; this one does not --
            // it IS the frame the window reveals. One retry.
            ok = m_presenter->Present(done);
        }
        if (!ok)
        {
            // Quit requested during this pump: PumpEvents() drained a real
            // SDL_EVENT_QUIT/WINDOW_CLOSE_REQUESTED for m_gpu's own window --
            // distinct from m_splashPresenter's quit detection above (that
            // one only covers the SPLASH being closed); this is the real
            // window's own event backlog, unpumped for the whole boot until
            // this exact call. Do not reveal an undrawn window -- abort as a
            // Fatal-stage failure like any other boot stage would. (Honest
            // asymmetry, not silently patched: this specific path reports
            // exit code 1, not the 0 a quit normally gets via
            // BootResult::quitRequested, because that flag is set only by
            // BootSequence::Run's OWN present() calls, not by a stage's
            // return value. An OS-shutdown broadcast landing in exactly this
            // narrow window is the only realistic trigger.)
            return false;
        }
        if (!m_presenter->HasPresentedFrame())
        {
            // Still nothing drawn after one retry -- extremely unlikely (two
            // consecutive zero-size/surface-out-of-date reports back to
            // back), but the never-fail-boot contract does not extend to
            // "never fail to draw a window": refusing to reveal garbage is
            // safer than showing it.
            ARC_ERROR("Arcane Editor: failed to present the boot-complete frame -- refusing to reveal an undrawn window");
            return false;
        }

        // Show() also RAISES (Task 8d defect B, Window.cpp) -- and it must run
        // while the splash still holds the foreground, i.e. strictly before
        // the Close() below, or Windows' foreground lock refuses the raise and
        // the editor opens behind the window stack.
        m_gpu->Win().Show();
        // Disarm BEFORE Close(): m_splashPresenter's own quit detection
        // (Run()'s comment / BootSplashPresenter::Present) would otherwise
        // see the splash we are about to close OURSELVES transition
        // open->closed on the very next present() call and mistake it for
        // the user closing it -- aborting a boot that actually just finished.
        m_splashPresenter.Disarm();
        if (m_splash) m_splash->Close();
        return true;
    }

    void EditorApp::UpdateWindowTitle()
    {
        // m_undo is built later in Init than the first title push; a session with no
        // command stack yet has nothing authored, so it reads as clean.
        const bool dirty = m_undo && m_scene.IsDirty(*m_undo);
        std::string title = EditorTitle(m_runtime ? m_runtime->CurrentProject() : nullptr,
                                        m_scene.DisplayName(), dirty,
                                        m_gpu ? Arcane::ToString(m_gpu->Device().Backend())
                                              : "");
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
                // Everything below was already on the log_msg and was previously
                // thrown away one line later -- capturing it is what makes
                // severity colouring, filtering, and the category column possible.
                ConsoleEntry e;
                switch (m.level)
                {
                    case spdlog::level::err:
                    case spdlog::level::critical: e.level = Arcane::DiagSeverity::Error;   break;
                    case spdlog::level::warn:     e.level = Arcane::DiagSeverity::Warning; break;
                    default:                      e.level = Arcane::DiagSeverity::Info;    break;
                }
                e.timestampMs = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        m.time.time_since_epoch()).count());
                e.message  = std::string(m.payload.data(), m.payload.size());
                e.category = std::string(CategoryForMessage(e.message));
                if (m.source.filename) { e.file = m.source.filename; e.line = m.source.line; }
                m_console.Push(std::move(e));
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
        if (m_frameCount == 0) return;
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

    bool EditorApp::Create()
    {
        // Installed here -- before HostBoot::EditorStages(ctx) builds the stage
        // list and before any BootSequence exists -- rather than from inside
        // StageEditorShell as it used to be (2026-07-31 review, Critical 1).
        // project_open (Worker) logs from ScanContent/plugin-descriptor warnings,
        // and editor_lock (Worker) logs on a failed lock write; both run BEFORE
        // editor_shell in the DAG. spdlog's vendored callback_sink_mt protects
        // ITS OWN invocation, but Log::Engine()->sinks() is a plain unlocked
        // std::vector (logger-inl.h's broadcast loop takes no lock) -- pushing
        // onto it from the main thread while a worker is mid-iteration over the
        // same vector is a data race (reallocation frees memory the worker is
        // still walking). Installing the sink before any worker stage exists
        // removes the race structurally instead of trying to serialize around
        // it. Needs only Log::Engine() and m_console, both live at this point,
        // so it has no stage dependency of its own. Bonus: this also means the
        // Console now captures runtime_create/gpu_core's banner lines, which it
        // used to miss because the sink installed after they ran.
        //
        // Both sinks are installed in Create(), not Init(), and Shutdown()
        // removes them unconditionally -- so the Create-failed path below still
        // gets its ARC_ERROR captured by the Console and the diagnostics store.
        InstallConsoleSink();
        // Same reasoning as InstallConsoleSink above -- Arcane::Diagnostics'
        // sink slot is mutex-guarded (unlike Log::Engine()->sinks()), but a
        // worker stage (e.g. project_open's content scan) can still publish
        // diagnostics before any BootSequence exists, so this installs from
        // the same early, dependency-free point.
        m_diagnostics.InstallAsEngineSink();

        m_bootCtx.runtime     = nullptr;              // stages populate as they go
        m_bootCtx.splash      = m_splash;
        m_bootCtx.projectPath = m_config.projectPath.c_str();
        m_bootCtx.pluginPath  = m_config.pluginPath.c_str();
        m_bootCtx.moduleName  = "ArcaneEditor.exe";

        // Spec sec 6: the editor ALWAYS shows boot progress, regardless of any
        // opened project's manifest (project_open's shared CoreStages body
        // never touches showProgress at all -- only RuntimeStages' override
        // does, see ProjectBoot.cpp). BootSplashWindow::ShowProgress already
        // defaults true, so this is redundant with that default today; set
        // explicitly anyway so the editor's intent reads from this file
        // rather than depending on a default it does not own.
        if (m_splash) m_splash->SetShowProgress(true);

        // THE host-owned stage set: every id whose real work touches an EditorApp
        // private member or an editor-exe-only type (EditorTheme/EditorFonts/
        // ShaderEditorDocument, none of which Arcane.dll can see), paired with the
        // method that supplies it. This table IS the list EditorApp.hpp's boot
        // comment describes -- prefer editing it over restating it in prose.
        //
        // Membership here is the whole contract: an id in this table is patched
        // below, an id NOT in it keeps whatever body EditorStages built. Both
        // directions of drift now fail loudly rather than silently:
        //   - id in EditorStages but missing here -> Make()'s Unpatched(id)
        //     sentinel fires when the stage runs (ProjectBoot.cpp).
        //   - id here but gone from EditorStages (renamed/removed upstream, with
        //     a live Stage* method left behind) -> the lookup below fails and
        //     Create() aborts. Nothing used to catch this half; BootStageParityTest
        //     cannot see it either, since it compares ids and never invokes `.run`.
        static constexpr HostStagePatch kHostStages[] =
        {
            { "runtime_create", &EditorApp::StageRuntimeCreate },
            { "gpu_core",       &EditorApp::StageGpuCore },
            { "editor_fonts",   &EditorApp::StageEditorFonts },
            { "editor_shell",   &EditorApp::StageEditorShell },
            { "render_bridge",  &EditorApp::StageRenderBridge },
            { "edit_core",      &EditorApp::StageEditCore },
            { "sprite_tables",  &EditorApp::StageSpriteTables },
            { "plugin_load",    &EditorApp::StagePluginLoad },
            { "finalize",       &EditorApp::StageFinalize },
            { "splash_ready",   &EditorApp::StageSplashReady },
        };

        // HostBoot::EditorStages(ctx) is the SAME shared function
        // BootStageParityTest exercises and RuntimeApp calls for its own list
        // (RuntimeApp::Run) -- this is the literal call that keeps the two hosts
        // from silently diverging on which steps exist. This is the one deviation
        // from the brief's literal one-line Run(): a direct
        // `BootSequence seq(EditorStages(ctx))` cannot reach EditorApp's private
        // members from inside Arcane.dll, so the vector is captured, patched with
        // the table above, then moved into BootSequence -- ids/deps/policy/thread/
        // weight (the actual DAG shape the parity test polices) are untouched.
        std::vector<Arcane::BootStage> stages = Arcane::HostBoot::EditorStages(m_bootCtx);

        // Iterate the TABLE, not the stage list: a patch that matches nothing is
        // then just a failed lookup at the point it mattered, with no parallel
        // "did I apply this one" array to keep in sync.
        for (const HostStagePatch& patch : kHostStages)
        {
            const auto it = std::ranges::find(stages, patch.id,
                [](const Arcane::BootStage& s) { return std::string_view(s.id); });
            if (it == stages.end())
            {
                ARC_ERROR("EditorApp::Create: host stage patch '{}' matched no "
                    "stage in EditorStages() -- the id was renamed or "
                    "removed in ProjectBoot.cpp without updating this table",
                    patch.id);
                return false;
            }
            it->run = [this, fn = patch.fn] { return (this->*fn)(m_bootCtx); };
        }

        m_bootSeq.emplace(std::move(stages));
        return true;
    }

    EditorApp::InitResult EditorApp::Init()
    {
        ARC_ASSERT(m_bootSeq.has_value(), "EditorApp::Init called before Create()");

        // m_splashPresenter is BootSequence's presenter for the WHOLE run
        // (Task 8c) -- from runtime_create through finalize, every per-stage
        // present() call reports into m_splash's status text + taskbar
        // progress rather than the swapchain, AND (2026-07-30 review round 2,
        // finding 2) arms/checks the splash's own open/closed state so
        // IBootPresenter's documented quit contract (BootSequence.hpp:65)
        // still fires if the user closes the splash mid-boot -- see
        // BootSplashPresenter::Present's own comment. Safe to run
        // unconditionally: it tolerates m_splash == nullptr, same never-fail
        // contract as BootSplashWindow itself. It is a class member, not
        // constructed here, so StageSplashReady can Disarm() it before
        // closing the splash intentionally -- see that method.
        const Arcane::BootResult boot = m_bootSeq->Run(&m_splashPresenter);
        if (boot.ok)
        {
            m_bootCompleted = true;   // see Shutdown()'s EditorLock::Clear
            return InitResult::Ok;
        }

        // Boot did not reach StageSplashReady, so nothing has closed the splash
        // and nothing ever will draw into it again. Close it HERE rather than
        // leaving it to Destroy(): Run() calls Shutdown() first, and Shutdown()
        // can block for a long time (m_moduleBuild.Join waits on a child
        // msbuild; WriteAutoScreenshot encodes a PNG), during all of which the
        // splash would sit on screen showing a stale status line -- the exact
        // unresponsive-window shape this whole arc exists to prevent.
        // Idempotent: Destroy() repeats it as a backstop, and Close() is a
        // no-op on a window that was never created or is already gone.
        if (m_splash)
            m_splash->Close();

        return boot.quitRequested ? InitResult::Quit : InitResult::Failed;
    }

    int EditorApp::Main()
    {
        MainLoop();
        return 0;
    }

    void EditorApp::Shutdown()
    {
        // Deregister the diagnostics sink FIRST, same reason as the console sink
        // erase right below: nothing may dispatch into a half-torn-down editor.
        // UninstallEngineSink is identity-guarded (ClearSinkIfCurrent), so this
        // is a no-op if some other DiagnosticStore has since become the
        // process-wide sink.
        m_diagnostics.UninstallEngineSink();

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

        // Reclaim the module-rebuild worker before member teardown: it only
        // touches its own mutex-guarded queue, but a thread outliving the
        // object that owns that queue is a use-after-free waiting for a
        // reorder. Blocks until the child cmd/msbuild exits -- closing the
        // editor mid-build waits for the build, by design (there is no child
        // handle to terminate through _wpopen).
        m_moduleBuild.Join();

        // While the device and viewport are still alive (m_gpu destructs after
        // Run returns): leave the Hub a fresh cover of the last thing seen.
        WriteAutoScreenshot();

        // Release the editor lock: this project is no longer open anywhere.
        if (m_bootCompleted && m_runtime)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                Arcane::EditorLock::Clear(proj->Root());
            }
        }

        // defensive: today Shutdown only runs after a successful Init, so m_gpu is non-null;
        // the guard covers a future partial-init/destructor path.
        if (m_gpu)
        {
            m_gpu->Device().Nvrhi()->waitForIdle();
        }
        ARC_INFO("Arcane Editor exiting after {} frames", m_frameCount);

        // The member destructors then run (after Run returns + ~EditorApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped.
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render stack (command list + framebuffer
        //                cache release their NVRHI handles before the device), window
        //                LAST. So gpu outlives runtime + plugin exactly as ArcaneRuntime's did.
        //                See GpuContext's header.
        // m_typeContext is intentionally NOT freed (heap-leaked, see Init).
    }

    void EditorApp::Destroy()
    {
        // Runs after Shutdown(), and after a failed Create() where it is empty.
        // Ordered before member teardown deliberately: the stage closures inside
        // hold `this` and reference m_bootCtx, and while nothing re-invokes them
        // at this point, releasing them here keeps that lifetime question from
        // ever being load-bearing on member destruction order.
        m_bootSeq.reset();

        // Belt and braces for the paths that never reached StageSplashReady --
        // a failed Create(), or a Fatal stage aborting before the reveal. Close()
        // is idempotent and a no-op when the window was never created, so the
        // normal success path (where splash_ready already closed it) is unaffected.
        // Without this, a boot that fails at, say, gpu_core leaves the splash on
        // screen with a stale status line until the process unwinds.
        if (m_splash)
        {
            m_splash->Close();
        }
    }

    int EditorApp::Run()
    {
        int exitCode = 1;

        if (Create())
        {
            switch (Init())
            {
            case InitResult::Ok:     exitCode = Main(); break;
            case InitResult::Quit:   exitCode = 0;      break;   // user closed the splash
            case InitResult::Failed: exitCode = 1;      break;
            }
        }

        Shutdown();
        Destroy();
        return exitCode;
    }
}
