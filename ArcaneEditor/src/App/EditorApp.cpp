// EditorApp: Init -> MainLoop -> Shutdown. Consumes the engine's host-boot
// helpers (Arcane::GpuContext/FramePerf/HostConfig, exported from Arcane.dll)
// and hosts Sandbox.dll via the lifted Arcane::PluginHost. The frame loop
// advances the sim through the RunLoop, renders the scene through the
// offscreen NriGraphContext viewport vehicle into a panel texture instead of
// the window's own backbuffer, and draws an editor shell -- a full-viewport
// dockspace (Arcane::Editor::BeginDockSpace)
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

#include "App/EditorApp.hpp"
#include "Widgets/EditorFonts.hpp"
#include "Widgets/EditorTheme.hpp"
#include "Panels/PanelRegistry.hpp"
#include "Documents/CrashReportDocument.hpp"
#include "Documents/MeshDocument.hpp"
#include "Documents/SpriteDocument.hpp"

#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Host/ReferenceImages.hpp>   // --compare/--bless (Task 9): ResolveReference/BlessReference/DiffArtifactPath
#include <Arcane/Host/VerifyReport.hpp>      // --report (Task 9): VerifyReport
#include <Arcane/Base/Assert.hpp>   // ARC_ASSERT (StageEditorShell's context tripwire)
#include <Arcane/Base/DiagEnvelope.hpp>   // Diag::ReadFile (crashReportFactory/Peek, beside materialFactory)
#include <Arcane/Base/Diagnostics.hpp>   // Diagnostics::RetargetDumpDir (RetargetDumpDir, beside RetargetLayoutIni)
#include <Arcane/Base/Engine.hpp>   // Arcane::BuildInfo / Arcane::ToString (host banner)
#include <Arcane/Base/Log.hpp>
#include <Arcane/Input/InputActions.hpp>
#include <Panels/ConsoleModel.hpp>   // ConsoleEntry / CategoryForMessage (ConsoleDiagnostics::Install)
#include <Arcane/Material/MaterialAsset.hpp>   // Save/LoadMaterialAsset (New/Open Material flows)
#include <Arcane/Mesh/MeshAsset.hpp>   // Save/LoadMeshAsset (MeshDocument factory + peek)
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::kGamePluginABIVersion (StagePluginLoad's failure banner)
#include <Arcane/Project/AssetId.hpp>    // AssetId::FromGuid (sprite-material resolver)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend / ToString (HUD)
#include <Arcane/Render/RenderErrorLatch.hpp>  // RenderErrorCount (the graph latch fold)
#include <Arcane/Render/GpuInstrumentation.hpp>   // Arcane::GpuDeviceLostObserved (Run()'s exit-code tail)
#include <Arcane/Render/Nri/NriCommon.hpp>   // ARC_NRI_CHECK (TeardownGraphForSwitch's idle)
#include <Arcane/Sprite/SpriteAsset.hpp>  // Save/LoadSpriteAsset (SpriteDocument factory + peek)

#include <Astra/Core/TypeContext.hpp>
#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

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
        std::string EditorTitle(const Arcane::Project* project, const std::string& scene, bool sceneDirty, const char* backend)
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
        // until quit). The scripted "ArcaneEditor --frames N" GPU-verify is not interactive
        // -> false -> miniaudio's device-less null backend (no real device grabbed on a CI box).
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
        // them: the render resources it owns (window, batcher, imgui, input)
        // must outlive runtime + plugin.
        //
        // THE BOOT-PATH SPLIT, mirroring RuntimeApp::StageGpuCore line for
        // line. GpuContext::Create builds the window, a device-less Batcher2D,
        // the ImGuiLayer and the input stack -- AND NO GRAPHICS DEVICE. Main()
        // then builds the NriGraphContext PAIR that owns the process's ONLY
        // graphics device: the chrome one over this same window, the viewport
        // one offscreen on that same device.
        m_gpu = GpuContext::Create(m_config);
        if (!m_gpu)
        {
            ARC_ERROR("Arcane Editor: GPU context create failed");
            return false;
        }

        ARC_INFO("{} -- Arcane Editor host, backend {}", Arcane::BuildInfo(),
                 Arcane::ToString(m_config.backend));
        ctx.gpu = m_gpu.get();

        // Capture the editor's own ImGui context right after GpuContext::
        // Create built it -- the ONLY ImGui context in existence at this
        // point (the plugin has not loaded yet; StageEditorShell asserts
        // against this, see its own body).
        m_editorImguiContext = ImGui::GetCurrentContext();

        // NO PRESENTER IS CONSTRUCTED HERE. BootSequence's per-stage pump is
        // driven by the pre-device splash (Arcane::BootSplashPresenter, bound
        // for the whole Run() call below) for the ENTIRE boot -- this host has
        // no swapchain-backed presenter at all, so there is no "next automatic
        // present()" that could land on an unready ImGui context.
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
        //
        // THE TOOLBAR MARK IS NOT SET HERE, because no device exists yet --
        // CreateGraphVehicles decodes the same PNG at the same maxSize
        // through LoadDisplayPixels and uploads it through the chrome
        // context's NriTextureCache. It cannot happen HERE because that
        // context does not exist yet: this stage runs inside boot, and the
        // graph vehicles are built strictly after it (Main -> CreateGraphVehicles).
        // The toolbar reads ToolbarLogoTextureId, and 0 is the
        // degraded-but-never-broken "no mark" path a missing PNG takes.

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
        // THERE IS NO RENDER-RESOURCES BRIDGE -- same as
        // RuntimeApp::StageRenderBridge states. A plugin is handed no graphics
        // device and no shader library; Runtime has no SetRenderResources at
        // all.
        //
        // The Assets facade stays device-less either way, and Assets::PixelsFor
        // is the retained, device-FREE decode the graph's NriTextureCache uploads
        // from, so a textured sprite still renders. The stage is kept for the
        // game-context ImGui handoff below.

        // The hosted plugin draws its debug UI into its OWN "game" ImGui context,
        // composited INTO the viewport texture (see MainLoop), instead of the
        // editor context where a HUD would float over the editor chrome. Created
        // here, AFTER the editor ImGui layer is up (StageGpuCore) and BEFORE
        // the plugin is loaded/adopts it in StagePluginLoad. (Create leaves
        // the current ImGui context null; harmless -- the editor's ImGuiLayer
        // re-pins its own context on every BeginFrame/WantCapture*.)
        //
        // ONE FACTORY: OffscreenImGuiLayer::Create() builds a bare CONTEXT --
        // own atlas, io.IniFilename = nullptr, the same pinning discipline in
        // every entry point -- and no renderer. The renderer is a node inside
        // the viewport's frame graph (ImGuiNriNode over FrameDesc::gameUi).
        //
        // NON-OPTIONAL: see the SetImGui block below for what handing the
        // plugin the EDITOR's context instead would cost.
        m_gameImgui = Arcane::OffscreenImGuiLayer::Create();
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
            // ===== THE GAME CONTEXT, UNCONDITIONALLY =====
            // There is deliberately NO `m_gameImgui ? Context() :
            // m_editorImguiContext` fallback here.
            //
            // WHAT SUCH A FALLBACK WOULD COST -- and it is NOT only "the HUD
            // draws over the chrome". OffscreenImGuiLayer sets
            // io.IniFilename = nullptr on the game context ("no imgui.ini for
            // the game context", OffscreenImGuiLayer.cpp) -- while the EDITOR
            // context's IniFilename is the PER-PROJECT LAYOUT FILE
            // (RetargetLayoutIni, below). A plugin window submitted through the
            // fallback would have had its position, size and dock node
            // PERSISTED into the user's project layout, permanently, and per-id
            // ini state silently overrides authored UI on every later boot --
            // the imgui.ini veto class, which is the worst kind of bug this
            // file can produce because it survives a rebuild.
            //
            // Holding that line by CONTROL FLOW -- gating the call site that
            // submits plugin windows -- is a weaker guarantee than holding it
            // by STRUCTURE. It is structural here: there is no path on which
            // the plugin sees m_editorImguiContext.
            m_runtime->SetImGui(m_gameImgui->Context(),
                                reinterpret_cast<void*>(allocFn),
                                reinterpret_cast<void*>(freeFn),
                                ud);
        }

        // Scene-in-a-panel viewport vehicle. The device is up by here in both
        // the interactive host and a `--frames N` run (which only
        // differs in the audio backend).
        //
        // THIS STAGE CREATES NOTHING. The
        // offscreen NriGraphContext that is the viewport vehicle
        // (ViewportTargets::graph) borrows the NriDevice the CHROME context
        // owns, and that context cannot exist yet: its swapchain must be
        // built over an already-Show()n window, which is strictly after boot.
        // Main() -> CreateGraphVehicles() builds both, in that order, before
        // the first frame. Nothing between here and there renders, so the gap
        // is not observable.
        //
        // sprite_tables is unaffected by that: its batcher is m_gpu->Batch()
        // -- the device-less Batcher2D GpuContext::Create built -- exactly as
        // RuntimeApp::StageSpriteTables does it.
        //
        // THE STAGE IS STILL INSTALLED even though its body creates nothing,
        // and removing it is its own change with its own blast radius:
        // "render_bridge" is pinned by name in ArcaneTests/src/
        // BootStageParityTest.cpp's id list (:118), and BOTH sprite_tables and
        // plugin_load declare a dependsOn edge to it (ProjectBoot.cpp:238,
        // :239), so deleting the stage re-parents them.
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
        // A missing dxcompiler.dll degrades to a warn (documents show
        // status).
        //
        // THE DEBOUNCE IS A LIVE CLOCK DEPENDENCY, worth knowing before any
        // caller pins `frame.now`: ShaderCompiler::Submit stamps `readyAt =
        // now + debounce`, so a driver that holds `now` CONSTANT can never
        // cross it, IsIdle() never returns true, and anything waiting for the
        // compile service to settle spins until its own timeout. A caller that
        // pins the clock must zero the debounce too.
        m_shaderCompiler = std::make_unique<Arcane::ShaderCompiler>();
        if (!m_shaderCompiler->Initialize(/*debounceSeconds=*/0.2))
        {
            ARC_WARN("Arcane Editor: dxcompiler.dll unavailable -- material editing disabled");
        }
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

        // GPU crash diagnostics arc, Task 10: .arcdiag -> CrashReportDocument
        // routing, registered right beside the .arcmat route above (same
        // factory+peek shape -- Diag::ReadFile stands in for LoadMaterialAsset,
        // both independently re-read the file, same as materialFactory/
        // materialPeek do today). CrashReportDocument is read-only (never
        // dirty), so unlike the sprite/material routes it needs no
        // DocServices/Services borrow from the app at all.
        const auto crashReportFactory =
            [](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto envelope = Arcane::Diag::ReadFile(p);
                if (!envelope)
                    return nullptr;
                return std::make_unique<Arcane::Editor::CrashReportDocument>(
                    p, std::move(*envelope));
            };
        const auto crashReportPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto envelope = Arcane::Diag::ReadFile(p);
                return envelope ? envelope->guid : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcdiag", crashReportFactory, crashReportPeek);

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

        // F2a, Task 9: .arcmesh -> MeshDocument routing, registered right
        // beside the .arcsprite route above (same factory+peek shape). The
        // preview-seam wiring (nriDevice/hostConfig/chromeHud/
        // retireGraphPreview) is the same four fields MakeDocServices sets
        // for the shader documents (EditorAppProject.cpp:57-94, read there
        // for the full ordering argument) -- inlined here rather than shared,
        // because MeshDocument::Services is deliberately its own small
        // struct (not a DocServices), matching SpriteDocument::Services'
        // precedent of not reusing the shader editor's shape either.
        const auto meshFactory =
            [this](const std::filesystem::path& p)
                -> std::unique_ptr<Arcane::Editor::EditorDocument>
            {
                auto data = Arcane::LoadMeshAsset(p);
                if (!data)
                    return nullptr;
                Arcane::Editor::MeshDocument::Services meshDocServices;
                meshDocServices.runtime = &*m_runtime;
                meshDocServices.undo = m_undo ? &*m_undo : nullptr;
                // Evict-then-re-resolve on a mesh re-save OR an undo/redo,
                // the same one-call route the .arcsprite factory above takes
                // for its own asset: the no-gap requirement (no frame may
                // render between the erase and the re-resolve -- and for a
                // mesh that would mean drawing NOTHING, since there is no
                // placeholder) is SceneRenderResolver::InvalidateMesh's
                // contract, not this lambda's.
                meshDocServices.invalidateMesh = [this](const Arcane::Guid& g)
                {
                    if (m_resolver)
                        m_resolver->InvalidateMesh(g);
                };
                if (ChromeGraph())
                {
                    meshDocServices.nriDevice  = &ChromeGraph()->Device();
                    meshDocServices.hostConfig = &m_config;
                    meshDocServices.chromeHud  = ChromeGraph()->ImGuiHud();
                    meshDocServices.retireGraphPreview =
                        [this](std::unique_ptr<Arcane::NriGraphContext> v)
                    {
                        RetireDocPreview(std::move(v));
                    };
                }
                return std::make_unique<Arcane::Editor::MeshDocument>(
                    std::move(meshDocServices), p, std::move(*data));
            };
        const auto meshPeek =
            [](const std::filesystem::path& p) -> Arcane::Guid
            {
                const auto data = Arcane::LoadMeshAsset(p);
                return data ? data->id : Arcane::Guid::Nil();
            };
        m_documents.RegisterFactory(".arcmesh", meshFactory, meshPeek);

        // Scene asset resolution (sprite-resolution lift): ONE engine-side
        // service resolves everything a scene references into what the
        // submission path can bind -- sprites (.arcsprite -> texture/UVs/size/
        // pivot), sprite materials and the post chain (.arcmat -> a registered
        // batcher material / a bound post chain, published as PostChainDesc
        // bytes for the graph recorder). It owns the three caches, the asset
        // resolver, and the compile drain site; the editor keeps only the
        // compile SERVICE (documents submit through it) and hands the resolver
        // the document routing below.
        {
            Arcane::SceneRenderResolver::Services rs;
            rs.runtime  = &*m_runtime;
            // THE SCENE BATCHER (material binds): the device-less GpuContext
            // one, the only one there is. The lines below are
            // RuntimeApp::StageSpriteTables' gates verbatim, and for its
            // stated reasons: SpriteMaterialCache registers BYTES-ONLY
            // materials with a device-less batcher, PostChainCache publishes
            // its PostChainDesc without building any device-side chain, and
            // the BACKEND -- which selects the shader flavor the compiles
            // target -- comes from the config because there is no device to
            // ask.
            rs.batcher  = &m_gpu->Batch();
            // Only the backend is set: Services carries no device field.
            rs.backend  = m_config.backend;
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
        // demand via --plugin Sandbox.dll or --project ReferenceProject.
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
                // though it never let the developer in to fix anything. Instead:
                // m_plugin.reset() leaves EXACTLY the same safe, disengaged state
                // the "no game module" branch below produces on purpose (every
                // m_plugin-> use in MainLoop is optional-guarded, per this
                // function's opening comment), and a detailed banner -- naming
                // the required ABI -- surfaces through m_modalErrors as the
                // "Open Project Failed" modal (EditorAppFrame.cpp) once MainLoop
                // starts, rather than only a Console line. Since Task 12
                // (EditorAppProject.cpp) SwitchProject no longer has its own
                // switch_plugin_load stage -- it MOVES this exact StagePluginLoad
                // body (this whole function) and runs it verbatim, so a switch
                // failure hits this SAME branch rather than a mirrored copy.
                ARC_ERROR("Arcane Editor: failed to load the game module / project plugins");
                m_modalErrors.Push("Open Project Failed",
                                     "The project opened, but its game module / plugins "
                                     "failed to load (see Console).\nCheck the DLL paths in "
                                     "the manifest and that they are built against ABI " +
                                     std::to_string(static_cast<int>(Arcane::kGamePluginABIVersion)) + ".");
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
            m_modalErrors.Push("Open Project Failed", "--project '" + m_config.projectPath +
                                 "' failed to open.\nRunning with the data/ + --plugin "
                                 "fallback instead (see Console).");
        }

        // Task 7's boot-scene handoff / EnsureScene / title / recents-record --
        // the project-open success tail. SwitchProject's plugin_load stage
        // (EditorAppProject.cpp, which since Task 12 reuses this exact
        // StagePluginLoad body -- there is no separate switch_plugin_load
        // stage anymore) calls this same OnProjectOpened function directly,
        // not a duplicated copy. See OnProjectOpened.
        OnProjectOpened();

        // Point ImGui's ini at this project's appdata layout file BEFORE the
        // first NewFrame reads it (MainLoop starts after boot) -- ImGui then
        // auto-loads the per-project layout on frame one. Not folded into
        // OnProjectOpened: the switch failure fallback also calls that tail
        // (recordRecents=false) but must retarget onto a DIFFERENT key (the
        // reverted/project-less state, not "this boot's project"), so every
        // call site keeps its own RetargetLayoutIni() immediately after.
        RetargetLayoutIni();
        // Same call-site family (GPU crash diagnostics arc, Task 8): a crash/
        // hang report from THIS boot must land under THIS project's own
        // Saved/Diagnostics, not the exe-relative default a project-less
        // boot would otherwise leave armed. See RetargetDumpDir's own
        // comment below for the <project>/Saved/Diagnostics vs default split.
        RetargetDumpDir();
        return true;
    }

    // The project-open SUCCESS tail (architecture pass sec 5) -- previously
    // duplicated verbatim across StageFinalize and a hand-rolled
    // switch_plugin_load stage, with a partial third copy in the switch
    // failure fallback. Now a single function called from all three sites:
    // StageFinalize, SwitchProject's plugin_load stage (which since Task 12
    // reuses this same StagePluginLoad body -- switch_plugin_load no longer
    // exists as a separate stage), and the switch failure fallback.
    // recordRecents=false is the fallback's case: it re-establishes the
    // PROJECT-LESS baseline (or the old project), and a refused open must
    // never reorder the recents lists.
    void EditorApp::OnProjectOpened(bool recordRecents)
    {
        // Task 7: open into the project's boot scene, now that the plugin has
        // loaded (a scene naming a component the game module registers would
        // otherwise silently drop it) and m_undo exists (Adopt records the
        // clean baseline against it). A project with no boot scene, or one
        // that fails to resolve/load, keeps whatever the plugin's Init built --
        // code-spawned scenes are legacy, but nothing clears them unless a
        // scene actually takes ownership (BootScene already logged the reason).
        if (m_undo)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                if (const auto boot = Arcane::HostBoot::BootScene(*m_runtime, *proj))
                {
                    m_scene.Adopt(boot->file, boot->id, *m_undo);
                    m_frameOnSceneOpen = true;
                }
            }
        }
        EnsureScene();
        // Compute the real title now that project/scene state is final,
        // rather than the "Untitled" placeholder a mid-boot call would have
        // shown for one MainLoop tick (UpdateWindowTitle also runs every
        // frame -- EditorAppFrame.cpp -- so this is a courtesy, not the only
        // call site).
        UpdateWindowTitle();
        // Record every call except a refused switch -- recordRecents == false
        // remains the refused-open case (must never reorder Open Recent). A
        // null project (project-less boot) now flows through to
        // NoteProjectOpened's null-project prefill branch (EditorRecents.cpp),
        // restoring the pre-pass behavior: the first File-menu frame is
        // already populated instead of showing an empty cache.
        if (recordRecents)
            m_recents.NoteProjectOpened(m_runtime->CurrentProject());
    }

    void EditorApp::RetargetLayoutIni()
    {
        // ===== PINNED UNDER --headless, AHEAD OF EVERYTHING ELSE =============
        // The per-project layout is USER STATE: it differs per machine, it is
        // whatever this desk last arranged, and the flush below WRITES IT BACK
        // on every project switch (and ~ImGuiLayer saves it again at exit). An
        // agent run would therefore both READ a layout it did not choose and
        // MUTATE it, making run N+1 differ from run N for a reason that has
        // nothing to do with the engine. That is the imgui.ini veto class this
        // file already calls "the worst kind of bug" (see the game-context
        // block above); the pinning precedent is OffscreenImGuiLayer's own
        // io.IniFilename = nullptr.
        //
        // FIRST STATEMENT IN THE FUNCTION on purpose: every path below carries
        // an early `return` (no LOCALAPPDATA, an unwritable dir, an unchanged
        // target), and a pin placed after any of them would be silently
        // skipped on exactly the machines least able to notice.
        //
        // BOTH HALVES ARE REQUIRED. A null IniFilename ALONE leaves the editor
        // with ImGui's built-in defaults -- deterministic, but every panel a
        // free-floating window over an empty dockspace. So exactly one
        // COMMITTED seed is loaded explicitly instead, and nothing is written
        // back: ImGui auto-saves only to io.IniFilename, which is null, and
        // both explicit SaveIniSettingsToDisk call sites in this host are
        // unreachable from here.
        //
        // A MISSING SEED IS NOT AN ERROR -- LoadIniSettingsFromDisk on an
        // absent path is a silent no-op inside ImGui, landing on those same
        // built-in defaults. Degraded, never broken, and still deterministic:
        // the file is either committed or absent for every run alike.
        if (m_config.headless)
        {
            ImGuiIO& io    = ImGui::GetIO();
            io.IniFilename = nullptr;               // never read, never written
            m_layoutIniPath.clear();                // and nothing for a flush to name

            const Arcane::Project* pinProj = m_runtime ? m_runtime->CurrentProject() : nullptr;
            if (!pinProj)
                return;                             // project-less: defaults, and no seed to find

            const std::filesystem::path seed = pinProj->Root() / "Saved" / "verify-layout.ini";
            std::error_code seedEc;
            if (std::filesystem::exists(seed, seedEc))
            {
                ImGui::LoadIniSettingsFromDisk(seed.string().c_str());
                ARC_INFO("--headless: layout PINNED to the committed seed {} "
                         "(io.IniFilename null -- never read, never written)", seed.string());
            }
            else
            {
                ARC_WARN("--headless: no layout seed at {} -- falling back to ImGui's built-in "
                         "defaults (deterministic, but not the authored layout)", seed.string());
            }
            return;
        }

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
        //
        // UNREACHABLE UNDER --headless: the pin at the top of this function
        // returns before this line, and it leaves m_layoutIniPath EMPTY and
        // io.IniFilename NULL -- so even a hypothetical fall-through fails
        // both halves of the condition. That is the Step 2 guard, held by an
        // early return plus the state it leaves behind rather than by a third
        // clause here.
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

    void EditorApp::RetargetDumpDir()
    {
        // <project>/Saved/Diagnostics: same "Saved/ is per-project, untracked
        // scratch" precedent WriteAutoScreenshot already uses (proj->Root() /
        // "Saved" / "AutoScreenshot.png", below) -- a crash/hang report keeps
        // company with the project it came from. project-less (boot with no
        // --project, or a failed switch's fallback -- see every call site of
        // this function) converges on an EMPTY path, which
        // Diagnostics::RetargetDumpDir forwards straight into
        // Config::dumpDir; ReportDir() already treats an empty dumpDir as
        // "<exe dir>/diagnostics" (Diagnostics.hpp's Config comment), so this
        // never re-derives that fallback itself.
        const Arcane::Project* proj = m_runtime ? m_runtime->CurrentProject() : nullptr;
        Arcane::Diagnostics::RetargetDumpDir(proj ? proj->Root() / "Saved" / "Diagnostics"
                                                   : std::filesystem::path{});
    }

    bool EditorApp::StageSplashReady(Arcane::HostBoot::BootContext&)
    {
        // THE SPLASH CARRIES THE LOADING UI, not the editor window. This
        // stage depends on "finalize" (ProjectBoot.cpp's
        // EditorStages), so it is the LAST stage the editor runs -- the
        // window is revealed only once boot is actually finished, matching
        // UnrealEdGlobals.cpp:215-236 ("Hide the splash screen now that
        // everything is ready to go" -> Hide() -> "Do final set up on the
        // editor frame and show it" -> CreateDefaultMainFrame).
        //
        // THE REVEAL IS NOT IN THIS STAGE. CreateGraphVehicles() reveals the
        // window, immediately before building the vehicle that owns its only
        // swapchain -- which satisfies both constraints at once: never show a
        // window nothing can draw into, and never build a surface against a
        // window the compositor has not mapped (the Show()-then-create
        // ordering three desk checkpoints proved).
        //
        // What this stage does is the splash Disarm/Close below: the end of
        // boot.

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
        // THE BACKEND NAME: there is no RenderDevice to ask -- GpuContext
        // builds none -- so this reads the config, the same substitution
        // RuntimeApp::StageSpriteTables makes for the resolver's shader
        // flavor.
        std::string title = EditorTitle(m_runtime ? m_runtime->CurrentProject() : nullptr,
                                        m_scene.DisplayName(), dirty,
                                        !m_gpu ? "" : Arcane::ToString(m_config.backend));
        if (title == m_windowTitle)
            return;
        m_windowTitle = std::move(title);
        m_gpu->Win().SetTitle(m_windowTitle);
    }

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
    // it. Needs only Log::Engine() and `console`, both live at this point, so
    // it has no stage dependency of its own. Bonus: this also means the
    // Console now captures runtime_create/gpu_core's banner lines, which it
    // used to miss because the sink installed after they ran.
    //
    // Both sinks are installed from Create(), not Init(), and Uninstall()
    // (called from the top of Shutdown()) removes them unconditionally -- so
    // a Create-failed path still gets its ARC_ERROR captured by the Console
    // and the diagnostics store.
    //
    // The diagnostics-store half (the store.InstallAsEngineSink() call
    // below): Arcane::Diagnostics' sink slot is mutex-guarded (unlike
    // Log::Engine()->sinks()), but a worker stage (e.g. project_open's
    // content scan) can still publish diagnostics before any BootSequence
    // exists, so it installs from the same early, dependency-free point.
    void EditorApp::ConsoleDiagnostics::Install()
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
                console.Push(std::move(e));
            });
        sink = cb;
        Arcane::Log::Engine()->sinks().push_back(cb);
        store.InstallAsEngineSink();
    }

    // Deregister the diagnostics sink FIRST, same reason as the console sink
    // erase right below: nothing may dispatch into a half-torn-down editor.
    // UninstallEngineSink is identity-guarded (ClearSinkIfCurrent), so this
    // is a no-op if some other DiagnosticStore has since become the
    // process-wide sink.
    //
    // Deregister the console sink SECOND, before anything below can log
    // through Arcane::Log::Engine(). ConsoleDiagnostics is declared after
    // m_runtime/m_plugin/m_gpu in EditorApp.hpp, so it destructs BEFORE them;
    // if the sink outlived this point, a log emitted during ~GpuContext's
    // Vulkan device teardown (validation messages) would invoke the callback
    // and Push into an already-destroyed console deque.
    void EditorApp::ConsoleDiagnostics::Uninstall()
    {
        store.UninstallEngineSink();

        if (sink)
        {
            auto& sinks = Arcane::Log::Engine()->sinks();
            sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());
            sink.reset();
        }
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
        if (!m_runtime || !m_gpu) return;

        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj) return;

        const std::filesystem::path file = proj->Root() / "Saved" / "AutoScreenshot.png";

        // ===== THE COVER CAPTURE =============================================
        // The vehicle's own capture path, FrameDesc::capture + ReadCapture,
        // taken on the VIEWPORT context and NEVER on the chrome one -- an
        // editor cover is a picture of the SCENE, and a capture node on the
        // chrome frame would copy a CHROMED backbuffer and silently redefine
        // what a cover contains (see PresentChromeFrame's four absences).
        //
        // A null `graph` means a vehicle that failed to build (a project
        // switch's rebuild failure): such a session simply writes no cover,
        // and the Hub keeps the previous one -- the same degradation a failed
        // PNG encode already takes.
        if (m_viewportTargets.graph && CaptureGraphViewportPng(file))
            ARC_INFO("Auto-screenshot {}", file.generic_string());
    }

    // ONE MORE VIEWPORT FRAME, ARMED FOR CAPTURE, then read it back.
    //
    // WHY A FRESH FRAME RATHER THAN A READ OF THE LAST ONE. The graph has no
    // "read this texture" entry point at all: a capture is a NODE
    // (RgUsage::CopySrc into an imported HOST_READBACK buffer), declared with
    // the frame it belongs to, and ReadCapture refuses outright unless a
    // submitted frame recorded one (m_captureRecorded). Arming capture on
    // EVERY viewport frame instead would pay a full-surface copy per frame for
    // an image taken twice a session.
    //
    // AND THE FRESH FRAME IS THE BETTER PICTURE, which is the part worth
    // stating: it carries the scene, the post chain and the Edit-mode overlays
    // (SubmitSceneToBatcher's gizmo + camera rect) but NOT the selection
    // outline and NOT the game HUD, because it does not call
    // ArmGraphViewportFrame. WriteAutoScreenshot's own comment calls the
    // outline-free shot the one it would rather have, and rejects it because
    // "an outline-free shot would need a second scene render" -- on this arm
    // the second scene render is what a capture costs anyway.
    //
    // BOTH CALL SITES ARE RARE EVENTS (a scene save, shutdown), which is what
    // pays for ReadCapture's DeviceWaitIdle -- the same stall SaveTexturePng
    // makes on the other arm, for the same reason.
    bool EditorApp::CaptureGraphViewportPng(const std::filesystem::path& file)
    {
        Arcane::NriGraphContext* graph = m_viewportTargets.graph.get();
        if (!graph)
            return false;

        // The SAME submission phase 10 makes, through the same function, so
        // the cover cannot drift from what the viewport shows. Re-Begin is
        // safe at either call site: phase 10's own frame already drained this
        // batcher, and the next frame's phase 10 re-Begins it again.
        Arcane::Batcher2D& b = m_gpu->Batch();
        b.Begin(graph->SurfaceWidth(), graph->SurfaceHeight());
        SubmitSceneToBatcher(b);

        const Arcane::GlobalParams globals =
            m_resolver ? m_resolver->Globals() : Arcane::GlobalParams{};

        Arcane::NriGraphContext::FrameDesc vp;
        vp.batch   = &b;
        vp.post    = m_resolver ? m_resolver->PostDesc() : nullptr;
        vp.globals = &globals;
        vp.capture = true;
        if (graph->RenderFrameOffscreen(vp) != Arcane::NriGraphContext::FrameOutcome::Presented)
            return false;   // Skipped (collapsed panel) or Failed (already latched + logged)

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // ReadCapture hands back TIGHT RGBA8, already swizzled out of the
        // output's BGRA, so these bytes are directly the ones a PNG wants.
        // A false here is NOT a run failure (its own contract says so).
        if (!graph->ReadCapture(w, h, rgba))
            return false;

        // 512 wide, and every other rule about what a cover is, through the
        // ONE shared definition (Arcane::WriteThumbnailPngRgba).
        return Arcane::WriteThumbnailPngRgba(file, w, h, std::move(rgba), 512);
    }

    namespace
    {
        using StageFn = bool (Arcane::Editor::EditorApp::*)(Arcane::HostBoot::BootContext&);
        struct HostStagePatch { std::string_view id; StageFn fn; };
    }

    // THE host-owned stage patch (architecture pass sec 5): Create() applies it
    // to the boot list; SwitchProject applies it to the same shared
    // EditorStages(ctx) list before cherry-picking its subset -- ONE patch
    // path, both directions of drift still fail loudly (see the table comment).
    bool EditorApp::PatchHostStages(std::vector<Arcane::BootStage>& stages)
    {
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

        // Iterate the TABLE, not the stage list: a patch that matches nothing is
        // then just a failed lookup at the point it mattered, with no parallel
        // "did I apply this one" array to keep in sync.
        for (const HostStagePatch& patch : kHostStages)
        {
            const auto it = std::ranges::find(stages, patch.id,
                [](const Arcane::BootStage& s) { return std::string_view(s.id); });
            if (it == stages.end())
            {
                ARC_ERROR("EditorApp::PatchHostStages: patch '{}' matched no stage in "
                          "EditorStages() -- renamed/removed in ProjectBoot.cpp without "
                          "updating this table", patch.id);
                return false;
            }
            it->run = [this, fn = patch.fn] { return (this->*fn)(m_bootCtx); };
        }
        return true;
    }

    bool EditorApp::Create()
    {
        // Installed before HostBoot::EditorStages(ctx) builds the stage list
        // and before any BootSequence exists -- see
        // ConsoleDiagnostics::Install()'s doc comment for why this early,
        // dependency-free point matters, and ConsoleDiagnostics::Uninstall()'s
        // for why Shutdown() removes both sinks unconditionally.
        m_consoleDiag.Install();

        // Report-written hook (GPU crash diagnostics arc, Task 9): mirrors
        // GpuSectionProvider's install pattern (Diagnostics.hpp) -- one call
        // per host lifetime, installed at this same early, dependency-free
        // point so a report written during boot (a boot-stage hang) is
        // still caught, not just one written mid-session. OnReportWritten
        // only ever touches m_pendingReportsMutex/m_pendingReports (see
        // EditorApp.hpp's Report-written notify section), so it is safe to
        // arm before m_runtime or anything else below exists.
        Arcane::Diagnostics::SetReportWrittenHook(&EditorApp::OnReportWritten, this);

        m_bootCtx.runtime     = nullptr;              // stages populate as they go
        m_bootCtx.splash      = m_splash;
        m_bootCtx.projectPath = m_config.projectPath.c_str();
        m_bootCtx.pluginPath  = m_config.pluginPath.c_str();
        m_bootCtx.moduleName  = "ArcaneEditor.exe";
        // Per-open engine settings, from THIS host's parsed command line. THE
        // EDITOR LANE OF THE GOLDEN GATE RUNS THROUGH HERE: a verify run
        // (--headless with --compare/--report) declines the diag:// mount, so
        // the Assets panel in the captured frame cannot enumerate this
        // machine's own crash history -- the 24-pixel scrollbar-thumb diff
        // that demoted editor-ui to advisory. Same shared rule the runtime
        // host uses (HostBoot::OpenOptionsFor), never a second copy.
        m_bootCtx.openOptions = Arcane::HostBoot::OpenOptionsFor(m_config);

        // Spec sec 6: the editor ALWAYS shows boot progress, regardless of any
        // opened project's manifest (project_open's shared CoreStages body
        // never touches showProgress at all -- only RuntimeStages' override
        // does, see ProjectBoot.cpp). BootSplashWindow::ShowProgress already
        // defaults true, so this is redundant with that default today; set
        // explicitly anyway so the editor's intent reads from this file
        // rather than depending on a default it does not own.
        if (m_splash) m_splash->SetShowProgress(true);

        // HostBoot::EditorStages(ctx) is the SAME shared function
        // BootStageParityTest exercises and RuntimeApp calls for its own list
        // (RuntimeApp::Run) -- this is the literal call that keeps the two hosts
        // from silently diverging on which steps exist. This is the one deviation
        // from the brief's literal one-line Run(): a direct
        // `BootSequence seq(EditorStages(ctx))` cannot reach EditorApp's private
        // members from inside Arcane.dll, so the vector is captured, patched via
        // PatchHostStages, then moved into BootSequence -- ids/deps/policy/thread/
        // weight (the actual DAG shape the parity test polices) are untouched.
        std::vector<Arcane::BootStage> stages = Arcane::HostBoot::EditorStages(m_bootCtx);

        if (!PatchHostStages(stages))
            return false;

        m_bootSeq.emplace(std::move(stages));
        return true;
    }

    EditorApp::InitResult EditorApp::Init()
    {
        ARC_ASSERT(m_bootSeq.has_value(), "EditorApp::Init called before Create()");

        // m_splashPresenter is BootSequence's presenter for the WHOLE run
        // -- from runtime_create through finalize, every per-stage
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

    // ---- The render vehicles ------------------------------------------------
    // The editor's counterpart of the block at the top of RuntimeApp::MainLoop,
    // and deliberately the same shape: reveal the window, then build the
    // host-window ("chrome") context that owns this process's ONLY graphics
    // device, then -- unlike the runtime, which has no panel -- build the
    // OFFSCREEN context the Viewport panel samples, on that same device.
    //
    // WHY BOTH LIVE HERE RATHER THAN IN A BOOT STAGE. The reveal has to precede
    // the swapchain create (a surface built against a window the compositor has
    // never mapped is a backend corner a desk-only machine cannot pre-clear --
    // the order three Phase-2 desk checkpoints proved), and the editor's reveal
    // is the LAST boot stage. So the earliest honest point for the chrome
    // context is after boot, and the offscreen one cannot precede it: it
    // BORROWS the device the chrome context creates.
    // See the declaration: one place resolves the live chrome context, so no
    // call site branches on --headless to find it. Exactly one member is ever
    // non-null; both null before CreateGraphVehicles and after ShutdownGraphPath.
    Arcane::NriGraphContext* EditorApp::ChromeGraph() const noexcept
    {
        return m_offscreenChrome ? &m_offscreenChrome->Graph() : m_graphChrome.get();
    }

    bool EditorApp::CreateGraphVehicles()
    {
        // UNCONDITIONAL, AND NOT #if-GUARDED. This body runs in every
        // configuration, Dist included: a preprocessor guard here would leave
        // Dist with m_graphChrome and m_viewportTargets.graph never built
        // while every use site still dereferenced them -- a silent, Dist-only
        // null dereference, which is exactly the failure class an
        // unconditional body forecloses.
        Arcane::Diagnostics::SetPhase("nri graph vehicle boot");

        // THE REVEAL, which no boot stage can do: it has to precede the
        // swapchain create (per the ordering above) and follow everything
        // else. Show() also RAISES, which is the launch reveal this host owes
        // exactly once.
        //
        // ...AND IT IS SKIPPED ENTIRELY UNDER --headless, exactly as
        // RuntimeApp::MainLoop skips its own. The window still EXISTS --
        // GpuContext creates it hidden -- so ImGuiLayer, InputDevices and the
        // event pump keep working unchanged, and io.DisplaySize still resolves
        // off that HWND's pixel size; it is simply never mapped, and no
        // swapchain is ever built over it.
        //
        // Deliberately NOT "hidden window + ordinary swapchain": the ordering
        // note above is the warning against exactly that -- a surface created
        // against a window the compositor has never mapped is the
        // backend-specific corner a desk-only machine cannot pre-clear. The
        // offscreen vehicle below builds no surface at all, which sidesteps
        // the corner instead of walking into it.
        if (!m_config.headless)
            m_gpu->Win().Show();

        // THE LATCH BASELINE, taken HERE rather than at
        // process start for the reason RuntimeApp::MainLoop states for its own:
        // boot-time errors belong to the boot, and everything from this point
        // until the last NRI object is gone belongs to the graph.
        // ShutdownGraphPath reads it back after both contexts are destroyed --
        // a teardown-only validation error must still fail the run.
        m_graphErrorBaseline = Arcane::RenderErrorCount();

        // The chrome context ARMS the crash chain (NriGraphContext::Create ->
        // NriDiagnostics::Arm), exactly as the runtime's single context does.
        // The offscreen one below deliberately does NOT -- Arm/Disarm name one
        // process-wide slot with no owner identity, so a second armer would be
        // harmless but a second DISARMER would unplug this one's chain. That
        // gating lives inside CreateOffscreen; nothing here may disturb it.
        if (m_config.headless)
        {
            // THE EXTENT, from the same place the windowed path's comes from:
            // the window's own pixel size. NriGraphContext::Init hands the
            // borrowed window to the swapchain, which resolves its extent off
            // that HWND -- so reading it here is the SAME number, hidden or
            // shown (SDL tracks a window's size regardless of visibility;
            // GpuContext's 1280x720 WindowDesc default is what both modes
            // therefore start at). It is ALSO what ImGuiLayer reports as
            // io.DisplaySize, so the chrome geometry and the render target
            // agree by construction rather than by coincidence.
            std::uint32_t chromeW = 0, chromeH = 0;
            m_gpu->Win().GetPixelSize(chromeW, chromeH);

            // hostHud ALONE, which is the whole of what a chrome context
            // draws. The host-window twin below declares neither pickOutline
            // nor gameUi ("it draws chrome, and a node it never declares is a
            // readback buffer and a descriptor pool nobody reads"), and this
            // one must match it or the two modes' captures would differ for a
            // reason that is not the editor.
            Arcane::NriGraphContext::NodeSet chromeNodes;
            chromeNodes.hostHud = true;

            m_offscreenChrome = Arcane::OffscreenVehicle::Create(m_config, chromeW, chromeH,
                                                                 chromeNodes);
            if (!m_offscreenChrome)
            {
                // NO SECOND ERROR LINE: Create logs the step that actually
                // failed (adapter / wrap / CreateOffscreen), and a generic
                // restatement here would only push the real cause further up
                // the log. Same contract as RuntimeApp::MainLoop's.
                return false;
            }

            // ===== THE GPU-STALL WATCHDOG IS DELIBERATELY LEFT UNARMED ======
            // RuntimeApp::MainLoop calls SetGpuHeartbeatPublisher(true) on its
            // offscreen vehicle. THIS HOST MUST NOT, and the reason is stated
            // at that call site: the contract is "this offscreen context is
            // the ONLY graph context in this process", and the editor can
            // never satisfy it. Even under --headless it holds FOUR graph
            // contexts -- this chrome one, the viewport
            // (BuildGraphViewportContext), and one apiece for any open
            // ShaderEditorDocument / MeshDocument preview -- so publishing
            // here would make the process's single heartbeat slot a four-way
            // alternation between unrelated fences, which is worse than
            // silence: Diagnostics' GPU-progress rule would compare a value
            // from one device timeline against a threshold set by another.
            //
            // The class CANNOT enforce this; it is caller contract only. So
            // the cost is named rather than papered over: under --headless
            // the editor's GPU-stall watchdog is OFF, a wedged GPU produces
            // no diagnostics capture, and closing that gap needs a different
            // rule ("the frame-driving context publishes") that does not
            // exist yet. The CRASH chain is unaffected -- CreateOffscreen's
            // own gating decides that, and nothing here disturbs it.
        }
        else
        {
            m_graphChrome = Arcane::NriGraphContext::Create(m_config, m_gpu->Win());
        }
        if (!ChromeGraph())
        {
            ARC_ERROR("the editor's chrome graph context could not be created");
            return false;
        }

        // ===== THE CHROME BACKEND'S ADOPTION, MADE EXPLICIT (Task 10) ========
        // It was already CORRECT before this line and it is still correct
        // without it: ImGuiNri::Init installs its backend identity on whatever
        // ImGui context is CURRENT, and that is the editor's -- StageGpuCore
        // left it pinned and every layer in this host re-pins its own context
        // in every entry point, so nothing between there and here can leave
        // another current for the duration of the Create above.
        //
        // IT IS SPELLED OUT ANYWAY, because as of THIS task that implicit
        // adoption stops being inert bookkeeping and starts deciding real
        // behaviour: this backend now RECORDS, and it now services the editor
        // atlas's ImTextureData (create/update/destroy) every frame. Two things
        // hang off which context it believes it serves -- the backend flags
        // (ImGuiBackendFlags_RendererHasTextures, without which a draw list
        // carries no Textures array and the atlas never reaches the GPU) and
        // ImGuiNri::Release, which walks THAT context's platform texture list
        // on teardown. A backend that silently adopted the wrong one would draw
        // nothing and disown someone else's live atlas, with no error anywhere.
        // Task 9's forward note asked for exactly this if the adoption moved;
        // it has not moved, so this is a restatement rather than a fix.
        // Idempotent by construction (AdoptContext pins, re-ORs two flags,
        // restores) and null-safe.
        if (Arcane::ImGuiNriNode* chromeNode = ChromeGraph()->ImGuiHud())
            chromeNode->AdoptImGuiContext(m_editorImguiContext);

        // ===== THE TOOLBAR MARK ==============================================
        // Uploaded through the CHROME context's NriTextureCache, which is the
        // only uploader there is. It cannot happen in StageEditorShell,
        // because no device exists during boot.
        //
        // A SYNTHETIC PER-RUN GUID, because this image is not a project asset
        // -- it is a file beside the exe -- and the cache's whole vocabulary is
        // Guids. Generated rather than hardcoded so it cannot collide with a
        // real asset id in any project; the supply below is the only thing that
        // answers it, and it answers nothing else (a real asset Guid arriving
        // here returns null, which is the cache's own "not resident" path and
        // is correct: the chrome context renders no scene content).
        //
        // ColorSpace::Display is load-bearing -- see NriTextureCache::ColorSpace.
        // maxSize 64 is StageEditorShell's own number, and the same reasoning:
        // ~2x the ~32px on-screen mark, so ImGui's single-tap bilinear only
        // minifies cleanly instead of aliasing a 550px source.
        if (Arcane::LoadDisplayPixels("data/images/arcane_logo.png", 64, m_graphLogoPixels))
        {
            m_graphLogoId = Arcane::Guid::Generate();
            ChromeGraph()->SetPixelSupply(
                [this](const Arcane::Guid& id) -> const Arcane::PixelData*
                {
                    return id == m_graphLogoId ? &m_graphLogoPixels : nullptr;
                });
            if (Arcane::NriTextureCache* cache = ChromeGraph()->Textures())
                if (nri::Texture* logo = cache->Resolve(
                        m_graphLogoId, Arcane::NriTextureCache::ColorSpace::Display))
                {
                    m_graphLogoTexture = (std::uint64_t)(std::intptr_t)logo;
                }
        }

        // THE VIEWPORT, at the boot extent -- 0/0 asks for the default that
        // function names (1280x720; see BuildGraphViewportContext's own
        // comment for why that number).
        //
        // ONE BODY, TWO CALLERS as of Task 12 -- see BuildGraphViewportContext.
        if (!BuildGraphViewportContext(0, 0))
            return false;
        return true;
    }

    // THE VIEWPORT CONTEXT AND EVERY SEAM IT NEEDS, in one body with TWO
    // callers: CreateGraphVehicles above, at boot, and
    // SwitchProject's "render_bridge" stage, which rebuilds this context from
    // scratch on every project switch (see TeardownGraphForSwitch for why the
    // outgoing one cannot be kept).
    //
    // IT IS SHARED RATHER THAN COPIED for the reason every other two-caller
    // body in this host is: a second copy of the wiring is how the switch and
    // the boot silently drift apart, and three of the four statements below
    // are exactly the kind that fail SILENTLY when missed -- a missing
    // AdoptImGuiContext is a blank HUD with no error, a missing resolver or
    // pixel supply is a white texel on every sprite with one WARN a session.
    //
    // NOT #if-guarded, for the reason m_graphChrome's declaration states: a
    // preprocessor-guarded definition forces a guard at every call site, and
    // SwitchProject compiles in every configuration. Both callers reach this
    // body in EVERY configuration, Dist included.
    bool EditorApp::BuildGraphViewportContext(std::uint32_t width, std::uint32_t height)
    {
        if (!ChromeGraph())
        {
            ARC_ERROR("no chrome context -- the viewport context has no device to borrow");
            return false;
        }

        // THE BOOT EXTENT, named ONCE, here, because both callers can want it:
        // boot has no measurement to offer and a switch may have had no
        // outgoing context to measure. The panel's real extent is not known
        // until it has drawn once, and phase 8's deferred resize adopts it on
        // the next frame -- so this number decides one frame's worth of
        // picture and nothing else.
        if (width == 0 || height == 0)
        {
            width  = 1280;
            height = 720;
        }


        // BOTH OPTIONAL NODE SETS. This context is the whole viewport, so it
        // needs both:
        //   * pickOutline -- PickNode + OutlineNode. Asked for here rather
        //     than inferred from --pick-probe because the editor's probe pixel
        //     is a click that has not happened yet and its hover cursor moves
        //     every frame; that flag arms a FIXED pixel and is a different
        //     question.
        //   * gameUi -- the second ImGuiNriNode, which draws the plugin HUD's
        //     ImDrawData between the tonemap and the outline composite. That
        //     is phase 11 then phase 12's order against this recorder.
        // The CHROME context asks for neither: it draws chrome, and a node it
        // never declares is a readback buffer and a descriptor pool nobody
        // reads.
        Arcane::NriGraphContext::NodeSet viewportNodes;
        viewportNodes.pickOutline = true;
        viewportNodes.gameUi      = true;
        m_viewportTargets.graph = Arcane::NriGraphContext::CreateOffscreen(
            m_config, ChromeGraph()->Device(), width, height, viewportNodes);
        if (!m_viewportTargets.graph)
        {
            ARC_ERROR("the editor's offscreen viewport context could not be created");
            return false;
        }

        // THE ONE OBLIGATION A gameUi CONTEXT CARRIES, discharged here because
        // this is the first moment both halves exist. ImGuiNri installs
        // ImGuiBackendFlags_RendererHasTextures on whatever ImGui context is
        // CURRENT when it is built -- which is the EDITOR's, since that is what
        // StageGpuCore left pinned and nothing since has moved it. Without this
        // call the GAME context would never get the flag, its draw lists would
        // carry no `Textures` array for the node to upload from, and the HUD
        // would render as nothing at all -- silently, with no error anywhere.
        // See ImGuiNri::AdoptContext.
        //
        // AND IT IS OWED AGAIN ON EVERY REBUILD, which is the whole reason this
        // body is shared: the node above is a BRAND NEW ImGuiNri that adopted
        // whatever context was current at its Create (the editor's), while
        // m_gameImgui -- deliberately NOT rebuilt by a switch -- is still the
        // context whose draw data it will be handed. Miss this on the switch
        // path and the plugin HUD is blank from the first switch onward, with
        // nothing in the log. Stated at m_gameImgui's declaration too.
        if (Arcane::ImGuiNriNode* gameNode = m_viewportTargets.graph->ImGuiGame())
            gameNode->AdoptImGuiContext(m_gameImgui ? m_gameImgui->Context() : nullptr);

        // The two injected seams the graph path needs to draw REAL content,
        // both copied from RuntimeApp::MainLoop's create block and both on the
        // VIEWPORT context only -- it is the one that renders the scene. The
        // chrome context draws ImGui geometry over textures ImGuiNri owns and
        // resolves no assets at all.
        //
        // Guid -> asset file: the same lambda SceneRenderResolver builds, so it
        // re-reads CurrentProject() per call and survives a project switch.
        // (It survives one on the OBJECT level too, and that is not what makes
        // a rebuild unnecessary -- the CACHES behind these seams are what a
        // switch has to shed. See TeardownGraphForSwitch.)
        m_viewportTargets.graph->SetAssetResolver(
            [rt = &*m_runtime](const Arcane::Guid& id)
                -> std::optional<std::filesystem::path>
            {
                const Arcane::Project* project = rt ? rt->CurrentProject() : nullptr;
                return project ? project->ResolveAsset(Arcane::AssetId::FromGuid(id))
                               : std::nullopt;
            });
        // ...and the same seam extended to PIXELS: NriTextureCache uploads
        // its own textures from the engine's RETAINED, device-free decode.
        m_viewportTargets.graph->SetPixelSupply(
            [rt = &*m_runtime](const Arcane::Guid& id) -> const Arcane::PixelData*
            {
                return rt ? rt->AssetsFacade().PixelsFor(id) : nullptr;
            });
        return true;
    }

    int EditorApp::Main()
    {
        if (!CreateGraphVehicles())
            return 1;
        MainLoop();
        return 0;
    }

    std::uint32_t EditorApp::ViewportWidth() const noexcept
    {
        // A null `graph` (a vehicle that failed to build) answers 0, which
        // every caller already tolerates.
        return m_viewportTargets.graph ? m_viewportTargets.graph->SurfaceWidth() : 0u;
    }

    std::uint32_t EditorApp::ViewportHeight() const noexcept
    {
        // See ViewportWidth().
        return m_viewportTargets.graph ? m_viewportTargets.graph->SurfaceHeight() : 0u;
    }

    void EditorApp::NoteGraphFrameFailure(const char* what)
    {
        // Reported through the "nri-graph" seam already, which means the latch
        // grew and ShutdownGraphPath's read would have produced exit 2 on its
        // own. 1 is the stronger statement -- it says WHERE the run died -- and
        // it is what RuntimeFrame::RenderGraph sets in the same situation, so
        // the two hosts report one vocabulary. Precedence 1 > 2: FIRST failure
        // wins and a later latch growth cannot demote it.
        ARC_ERROR("{}; stopping", what);
        if (m_graphExit == 0)
            m_graphExit = 1;
        // The frame loop's own exit channel (see PumpFrameEvents, which reads
        // this before any phase runs). Used rather than a break because the
        // failure is discovered mid-frame, and the frame's ImGui pairing and
        // its remaining phases have to finish before the loop may leave.
        m_requestExit = true;
    }

    void EditorApp::RetireDocPreview(std::unique_ptr<Arcane::NriGraphContext> vehicle)
    {
        if (vehicle)
            m_retiredDocPreviews.push_back(std::move(vehicle));
    }

    // Destroy every vehicle a document handed over, invalidate FIRST.
    //
    // WHY THE DEFERRAL EXISTS, in one line: the document died at phase 14 and
    // the chrome frame naming its texture was recorded at phase 19. Draining
    // here -- at the top of a LATER frame -- means that frame has been
    // recorded and submitted, and InvalidateUserTextureNow's own
    // DeviceWaitIdle is what makes "submitted" into "retired" before the view
    // is destroyed. See DocServices::retireGraphPreview.
    //
    // The invalidate is the app's rather than the document's because by now
    // the document is gone -- and because it always was the CHROME context's
    // node that had to be told, which only the app holds.
    void EditorApp::DrainRetiredDocPreviews()
    {
        if (m_retiredDocPreviews.empty())
            return;
        Arcane::ImGuiNriNode* chrome = ChromeGraph() ? ChromeGraph()->ImGuiHud() : nullptr;
        for (std::unique_ptr<Arcane::NriGraphContext>& vehicle : m_retiredDocPreviews)
        {
            if (!vehicle)
                continue;
            // Same three-part order as ShutdownGraphPath: view (chrome's lane)
            // before texture (this vehicle's lane), then the borrower dies.
            // Unconditional and idempotent -- a preview that never drew is a
            // routine miss.
            if (chrome)
                (void)chrome->InvalidateUserTextureNow(vehicle->OffscreenOutput());
            vehicle.reset();
        }
        m_retiredDocPreviews.clear();
    }

    namespace
    {
        // --compare / --bless (Task 9). Own private copy -- see
        // EditorAppFrame.cpp's identical helper (used by MainLoop's pre-loop
        // resolve) for the full reasoning; ShutdownGraphPath below needs the
        // same file-path-component spelling to re-resolve a just-blessed
        // reference and to build the diff-artifact path.
        const char* CompareBackendName(Arcane::GraphicsBackend backend)
        {
            return backend == Arcane::GraphicsBackend::Vulkan ? "vulkan" : "dx12";
        }
    }

    // ===================================================================
    // THE PROJECT SWITCH'S RENDER TEARDOWN -- SwitchProject's
    // "switch_teardown" stage, and it is deliberately the
    // sibling of ShutdownGraphPath below rather than a body inside
    // EditorAppProject.cpp: the two sequences must be read against each other,
    // and every clause one of them gets right is a clause the other owes.
    //
    // ============ WHAT IS TORN DOWN AND WHAT IS KEPT, AND WHY ============
    // The rule the split follows: NOTHING PROJECT-SCOPED SURVIVES A SWITCH,
    // NOTHING WINDOW- OR DEVICE-SCOPED IS DESTROYED BY ONE.
    //
    // KEPT -- m_graphChrome, the host-window context. Not one thing it holds
    // belongs to the outgoing project, and that is checkable rather than
    // asserted (CreateGraphVehicles is the whole of its wiring):
    //   * it owns the process's ONE NriDevice and the swapchain bound to the
    //     host window -- both window/session-scoped, and a switch does not
    //     touch the window;
    //   * it ARMED the process-wide crash chain, and NriDiagnostics keeps one
    //     slot with no owner identity -- tearing this context down mid-session
    //     would disarm it and rebuild it, for nothing;
    //   * its ImGuiNri serves the EDITOR ImGui context, which survives;
    //   * its NriTextureCache has exactly ONE possible tenant: the toolbar
    //     logo, keyed by a synthetic per-run Guid answered by a pixel supply
    //     that returns null for every other id. No project asset can enter it.
    //     (The VIEWPORT context is the one that gets the project's resolver +
    //     Assets pixel supply -- BuildGraphViewportContext, and only there.)
    // Destroying it would mean destroying and re-creating a graphics device
    // and a flip-model swapchain on a live HWND mid-session, taking every
    // borrower (the viewport context, every document preview) with it, in
    // exchange for nothing at all.
    //
    // TORN DOWN -- m_viewportTargets.graph, the offscreen viewport context.
    // Its SIZE is panel-scoped (so the replacement is created at the outgoing
    // one's extent, below, and the panel does not snap back to 1280x720), but
    // its CONTENT is project-scoped through and through, and -- this is the
    // part that decides it -- the content lives in caches that HAVE NO
    // INVALIDATION HOOK BY DESIGN:
    //   * NriTextureCache holds nri::Textures uploaded from the OUTGOING
    //     project's asset pixels, keyed by asset Guid. Two projects can carry
    //     the same Guid (copying a project copies its asset ids), so a kept
    //     cache does not merely leak VRAM -- it can serve project A's pixels
    //     for project B's asset. It does have Release(graves, fence)...
    //   * ...but Batch2DNode::EnsureSpriteSet caches a DESCRIPTOR SET per
    //     sprite Guid naming that cache's view, written ONCE and never
    //     rewritten -- the discipline that keeps ResetDescriptorPool and its
    //     fence rules out of that file. Flushing the texture cache under it
    //     would leave those sets naming destroyed views, which is a fault
    //     rather than a stale pixel. Same shape for the material sets and for
    //     PostChainNode's declared-param views.
    // So a "flush the content, keep the object" switch would have to re-open a
    // deliberately-closed discipline in three nodes. Rebuilding the context
    // achieves the same state by construction -- new pool, new sets, new cache
    // -- with no new invalidation surface, and it is what this arc's two
    // contract blocks (NriGraphContext.hpp's TWO CONTEXTS, TWO LANES item (2);
    // ImGuiNri.hpp's caller list) already named as the switch's shape.
    //
    // KEPT, AND IT MUST BE -- m_gameImgui. The viewport context's game
    // ImGuiNriNode ADOPTED it, and ImGuiNri::Release PINS the adopted context
    // to walk its platform texture list, which is a DEREFERENCE. At process
    // exit member order guarantees it; here it has to be written down, because
    // this function destroys a context while every ImGui context in the
    // process survives. Nothing below resets it. The other half of that
    // obligation -- re-adopting it on the REBUILT node -- is discharged in
    // BuildGraphViewportContext, which is why the switch calls that body
    // rather than CreateOffscreen directly.
    //
    // AND THE SPLIT KEEPS "ONE ImGuiNri PER ImGui CONTEXT" TRUE AT EVERY
    // INSTANT, which is NodeSet's invariant and not a tidiness claim: a
    // graph-mode editor holds exactly two backends -- the chrome hub over the
    // EDITOR context and the viewport's game node over the GAME context
    // (document previews are built with NodeSet{} and have none). This
    // function destroys the second, and the rebuild in a LATER stage creates
    // its replacement, so the game context is never served by two. Rebuilding
    // the chrome context would have had to make the same argument for the
    // editor context; keeping it means there is nothing to argue.
    //
    // WHAT THE GAME CONTEXT LOSES ACROSS THE GAP, stated because it looks like
    // damage and is the protocol working: ImGuiNri::Release DISOWNS the
    // adopted context's RefCount==1 ImTextureData, so the game atlas comes out
    // of this marked WantCreate with its CPU pixels intact -- and the rebuilt
    // backend re-uploads it on its first frame through the same 1.92 texture
    // path that created it originally. That is precisely why Release must run
    // while the context is ALIVE, i.e. why obligation (a) exists.
    //
    // ================= THE ORDER, WHICH IS THE POINT =================
    //   1. IDLE. Explicit and unconditional, and owed for one reason: the stage
    //      that runs immediately after this one unloads the plugin, and a
    //      plugin torn down under a GPU still reading resources is the hazard
    //      that line has always existed to prevent. Not folded into the
    //      invalidate below: InvalidateUserTextureNow early-outs on a null
    //      texture BEFORE its own idle, so a session where nothing ever drew
    //      the output would silently skip it.
    //   2. INVALIDATE, on the CHROME hub, for every texture about to die --
    //      the retired document previews (drained here) and the viewport
    //      output. The chrome backend caches by RAW nri::Texture* and NRI does
    //      not ref-count, so the replacement may land on the freed address;
    //      the entry must be evicted while the texture is still alive, and
    //      through the `Now` variant, because view and texture die through
    //      DIFFERENT graveyard lanes and nothing orders two lanes.
    //   3. RELEASE. ~NriGraphContext then runs its own idle -> bury nodes,
    //      caches, graph -> drain over its OWN lane, which is step 4 as well:
    //      the lane is emptied inside that destructor, so nothing of the dead
    //      context is left pending anywhere.
    //
    // AND NOTHING RENDERS BETWEEN STEP 2 AND STEP 3 -- the adjacency rule the
    // resize path states as "(iii) THE PAIR IS ONE OPERATION". A chrome frame
    // recorded in that window would take ImGuiNri's CREATE path and build a
    // fresh view + set over a texture about to be destroyed. It holds here for
    // a stronger reason than adjacency: SwitchProject drives its stages with a
    // NULL presenter on this arm (see the seq.Run call site), so no frame is
    // recorded anywhere between this function and the rebuild.
    //
    // WHAT THIS FUNCTION DELIBERATELY DOES NOT DO: close documents or clear
    // caches. ResetPerProjectState owns that list, runs immediately before
    // this, and is where a new project-scoped member must be registered. This
    // function consumes the RESULT of its CloseAll (the retire list) and adds
    // no second reset list -- audit defect A3 was three of those.
    void EditorApp::TeardownGraphForSwitch(std::uint32_t& keepWidth, std::uint32_t& keepHeight)
    {
        keepWidth  = 0;
        keepHeight = 0;
        if (!ChromeGraph())
            return;   // never built a vehicle -- nothing of this arm exists to tear down

        // The panel's CURRENT extent, carried to the rebuild so the replacement
        // is created at the size the user is looking at rather than at the boot
        // default. Read BEFORE anything is destroyed, obviously, and 0 when
        // there is no viewport context (a rebuild that already failed once) --
        // which the rebuild reads as "use the boot default".
        if (m_viewportTargets.graph)
        {
            keepWidth  = m_viewportTargets.graph->SurfaceWidth();
            keepHeight = m_viewportTargets.graph->SurfaceHeight();
        }

        // ---- 1. IDLE ------------------------------------------------------
        Arcane::NriDevice& device = ChromeGraph()->Device();
        const nri::CoreInterface& core = device.Core();
        if (core.DeviceWaitIdle)
            (void)ARC_NRI_CHECK(core.DeviceWaitIdle(&device.Device()));

        // ---- 2. INVALIDATE ------------------------------------------------
        // The documents ResetPerProjectState just closed handed their preview
        // vehicles to the retire list rather than destroying them inline
        // (DocServices::retireGraphPreview). Draining HERE -- while
        // m_graphChrome, whose node the drain invalidates against, is still
        // alive and before any frame is recorded -- is the same pairing
        // ShutdownGraphPath makes, and for the same reason. Without it the
        // vehicles would sit in the list until the NEXT frame's phase 13
        // drain, i.e. across a plugin unload and a project open, holding
        // device memory for a project that no longer exists.
        DrainRetiredDocPreviews();

        // ...and the viewport output itself, the one descriptor that inherently
        // spans both contexts. Unconditional and idempotent: a miss (nothing
        // ever drew the panel) returns false and buries nothing, a null
        // early-outs.
        if (ChromeGraph()->ImGuiHud() && m_viewportTargets.graph)
        {
            ChromeGraph()->ImGuiHud()->InvalidateUserTextureNow(
                m_viewportTargets.graph->OffscreenOutput());
        }

        // ---- 3. RELEASE (+ its own drain) ---------------------------------
        // The borrower dies; the owner of the device it borrowed does not.
        m_viewportTargets.graph.reset();

        // NO LATCH READ HERE, unlike ShutdownGraphPath -- and the omission is
        // deliberate. m_graphErrorBaseline spans the whole graph session and is
        // read exactly once, at exit; sampling it mid-session would either have
        // to re-baseline (losing errors a switch fired) or report a growth the
        // exit read is about to report again. A validation error raised by this
        // teardown is still caught, at exit, by that one read.
    }

    void EditorApp::ShutdownGraphPath()
    {
        if (!ChromeGraph() && !m_viewportTargets.graph)
            return;   // a run that never built a vehicle

        // ===== OPEN DOCUMENTS FIRST ==========================================
        // A shader document owns its OWN offscreen context, built
        // over the device m_graphChrome owns, and holds m_graphChrome's
        // ImGuiNriNode to invalidate against on the way out. Both of those are
        // gone the moment the two resets below run.
        //
        // MEMBER ORDER DOES NOT COVER IT, and it looks like it should:
        // m_documents is declared AFTER m_graphChrome, so reverse-order
        // destruction genuinely does close documents first -- but that happens
        // in ~EditorApp, long after THIS function has already destroyed both
        // contexts. The window between them is the whole hazard, and it is the
        // same shape as the one this function exists to close: the fix is to
        // close the borrowers at the point the owner is about to die, not to
        // reason about a declaration order that runs later.
        //
        // CloseAll is the document host's own teardown (it is what a project
        // switch runs), so this is not a special exit path -- it is the
        // ordinary one, moved earlier.
        m_documents.CloseAll();
        // ...which hands their preview vehicles to the retire list rather than
        // destroying them inline, so the list has to be drained HERE, while
        // m_graphChrome (whose ImGuiNri node the drain invalidates against) is
        // still alive. No frame is recorded between the two, which is the one
        // condition the deferral exists to satisfy.
        DrainRetiredDocPreviews();

        // ===== THE TEARDOWN HALF OF THE VIEW-BEFORE-TEXTURE RULE =============
        // A RESIZE is not the only moment
        // the viewport's output texture dies -- PROCESS EXIT is the other, and
        // unlike a resize it happens on every single run.
        //
        // WHY MEMBER ORDER CANNOT COVER IT. Reverse-declaration destruction runs
        // m_viewportTargets.graph -- the offscreen context, which OWNS the
        // output -- hundreds of declarations BEFORE m_graphChrome. That order is
        // correct and required for the borrowed DEVICE (the borrower must die
        // first). But the chrome context's ImGuiNri caches that output by RAW
        // nri::Texture*, so ~ImGuiNri disposes its descriptor + set hundreds of
        // declarations LATER: DestroyDescriptor after DestroyTexture, which is
        // exactly the inversion NriGraphContext.hpp's TWO CONTEXTS, TWO LANES
        // calls the part of the rule that "does not go away" (on Vulkan the
        // error raises at image-destroy time). NO declaration order fixes it --
        // the device wants one order and the view wants the opposite.
        //
        // So the caller closes it with the same hook the resize uses, for the
        // same reason: destroy the view while the texture it views is still
        // alive. Unconditional and idempotent -- a routine miss (nothing ever
        // drew the output) returns false and buries nothing, and a null pointer
        // early-outs ahead of the idle.
        //
        // LIVE AS OF TASK 10 -- it was inert while the chrome context rendered
        // nothing, and this is the task that makes it record: the chrome frame
        // draws the Viewport panel's ImGui::Image over OffscreenTextureId(), so
        // there IS a pointer-keyed entry to evict now, on EVERY run. A grown
        // RenderErrorCount here is precisely what the D3b/D3c watch lists read
        // as a finding, and the line below is what makes it legible.
        if (ChromeGraph() && ChromeGraph()->ImGuiHud() && m_viewportTargets.graph)
        {
            ChromeGraph()->ImGuiHud()->InvalidateUserTextureNow(
                m_viewportTargets.graph->OffscreenOutput());
        }

        // ===== THEN THE CONTEXTS, BORROWER FIRST =============================
        // Explicit resets rather than member destruction, for the reason
        // RuntimeApp::ShutdownGraphPath states: the latch has to be sampled
        // strictly AFTER the last NRI object is gone (teardown ordering is
        // exactly the class of mistake a validation layer exists to catch), and
        // member destructors run after Run() has already returned its code.
        //
        // The order is the SAME one declaration order gives, restated because
        // it is now this function's to get right: the viewport context BORROWS
        // the device the chrome context owns, so the borrower dies first. Two
        // other lifetimes bracket this and both are satisfied HERE and not in
        // ~EditorApp: m_gameImgui (whose context the viewport's game
        // ImGuiNriNode adopted, and whose platform texture list ImGuiNri::
        // Release DEREFERENCES) and m_gpu's ImGuiLayer (whose context the
        // chrome node adopted) are both still fully alive at this point in
        // Shutdown -- which is a strictly stronger guarantee than the
        // declaration-order one it replaces.
        m_viewportTargets.graph.reset();
        m_graphChrome.reset();
        // The --headless twin, released in the same statement group and for
        // the same reason -- exactly one of the two was ever built, and
        // resetting the null one is a no-op. It is destroyed AFTER
        // m_viewportTargets.graph because the viewport context BORROWS the
        // device this vehicle owns.
        m_offscreenChrome.reset();

        // ===== AND ONLY THEN THE LATCH =======================================
        // The editor's equivalent of the runtime's line, and the reason it
        // exists: "RenderErrorCount must not grow across a clean --nri-graph
        // editor exit" is the observable proof that the invalidate above
        // actually ordered the view ahead of the texture. Without the pair
        // printed, a desk operator has a rule with nothing to read it against.
        const std::uint64_t errorsNow = Arcane::RenderErrorCount();
        ARC_INFO("[nri-graph] RenderErrorCount {} -> {}", m_graphErrorBaseline, errorsNow);
        if (errorsNow > m_graphErrorBaseline)
        {
            ARC_ERROR("[nri-graph] FAILED: {} validation/render error(s) fired during the editor "
                      "run (teardown included)", errorsNow - m_graphErrorBaseline);
            // Precedence 1 > 2, the runtime's: a frame failure says WHERE the
            // run died and outranks the errors it produced on the way out.
            if (m_graphExit == 0)
                m_graphExit = 2;
        }

        // --settle N (Task 9): FAIL EXPLICITLY on non-convergence -- checked
        // HERE, not only inside the --report block below, because an agent
        // that checks only the PROCESS EXIT CODE (never opens the JSON) must
        // still see this fail. m_settleConverged/m_settleAttemptsUsed are
        // only meaningful when settle was actually requested
        // (m_config.settleAttempts != 0); a run that never asked for it
        // leaves both at their defaults, which must never read as a
        // failure. Precedence 1/2 > 3, matching RuntimeApp::ShutdownGraphPath:
        // a run that already failed for a REAL reason keeps that reason --
        // this only ever promotes an otherwise-clean exit code.
        const bool settleFailed = m_config.settleAttempts != 0 && !m_settleConverged;
        if (settleFailed && m_graphExit == 0)
            m_graphExit = 3;

        // --compare / --bless (Task 9). NOT nested inside the `--report`
        // block below: --settle only requires --screenshot OR --report
        // (HostConfig.cpp), so a run can legally pair --compare/--bless with
        // --screenshot and no --report at all -- the PNG this writes must
        // land regardless of whether a JSON report was ever asked for.
        //
        // Finding 4 of the Task 8 dispatch audit (mirrored here verbatim): a
        // --bless run's settle loop converges on byteEqual && idle ALONE --
        // the compare conjunct is disabled for the whole run
        // (m_compareRequested) -- so m_captureRead here means exactly "the
        // settle loop converged", and THIS is where that converged capture
        // actually becomes the new reference. Never attempted when
        // settleFailed (nothing converged to bless) or when --bless was not
        // given at all.
        bool compareBlessed     = false;
        bool compareWriteFailed = false;
        if (m_config.bless && !m_config.compareReference.empty() && m_captureRead)
        {
            if (Arcane::BlessReference(m_compareResolution, m_captureWidth, m_captureHeight,
                                       m_captureRgba.data()))
            {
                compareBlessed = true;
                ARC_INFO("--bless: wrote reference '{}' ({}x{}) -> {}", m_config.compareReference,
                         m_captureWidth, m_captureHeight, m_compareResolution.blessTarget.string());
            }
            else
            {
                compareWriteFailed = true;
                ARC_ERROR("--bless: failed to write reference to {}",
                          m_compareResolution.blessTarget.string());
                if (m_graphExit == 0)
                    m_graphExit = 4;
            }
        }

        // THE DIFF ARTIFACT (Task 9; HOISTED OUT of the --report block by
        // final-review I-2, in lockstep with RuntimeApp::ShutdownGraphPath --
        // see that function's fuller comment for the reasoning). It sits here,
        // beside the bless write, because --settle requires --screenshot OR
        // --report, so `--compare X --settle N --screenshot out.png` with no
        // --report is legal and used to produce NO diff image at all. The path
        // is carried into SetCompare below when a report WAS asked for; when it
        // was not, the PNG still lands on disk.
        //
        // The guard reproduces the report block's if/else-if chain exactly, so
        // the two sites stay provably in step.
        std::string compareDiffPath;
        if (!m_config.compareReference.empty() && !m_compareMissingFatal && !compareBlessed
            && !compareWriteFailed && m_compareEvaluated && !m_compareResult.passed
            && !m_compareResult.diffRgba.empty())
        {
            const std::filesystem::path diffProjectRoot =
                (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                           : std::filesystem::path{};
            const std::filesystem::path artifact =
                Arcane::DiffArtifactPath(diffProjectRoot, m_config.compareReference,
                                         CompareBackendName(m_config.backend));
            // Empty means DiffArtifactPath REFUSED the name (path-traversal
            // guard) -- never write a path it declined.
            if (!artifact.empty())
            {
                if (Arcane::WritePngRgba(artifact, m_compareResult.width, m_compareResult.height,
                                         m_compareResult.diffRgba.data()))
                {
                    compareDiffPath = artifact.string();
                    ARC_INFO("--compare: diff artifact written to {}", compareDiffPath);
                }
                else
                {
                    ARC_ERROR("--compare: failed to write diff artifact to {}", artifact.string());
                }
            }
        }

        // THE REPORT (Task 9), written LAST in this function so exitReason
        // below can read the FINAL m_graphExit -- including the fold just
        // above, which only settles after the vehicle is gone. WriteTo never
        // touches m_graphExit itself, so this call only DESCRIBES what
        // already happened; Run()'s tail still decides the process exit code
        // exactly as it did before this task.
        //
        // Guarded on reportPath alone (HostConfig.hpp's "Empty = off"): a
        // bare `--report` still writes a valid report (run identity +
        // whatever AddCensus below always contributes). This host writes no
        // `probes` entries and no `pick` field, ever -- --probe stays
        // refused at main.cpp, so ParseProbe/Evaluate have nothing to do
        // here; the report's `probes` array is simply left at its default
        // empty state, matching what an agent would see from a runtime run
        // that also passed no --probe.
        if (!m_config.reportPath.empty())
        {
            const bool completedAllFrames =
                m_config.maxFrames != 0 && m_frameCount >= m_config.maxFrames;

            // Reuses the SAME vocabulary this function's own m_graphExit
            // comment (EditorApp.hpp) documents (1 = a graph frame failed,
            // 2 = RenderErrorCount grew) -- this is not a new
            // classification, just the existing one spelled into the
            // exit-reason string VerifyReport carries. Precedence mirrors
            // RuntimeApp::ShutdownGraphPath's exactly, including checking
            // m_compareMissingFatal and settleFailed BEFORE
            // completedAllFrames: a non-converged settle run has
            // m_frameCount >= m_config.maxFrames by construction (settle
            // attempts only ever run after the base budget is spent), so
            // completedAllFrames reads true regardless of whether the run
            // actually converged -- left unguarded this would silently fall
            // through to "frames-complete", hiding exactly the failure this
            // mode exists to catch.
            std::string exitReason = "frames-complete";
            if (Arcane::GpuDeviceLostObserved())
                exitReason = "device-lost";
            else if (m_graphExit == 1)
                exitReason = "render-failed";
            else if (m_graphExit == 2)
                exitReason = "validation-errors";
            else if (m_compareMissingFatal)
                exitReason = "compare-missing-reference";
            else if (settleFailed)
                exitReason = m_compareEvaluated ? "compare-failed" : "settle-not-converged";
            else if (compareBlessed)
                exitReason = "compare-blessed";
            else if (compareWriteFailed)
                exitReason = "compare-failed";
            else if (!completedAllFrames)
                exitReason = "stopped-early";

            Arcane::VerifyReport report;
            report.SetRun(Arcane::ToString(m_config.backend), m_frameCount, exitReason);

            // Only set when a readback actually landed and became the
            // agreed-on capture this run (the ordinary single-shot tail, or
            // a converged settle attempt) -- an early exit, a run that asked
            // for neither --screenshot nor --report, or an unconverged
            // settle run all leave this false, and SetCapture is skipped so
            // Brightness/Luma/Rgba-style consumers see an honest "no capture
            // set" rather than a phantom frame. (This host evaluates no such
            // probes itself -- see the `probes` note above -- but the same
            // capture section feeds Servitor-side tooling the runtime's
            // report already does.)
            if (m_captureRead)
                report.SetCapture(m_captureWidth, m_captureHeight, m_captureRgba);

            // The --settle verdict (Task 3), ported from RuntimeApp::
            // ShutdownGraphPath's twin call site -- BOTH hosts must emit these
            // keys or the schema-3 fields are simply absent from every editor
            // report, silently, because absence is a legal state in this
            // contract.
            //
            // TWO conditions, mirroring the runtime's: settle asked for, AND
            // the loop actually reached a verdict. This host has the identical
            // ahead-of-the-loop exits (m_compareMissingFatal in the exitReason
            // chain just above, device-lost, render-failed,
            // validation-errors), which leave m_settleBail at Keep -- guarding
            // on the count alone would report "timeout-bound" for a run that
            // never spent a timeout.
            //
            // m_settleAttemptsUsed is passed AS MEASURED. It is honest now:
            // PresentChromeFrame's defensive branch used to stop the loop by
            // forging it to m_config.settleAttempts, and Task 2 replaced that
            // with m_settleAborted -- `++m_settleAttemptsUsed` is the only
            // write left. Reporting the forged value would have made this JSON
            // LIE on this host, which is worse than the field's absence: an
            // absent field is an honest gap, a present wrong one is a false
            // report an agent will trust.
            if (m_config.settleAttempts != 0
                && Arcane::SettleVerdictReached(m_settleConverged, m_settleBail))
            {
                report.SetSettle(m_settleAttemptsUsed, m_settleConverged, m_settleBail,
                                  m_settleCaptureFailed);
            }

            // The census is carried whether or not a `census` PROBE was
            // asked for, matching RuntimeApp::ShutdownGraphPath's own
            // comment on why: ToJson's top-level "census" field reads
            // AddCensus's values directly, and Materials() is a live,
            // on-demand read -- so populating it always costs nothing
            // extra, and it is unaffected by --probe staying refused on
            // this host.
            if (m_resolver)
            {
                const Arcane::SceneRenderResolver::MaterialCensus census = m_resolver->Materials();
                report.AddCensus(census.spriteReferenced, census.spriteBound,
                                  census.postReferenced, census.postBound,
                                  census.meshReferenced, census.meshBound);
            }

            // The --compare verdict (Task 9), ported from RuntimeApp::
            // ShutdownGraphPath verbatim (structure and field meanings
            // unchanged -- see that function's own comments for the full
            // reasoning on each branch). Emitted ONLY when a comparison was
            // actually requested, so an agent can tell "not asked" from
            // "asked and passed".
            if (!m_config.compareReference.empty())
            {
                const char* const backendName = CompareBackendName(m_config.backend);
                const std::filesystem::path projectRoot =
                    (m_runtime && m_runtime->CurrentProject()) ? m_runtime->CurrentProject()->Root()
                                                                : std::filesystem::path{};

                const auto levelName = [](Arcane::ReferenceLevel level) -> std::string
                {
                    switch (level)
                    {
                        case Arcane::ReferenceLevel::Shared:  return "shared";
                        case Arcane::ReferenceLevel::Backend: return "backend";
                        default:                               return "none";
                    }
                };

                // Schema 5: `compare.triedPaths` -- ported from RuntimeApp::
                // ShutdownGraphPath verbatim (see that function's own
                // comment). Seeded from m_compareResolution, re-seeded from
                // `after` in the bless branch, exactly mirroring
                // resolvedLevel/referencePath's own staleness handling below.
                const auto stringifyTriedPaths =
                    [](const std::vector<std::filesystem::path>& paths) -> std::vector<std::string>
                {
                    std::vector<std::string> out;
                    out.reserve(paths.size());
                    for (const std::filesystem::path& p : paths)
                        out.push_back(p.generic_string());
                    return out;
                };

                std::string   resolvedLevel = levelName(m_compareResolution.level);
                std::string   referencePath = m_compareResolution.path.string();
                std::vector<std::string> triedPaths =
                    stringifyTriedPaths(m_compareResolution.triedPaths);
                bool          passed            = false;
                std::uint64_t diffCount         = 0;
                double        diffRatio         = 0.0;
                std::uint64_t maxDiffPixelsUsed = 0;
                bool          sizesMismatch     = false;
                std::string   diffPath;
                std::string   errorMessage;
                double        maxLocalDifference = 0.0;

                if (m_compareMissingFatal)
                {
                    errorMessage = "no reference image on disk; re-run with --bless to create one";
                }
                else if (compareBlessed)
                {
                    // m_compareResolution was captured BEFORE the write, so
                    // its level is stale for a first bless (was None, is now
                    // Shared) -- re-resolve so the report describes where
                    // the reference actually ended up.
                    const Arcane::ReferenceResolution after =
                        Arcane::ResolveReference(projectRoot, m_config.compareReference,
                                                  backendName);
                    resolvedLevel = levelName(after.level);
                    referencePath = after.path.string();
                    triedPaths    = stringifyTriedPaths(after.triedPaths);
                    passed        = true;   // the reference now IS the capture, by construction
                }
                else if (compareWriteFailed)
                {
                    errorMessage = "the settle loop converged, but writing the blessed reference "
                                   "to '" + m_compareResolution.blessTarget.string() + "' failed";
                }
                else if (m_compareEvaluated)
                {
                    passed             = m_compareResult.passed;
                    diffCount          = m_compareResult.diffCount;
                    diffRatio          = m_compareResult.diffRatio;
                    maxDiffPixelsUsed  = m_compareResult.maxDiffPixelsUsed;
                    sizesMismatch      = m_compareResult.sizesMismatch;
                    errorMessage       = m_compareResult.errorMessage;
                    maxLocalDifference = m_compareResult.maxLocalDifference;

                    // The write itself happens ABOVE, outside this --report
                    // block (final-review I-2) -- the diff PNG must land
                    // whether or not a JSON report was asked for. All that is
                    // left here is to REPORT where it went; empty means it was
                    // never written (a pass, or a refused/failed path), exactly
                    // what this field meant before the hoist.
                    diffPath = compareDiffPath;
                }
                else
                {
                    errorMessage = "the settle loop never reached a stable, idle frame, so no "
                                   "comparison ever ran";
                }

                report.SetCompare(m_config.compareReference, resolvedLevel, referencePath, passed,
                                   diffCount, diffRatio, maxDiffPixelsUsed, sizesMismatch, diffPath,
                                   errorMessage, maxLocalDifference, triedPaths);
            }

            // No probe specs, ever, on this host -- Evaluate() is
            // deliberately not called: m_probes stays its default empty
            // array either way (ToJson emits `"probes": []`), and calling
            // it with nothing to evaluate would only dress up a no-op as a
            // step. --probe stays refused at main.cpp (Task 9, Finding B).
            if (report.WriteTo(m_config.reportPath))
                ARC_INFO("report written: {}", m_config.reportPath);
            else
                ARC_ERROR("failed to write report to '{}'", m_config.reportPath);
        }
    }

    void EditorApp::Shutdown()
    {
        // Deregister both sinks FIRST, before anything below can dispatch into
        // a half-torn-down editor -- see ConsoleDiagnostics::Uninstall()'s doc
        // comment for the ordering rationale (diagnostics store, then console).
        m_consoleDiag.Uninstall();

        // Uninstall the report-written hook too, same reason and same
        // ordering: the hang/gpu-stall watchdog thread keeps running until
        // Diagnostics::Shutdown() (called from main(), after this object is
        // gone) joins it, so a report written in that window must not
        // dispatch into an EditorApp mid-teardown or already destroyed.
        // OnReportWritten's `user` is `this` -- clearing the slot here,
        // before any member below starts tearing down, is what makes that
        // pointer safe to have handed out.
        Arcane::Diagnostics::ClearReportWrittenHook();

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

        // --dump-layout: write the LIVE ImGui layout (real EditorDockHost
        // dock-node ids and sizes, as of whatever this run actually
        // submitted over its --frames budget) to this .ini, right before
        // teardown -- the authoring half of the committed
        // ReferenceProject/Saved/verify-layout.ini seed.
        //
        // NOT placed beside RetargetLayoutIni's own SaveIniSettingsToDisk
        // call (the guard `!m_layoutIniPath.empty() && io.IniFilename` a
        // project switch flushes through): that call fires at OnProjectOpened,
        // BEFORE the frame loop has drawn a single panel, and it is flatly
        // UNREACHABLE under --headless (RetargetLayoutIni's headless branch
        // clears m_layoutIniPath, nulls io.IniFilename, and returns ahead of
        // it). Dumping there would capture whatever was on disk before this
        // run touched anything -- not "the live layout", and not reachable
        // headless at all, which is the one mode this flag exists for.
        //
        // Runs UNCONDITIONALLY relative to --headless/windowed, matching
        // HostConfig.hpp's own comment on dumpLayoutPath:
        // ImGui::SaveIniSettingsToDisk(path) never reads io.IniFilename (which
        // IS null here under --headless) -- it always writes to the path it
        // is GIVEN. Gated on m_gpu, not m_frameCount: a null m_gpu means no
        // ImGui context was ever created (a failed Create()), and
        // SaveIniSettingsToDisk dereferences the current context
        // unconditionally -- a crash, not a degraded result. A dump with zero
        // frames rendered is merely uninteresting, not unsafe, so that case
        // is not refused.
        if (!m_config.dumpLayoutPath.empty() && m_gpu)
        {
            ImGui::SaveIniSettingsToDisk(m_config.dumpLayoutPath.c_str());
            ARC_INFO("--dump-layout: wrote the live ImGui layout to {}", m_config.dumpLayoutPath);
        }

        // Release the editor lock: this project is no longer open anywhere.
        if (m_bootCompleted && m_runtime)
        {
            if (const Arcane::Project* proj = m_runtime->CurrentProject())
            {
                Arcane::EditorLock::Clear(proj->Root());
            }
        }

        // The whole render teardown -- the view-before-texture invalidate,
        // both contexts, and the latch read-back -- in the one order that is
        // correct. See ShutdownGraphPath.
        ShutdownGraphPath();

        // NO DEVICE IDLE IS OWED HERE: ShutdownGraphPath above has already
        // destroyed both contexts, and each idles the shared device and drains
        // its own lane on the way out.
        ARC_INFO("Arcane Editor exiting after {} frames", m_frameCount);

        // The member destructors then run (after Run returns + ~EditorApp), in
        // reverse declaration order -- the load-bearing TEARDOWN CONTRACT:
        //   m_plugin  -> ~PluginHost: Unload (TeardownLive -> ClearSystems +
        //                ResetRegistry) while the plugin DLL is STILL mapped.
        //   m_runtime -> ~Runtime: destroys JobSystem + the now-empty Registry.
        //   m_gpu     -> ~GpuContext: the render/input stack, window LAST. So
        //                gpu outlives runtime + plugin exactly as
        //                ArcaneRuntime's does. See GpuContext's header.
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

        // ===== THE EXIT-CODE FOLD ============================================
        // Mirrors RuntimeApp::Run's tail, code for code, so one desk battery
        // item reads the same against either host. Everything below is 0 on an
        // ordinary run -- the contract this fold keeps ("0 clean, 1 boot/init
        // failed").
        //
        // A device-loss exit is an abnormal end even though it was orderly: the
        // crash report exists, but the session did not do what it was asked to.
        // First, because it outranks every other explanation -- a lost device is
        // WHY the frames afterwards failed. This is also the fold the editor
        // simply did not have: MainLoop has broken on GpuDeviceLostObserved
        // since long before this phase, and reported it as a clean exit 0.
        if (Arcane::GpuDeviceLostObserved())
            return 1;
        // A boot/init failure keeps its own code and outranks the graph's for
        // the same reason: it says where the run died, and it died earlier.
        if (exitCode != 0)
            return exitCode;
        // The graph path's own codes: 1 = a graph frame FAILED (either context
        // -- the viewport's, phase 10, or the chrome's, phase 19), 2 =
        // RenderErrorCount GREW across the run, teardown included. Precedence
        // 1 > 2, set at the two sites that produce them (NoteGraphFrameFailure
        // and ShutdownGraphPath). They can fire on ANY run -- the render path
        // is unconditional -- so 0 here means no graph failure occurred.
        if (m_graphExit != 0)
            return m_graphExit;
        // See ArcaneEditor/src/main.cpp's exit-code table (right above `int
        // main`) for how the Hub's launch.rs decodes these.
        return 0;
    }
}
