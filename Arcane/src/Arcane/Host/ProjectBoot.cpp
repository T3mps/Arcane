#include <Arcane/Host/ProjectBoot.hpp>

#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/GpuContext.hpp>

#include <algorithm>
#include <iterator>
#include <type_traits>

namespace Arcane::HostBoot
{
    // Binding proof (2026-07-30 review): BootContext::gpu must be a real
    // Arcane::GpuContext*, not a phantom Arcane::HostBoot::GpuContext* that an
    // elaborated-type-specifier ("class GpuContext*") would silently conjure
    // up if no Arcane::GpuContext were visible at the point BootContext is
    // declared. Kept permanent rather than thrown away: it costs nothing at
    // compile time and fails loudly if the forward declarations in
    // ProjectBoot.hpp are ever removed or reordered past BootContext.
    static_assert(std::is_same_v<decltype(BootContext::gpu), Arcane::GpuContext*>,
                  "BootContext::gpu must bind Arcane::GpuContext -- see the "
                  "Arcane-namespace forward declarations at the top of "
                  "ProjectBoot.hpp");

    // Same binding proof for BootContext::splash, added once Task 7 gave
    // BootSplashWindow a real definition (the Task 5 forward declaration this
    // guards against was already in place, but std::is_same_v needs no
    // complete pointee -- an incomplete Arcane::BootSplashWindow would have
    // worked here too; this is just the earliest point a real include exists).
    static_assert(std::is_same_v<decltype(BootContext::splash), Arcane::BootSplashWindow*>,
                  "BootContext::splash must bind Arcane::BootSplashWindow -- see the "
                  "Arcane-namespace forward declarations at the top of "
                  "ProjectBoot.hpp");

    namespace
    {
        // The sentinel body for a stage no host has supplied real work for
        // (Task 8, 2026-07-30 review fix). `Make(..., run)` with `run` left
        // default-constructed (empty) installs this instead of a silent
        // `return true`: a typo'd or renamed id, or a host that simply forgot
        // to patch one, now logs loudly AND fails the stage, so a Fatal stage
        // hard-aborts boot rather than reporting success with a step quietly
        // skipped -- exactly the shape of all three shipped bugs this whole
        // arc exists to close, and exactly the gap BootStageParityTest cannot
        // see (it compares ids only, never invokes `.run`). See BootContext's
        // "THE CANONICAL BOOT SEQUENCE" comment in ProjectBoot.hpp for the
        // full contract, including how a host marks an id as an INTENTIONAL
        // no-op rather than relying on this sentinel.
        std::function<bool()> Unpatched(std::string id)
        {
            return [id = std::move(id)]
            {
                ARC_ERROR("BootStage '{}' has no host body -- a host forgot to "
                          "patch it, or the id was renamed/typo'd without "
                          "updating the patch site (see ProjectBoot.hpp's "
                          "CoreStages comment)", id);
                return false;
            };
        }

        BootStage Make(std::string id, std::vector<std::string> deps,
                       BootThread thread, BootPolicy policy, std::uint32_t weight,
                       std::function<bool()> run = {})
        {
            BootStage s;
            s.id = id;   // copy: Unpatched(id) below still needs it if run is empty
            s.dependsOn = std::move(deps);
            s.thread = thread;
            s.policy = policy;
            s.weight = weight;
            s.run = run ? std::move(run) : Unpatched(std::move(id));
            return s;
        }
    }

    std::vector<BootStage> CoreStages(BootContext& ctx)
    {
        std::vector<BootStage> s;
        // Task 8 fills in real bodies for the stages that are fully expressible
        // through BootContext alone -- type_context_install, project_open,
        // input_config -- because that logic is genuinely identical for any
        // host and belongs in exactly one place (the DAG's whole point).
        //
        // The rest (runtime_create, gpu_core, render_bridge, sprite_tables,
        // plugin_load, finalize) construct or populate HOST-OWNED storage
        // (EditorApp::m_gpu / m_runtime / m_plugin / m_resolver, and their
        // ArcaneRuntime equivalents) that this module cannot see or own --
        // Arcane.dll cannot reach into an EXE's private members, and some of
        // the editor's own steps (theme/fonts/console sink/document routing)
        // live in ArcaneEditor.exe, not here. Each host is REQUIRED to
        // overwrite that stage's `run` with its own closure after calling
        // EditorStages/RuntimeStages (see EditorApp::Run / RuntimeApp::Run)
        // -- the id, dependsOn, thread and weight it inherits from here are
        // what stays canonical and shared; only the body differs, exactly as
        // BootStageParityTest documents ("proves both hosts RUN the same
        // stages, not that a stage's body behaves identically"). `Make(...)`
        // called with no trailing `run` argument (every push_back below with
        // no lambda) installs Unpatched(id) instead of a silent success, so a
        // host that forgets -- or a future rename this file's own id string
        // and a host's patch-site string drift apart on -- fails LOUDLY
        // instead of quietly reporting a healthy boot. See ProjectBoot.hpp's
        // "THE CANONICAL BOOT SEQUENCE" comment for the full contract.
        s.push_back(Make("runtime_create",       {},                                  BootThread::Main,   BootPolicy::Fatal,     5));
        // edit_core (2026-07-30 review, Fix 5 -- a design change the human
        // ruled on, not a mechanical fix): the editor's undo/redo history
        // (m_undo) and structural-edit binding (m_editBinding) need nothing
        // but the Runtime, yet used to be built inside the OLD monolithic
        // sprite_tables -- gated behind project_open AND render_bridge for
        // no reason, and Optional-policy, so a host that forgot to patch
        // sprite_tables left m_undo never constructed and StageFinalize's
        // `m_scene.Adopt(..., *m_undo)` a null-optional dereference. Split
        // into its own Fatal stage depending only on runtime_create: the
        // editor builds m_undo/m_editBinding here; the runtime has nothing
        // to do (no undo history, no scene session) and patches an
        // EXPLICIT no-op, the same shape RuntimeApp's "finalize" already
        // uses, so an intentional empty body stays distinguishable from a
        // forgotten one (see RuntimeApp::Run).
        s.push_back(Make("edit_core",            {"runtime_create"},                  BootThread::Main,   BootPolicy::Fatal,     1));
        s.push_back(Make("type_context_install", {"runtime_create"},                  BootThread::Main,   BootPolicy::Fatal,     1, [&ctx]
        {
            // The host's own Astra::SetTypeContext(...) call happens inside
            // runtime_create's host-supplied body -- it needs the host-owned
            // TypeContext pointer, which only the host allocates. This half is
            // pure engine logic (VerifySharedTypeContext takes only a Registry&
            // and a label) and is genuinely shared by both hosts.
            //
            // BEHAVIOR CHANGE, DELIBERATE (2026-07-30 review): before this
            // task, both hosts called VerifySharedTypeContext with the result
            // explicitly discarded (`(void)...`) under a comment reading
            // "Non-fatal by choice". This stage's policy is Fatal and its
            // body RETURNS the check's result, so a genuine TypeContext
            // mismatch now ABORTS boot on both hosts instead of merely
            // logging and continuing with components silently misread. That
            // is intentional and is being kept: a mismatch here IS motivating
            // bug #3 (the 2026-07-30 Camera/Transform aliasing incident) --
            // discovering it loudly at boot beats discovering it as "my
            // sprite renders as one pixel" hours later.
            if (!ctx.runtime) return true;   // facility absent -- nothing to verify
            return VerifySharedTypeContext(ctx.runtime->Registry(),
                                           ctx.moduleName ? ctx.moduleName : "HostBoot");
        }));
        s.push_back(Make("gpu_core",             {},                                  BootThread::Main,   BootPolicy::Fatal,    25));
        s.push_back(Make("project_open",         {"runtime_create"},                  BootThread::Worker, BootPolicy::Optional, 45, [&ctx]
        {
            // Optional default: a failed/absent open is never fatal here -- the
            // runtime host tightens this into a Fatal ABI refusal (see
            // RuntimeStages below); the editor keeps exactly this behavior.
            if (!ctx.runtime || !ctx.projectPath || !*ctx.projectPath)
                return true;   // no --project: nothing to open, not a failure
            if (ctx.runtime->OpenProject(ctx.projectPath))
                return true;
            ARC_WARN("{}: --project '{}' failed to open; using data/ + --plugin fallback",
                     ctx.moduleName ? ctx.moduleName : "HostBoot", ctx.projectPath);
            return true;
        }));
        s.push_back(Make("render_bridge",        {"gpu_core", "runtime_create"},      BootThread::Main,   BootPolicy::Fatal,     3));
        s.push_back(Make("input_config",         {"project_open", "gpu_core"},        BootThread::Main,   BootPolicy::Optional,  2, [&ctx]
        {
            if (!ctx.gpu || !ctx.runtime) return true;
            if (!LoadInputConfig(ctx.gpu->Input(), ctx.runtime->Configuration()))
                ARC_WARN("{}: input actions failed to load", ctx.moduleName ? ctx.moduleName : "HostBoot");
            return true;
        }));
        // sprite_tables is Fatal, not Optional (2026-07-30 review, Fix 5):
        // Optional bought nothing here. Both hosts' bodies already degrade a
        // missing dxcompiler.dll to an internal ARC_WARN and still `return
        // true` -- that is the ONE genuinely-optional failure mode, and it
        // was never what this stage's own BootPolicy gated. The only thing
        // Optional actually gated was a MISSING HOST BODY (the Fix-2
        // Unpatched sentinel), which it turned into a logged-but-ignored
        // warning instead of a hard boot failure -- exactly backwards for a
        // stage whose whole job is publishing the sprite/material/resolver
        // tables the arc's motivating bug #2 was about. The runtime's
        // resolver is equally not optional: without it every sprite falls
        // back to the untextured 1x1 m quad, silently.
        s.push_back(Make("sprite_tables",        {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Fatal,     2));
        s.push_back(Make("plugin_load",          {"project_open", "render_bridge"},   BootThread::Main,   BootPolicy::Fatal,     9));
        // finalize depends on edit_core too (2026-07-30 review, Fix 5): the
        // editor's StageFinalize dereferences `*m_undo` (m_scene.Adopt), and
        // m_undo is now built in edit_core, a DAG SIBLING of sprite_tables/
        // plugin_load/input_config (no ordering relationship among them
        // otherwise) -- this dependency is what makes "m_undo exists before
        // finalize touches it" structural rather than incidental.
        s.push_back(Make("finalize",             {"plugin_load", "input_config",
                                                  "sprite_tables", "edit_core"},      BootThread::Main,   BootPolicy::Fatal,     1));
        return s;
    }

    std::vector<std::string> CoreStageIds()
    {
        BootContext ctx{};
        std::vector<std::string> ids;
        for (const BootStage& s : CoreStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<BootStage> RuntimeStages(BootContext& ctx)
    {
        // appends nothing -- that is the point (BootStageParityTest pins the id
        // list to be IDENTICAL to CoreStageIds()). What CAN and does differ per
        // host is a stage's run/policy, which the id-based parity checks never
        // inspect. project_open is the one deliberate asymmetry: the editor can
        // usefully run project-less (Optional, CoreStages' default above), but
        // an ABI-stale --project silently booting the data/ fallback is exactly
        // the failure this refusal exists to prevent for the runtime host.
        std::vector<BootStage> s = CoreStages(ctx);
        for (BootStage& stage : s)
        {
            if (stage.id != "project_open")
                continue;
            stage.policy = BootPolicy::Fatal;
            stage.run = [&ctx]
            {
                if (!ctx.runtime || !ctx.projectPath || !*ctx.projectPath)
                    return true;   // no --project: nothing to open, not a failure
                if (ctx.runtime->OpenProject(ctx.projectPath))
                    return true;
                ARC_ERROR("{}: '{}' could not be opened (engine ABI {} -- is the "
                          "project's game DLL built against this engine?). Refusing "
                          "to boot with the data/ fallback.",
                          ctx.moduleName ? ctx.moduleName : "HostBoot", ctx.projectPath,
                          static_cast<unsigned>(Arcane::PluginABIVersion()));
                return false;   // Fatal for the runtime host
            };
            break;
        }
        return s;
    }

    std::vector<BootStage> EditorStages(BootContext& ctx)
    {
        std::vector<BootStage> s = CoreStages(ctx);

        // editor_fonts/editor_shell are INSERTED right after gpu_core, not
        // merely appended at the end (2026-07-30 second review fix -- the
        // FIRST review fix, binding the presenter at the end of editor_shell
        // instead of gpu_core, was necessary but NOT sufficient; this is the
        // actual root cause).
        //
        // BootSequence picks the lowest-INDEXED ready Main stage each
        // iteration. Appending editor_fonts/editor_shell after CoreStages'
        // 10 entries (indices 10+) meant that once the project_open WORKER
        // completes -- which it reliably does during gpu_core's own device
        // creation -- render_bridge, input_config, sprite_tables, and
        // PLUGIN_LOAD all had LOWER indices and ran first. PluginHost::Load
        // -> the plugin's Init calls `ImGui::SetCurrentContext(...)` to
        // adopt the HOST's context for its OWN offscreen "game" ImGui layer
        // (Sandbox.cpp:102) and never restores it (PluginHost does not
        // bracket the call -- see Fix 3, PluginHost.cpp, which closes that
        // landmine at its source; this reordering closes the SPECIFIC
        // boot-time symptom). So by the time editor_fonts/editor_shell
        // finally ran, EVERY ImGui call they make -- InstallEditorFonts'
        // AddFontFromFileTTF, ApplyEditorTheme(GetStyle()), the
        // ConfigFlags |= ImGuiConfigFlags_DockingEnable, AddSettingsHandler
        // -- landed on the GAME context, not the editor's:
        //   - Fonts added to the game atlas: the editor keeps ImGui's stock
        //     face, and every ICON_LC_* glyph renders as a missing-glyph box.
        //   - The docking flag set on the game context: the EDITOR context's
        //     first NewFrame then sees DockingEnable unset, so ImGui's own
        //     DockContextNewFrameUpdateUndocking wipes any loaded dock nodes;
        //     EndDockSpace finds none, calls DockBuilderAddNode, whose
        //     internal DockSpace() call early-returns 0 for the same reason,
        //     and `node->LastFrameAlive = ...` derefs NULL --
        //     EXCEPTION_ACCESS_VIOLATION, imgui.cpp:20823, backend-
        //     independent (confirmed via WER + llvm-symbolizer against the
        //     PDB: identical fault offset on D3D12 and Vulkan).
        //   - Settings handlers registered on the game context, whose
        //     io.IniFilename is null: [ArcaneEditorLayout]/[EditorPlayMode]
        //     are read (by the EDITOR context, correctly pointed at
        //     imgui.ini) before these handlers exist on THAT context, so
        //     LoadIniSettingsFromMemory drops the sections.
        //   - The theme landed on the game context: the real window reveals
        //     in stock ImGui blue, not the editor palette.
        // Pre-existing code never hit this: the OLD EditorApp::Init set the
        // docking flag/theme/fonts long before OffscreenImGuiLayer::Create
        // and PluginHost::Load, and ImGuiLayer::BeginFrame re-pins the
        // EDITOR context every MainLoop frame regardless of what a plugin
        // last set -- so a plugin's dangling SetCurrentContext was always
        // harmless there. This arc introduced the hazard by moving that
        // ONE-TIME setup into stages whose registration order let plugin_load
        // run first.
        //
        // Fix: editor_fonts and editor_shell are inserted immediately after
        // "gpu_core" in the vector -- ahead of render_bridge/input_config/
        // sprite_tables/plugin_load/finalize -- so they are the first Main
        // stages ready once gpu_core completes, regardless of how fast the
        // project_open worker finishes. This is ALSO what makes the real
        // loading screen non-vestigial: StageEditorShell binds the presenter
        // into m_lazyPresenter at its own end (unchanged from the first
        // review fix), and with this reordering that bind now happens after
        // only 3 stages (runtime_create/edit_core-or-similar/gpu_core/
        // editor_fonts/editor_shell) instead of after 11 of 13 -- so the
        // presenter is live for essentially the WHOLE remaining boot
        // (project_open's worker tail, render_bridge, input_config,
        // sprite_tables, plugin_load, finalize), not for two frames at ~99%
        // -- confirm this held after the fix by tracing the actual stage
        // sequence, don't just assume the reorder achieved it.
        {
            std::vector<BootStage> editorEarly;
            editorEarly.push_back(Make("editor_fonts", {"gpu_core"},      BootThread::Main, BootPolicy::Fatal, 5));
            editorEarly.push_back(Make("editor_shell",  {"editor_fonts"}, BootThread::Main, BootPolicy::Fatal, 3));

            const auto gpuCoreIt = std::find_if(s.begin(), s.end(),
                [](const BootStage& st) { return st.id == "gpu_core"; });
            // gpu_core is a CoreStages invariant guarded by BootStageParityTest's
            // pinned kCanonical list, so this is always found in practice; the
            // fallback (append at the end) degrades to the OLD, already-buggy
            // ordering rather than an out-of-bounds insert if that ever changes.
            const auto insertAt = (gpuCoreIt != s.end()) ? gpuCoreIt + 1 : s.end();
            s.insert(insertAt,
                     std::make_move_iterator(editorEarly.begin()),
                     std::make_move_iterator(editorEarly.end()));
        }

        s.push_back(Make("splash_ready",  {"editor_fonts", "editor_shell"}, BootThread::Main, BootPolicy::Fatal, 2, [&ctx]
        {
            // Show the REAL window first, THEN close the pre-device splash.
            // Reversing these leaves a frame with neither on screen. Pure
            // ctx/engine logic (Window::Show, BootSplashWindow::Close) -- the
            // runtime host performs the same handoff itself (folded into its
            // render_bridge override, since it has no editor_fonts dependency
            // to hang a dedicated stage id off of and RuntimeStages appends
            // nothing).
            //
            // Depends on editor_shell TOO, not just editor_fonts: editor_shell
            // is what calls SetTitle/SetIcon/ApplyEditorTheme and binds the
            // real presenter (see the ordering comment above editor_fonts'
            // insertion). splash_ready stays APPENDED (not moved forward with
            // editor_fonts/editor_shell) precisely so it keeps running after
            // both, whatever their own position -- dependency ids, not vector
            // position, are the actual guarantee here.
            if (ctx.gpu) ctx.gpu->Win().Show();
            if (ctx.splash) ctx.splash->Close();
            return true;
        }));
        s.push_back(Make("editor_lock",   {"project_open"},  BootThread::Worker, BootPolicy::Optional, 1, [&ctx]
        {
            // Arcane::EditorLock is engine-visible (Project.hpp), so this is
            // pure ctx logic, unlike editor_fonts/editor_shell.
            if (ctx.runtime)
                if (const Arcane::Project* proj = ctx.runtime->CurrentProject())
                    Arcane::EditorLock::Write(proj->Root());
            return true;
        }));
        return s;
    }

    std::vector<std::string> EditorStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : EditorStages(ctx)) ids.push_back(s.id);
        return ids;
    }

    std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx)
    {
        std::vector<std::string> ids;
        for (const BootStage& s : RuntimeStages(ctx)) ids.push_back(s.id);
        return ids;
    }
}
