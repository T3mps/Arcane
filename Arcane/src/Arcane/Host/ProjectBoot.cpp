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

        // project_open's AssetRegistry::ScanContent progress callback, shared by
        // CoreStages' own body (the editor keeps this unmodified) and
        // RuntimeStages' override (which replaces the WHOLE closure for the
        // Fatal-ABI-refusal behaviour but wants the identical sub-progress
        // reporting) -- see BootStage::detail's own comment for why the box is
        // a shared_ptr rather than something this function could just close
        // over and hand back as a FunctionRef (it cannot: FunctionRef is a
        // non-owning view of a SYNCHRONOUS, non-escaping callable, so the
        // referent has to be a named local at the actual OpenProject call
        // site, not a temporary this factory would return).
        //
        // Throttled to roughly every 32 files, plus always the first and the
        // final tick: IBootPresenter's contract (BootSequence.hpp) requires a
        // presenter to stay cheap and non-blocking, and BootStageDetail::Set
        // is not free (a mutex lock plus a std::string format+allocation) --
        // ScanContent's own callback fires once per file, so reporting EVERY
        // one of a content tree's files would put that cost on the worker
        // thread for a status line nothing reads faster than present()'s own
        // ~8ms pump cadence (BootSequence.cpp) actually repaints it.
        void ReportScanProgress(BootStageDetail& box, std::size_t done, std::size_t total)
        {
            constexpr std::size_t kStride = 32;
            if (done != 1 && done != total && done % kStride != 0)
                return;
            box.Set("Scanning content... " + std::to_string(done) + " / " + std::to_string(total));
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
        {
            // scanDetail: attached to the stage below (BootStage::detail) so
            // BootSequence's present() can read it into BootProgress::detail
            // while this stage runs -- see ReportScanProgress's own comment for
            // why the box (not a returned FunctionRef) is what gets shared with
            // RuntimeStages' override.
            auto scanDetail = std::make_shared<BootStageDetail>();
            BootStage projectOpen = Make("project_open", {"runtime_create"}, BootThread::Worker,
                                         BootPolicy::Optional, 45, [&ctx, scanDetail]
            {
                // Optional default: a failed/absent open is never fatal here -- the
                // runtime host tightens this into a Fatal ABI refusal (see
                // RuntimeStages below); the editor keeps exactly this behavior.
                // No clear-on-exit here (an earlier revision of this closure had
                // one): once this stage completes, BootSequence.cpp's present()
                // never again looks up "project_open"'s id -- the per-stage
                // present(ranStageId)/present(workerStageId) call sites only ever
                // pass a stage id that is CURRENTLY running or just completed,
                // and a Worker stage that already finished is neither, for the
                // rest of this boot. A leftover "Scanning content... N / N" in
                // the box is therefore inert, and leaving it also makes the box's
                // final content directly, deterministically testable (see
                // BootStageParityTest.cpp's project_open .run() coverage) without
                // racing a background thread against a clear this closure would
                // otherwise perform the instant OpenProject returns.
                if (!ctx.runtime || !ctx.projectPath || !*ctx.projectPath)
                    return true;   // no --project: nothing to open, not a failure
                if (ctx.runtime->OpenProject(ctx.projectPath,
                        [scanDetail](std::size_t done, std::size_t total)
                        { ReportScanProgress(*scanDetail, done, total); }))
                    return true;
                ARC_WARN("{}: --project '{}' failed to open; using data/ + --plugin fallback",
                         ctx.moduleName ? ctx.moduleName : "HostBoot", ctx.projectPath);
                return true;
            });
            projectOpen.detail = scanDetail;
            s.push_back(std::move(projectOpen));
        }
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
            // Reuse the SAME BootStageDetail box CoreStages already attached
            // (stage.detail), not a fresh one -- this closure fully replaces
            // `.run` below, so it must capture its own reference to report
            // into, and it has to be the box BootSequence's present() already
            // knows to read for this stage's id.
            auto scanDetail = stage.detail;
            stage.run = [&ctx, scanDetail]
            {
                // Runtime-only default (spec sec 6): no boot progress until a
                // project's own manifest opts in. Set every time this stage
                // actually RUNS (not once at RuntimeStages-construction time,
                // which callers like RuntimeStageIdsForTest invoke without ever
                // running a boot) -- RuntimeApp::Run additionally sets this
                // BEFORE BootSequence::Run begins, so not even the present()
                // call after "runtime_create" (which completes and reports
                // before this Worker stage's body gets a chance to run) can
                // show progress a player never asked to see.
                if (ctx.splash) ctx.splash->SetShowProgress(false);
                if (!ctx.runtime || !ctx.projectPath || !*ctx.projectPath)
                    return true;   // no --project: nothing to open, not a failure

                // Pre-open PEEK at splash.showProgress, so an opted-in project's
                // "Scanning content... N / M" text can actually be LIVE during
                // the scan it describes, not just the taskbar/fraction from
                // render_bridge onward. Without this, showProgress could only be
                // learned from ctx.runtime->CurrentProject() AFTER OpenProject
                // returns -- but ProjectManifest is parsed (Project.cpp) BEFORE
                // that same call's content scan runs, so by the time this code
                // could see it, the scan (and every onProgress call below) has
                // already finished. Arcane::Project::ResolveManifestFile is the
                // SAME function Project::Open itself now calls internally (that
                // file's own comment) -- reusing it here means there is exactly
                // ONE implementation of "how does a project root resolve to a
                // manifest file", not a second one reimplemented in this TU.
                // Silent and best-effort by design: no --project, an ambiguous/
                // missing .arcproj, or an unparseable manifest all just leave
                // showProgress at the `false` set two lines up -- OpenProject
                // right below is the real, authoritative, error-reporting open;
                // this is only a look-ahead for one boolean, and its failure
                // modes are already OpenProject's failure modes reported again.
                if (ctx.splash)
                    if (const auto peekFile = Arcane::Project::ResolveManifestFile(ctx.projectPath))
                        if (const auto peek = Arcane::ProjectManifest::LoadFile(*peekFile))
                            ctx.splash->SetShowProgress(peek->splash.showProgress);

                if (ctx.runtime->OpenProject(ctx.projectPath,
                        [scanDetail](std::size_t done, std::size_t total)
                        { ReportScanProgress(*scanDetail, done, total); }))
                {
                    // Re-set from the ADOPTED manifest (not just the peek above):
                    // authoritative over the peek in the (practically impossible,
                    // for a boot-time --project) case they could ever disagree --
                    // e.g. a self-heal rewrite between the peek's read and this
                    // one. Never the editor's default (EditorStages does not
                    // touch showProgress at all, so its true default stands
                    // regardless of what any opened project's manifest says).
                    if (ctx.splash)
                        if (const Arcane::Project* proj = ctx.runtime->CurrentProject())
                            ctx.splash->SetShowProgress(proj->Manifest().splash.showProgress);
                    return true;
                }
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

        // plugin_load: Optional here, not CoreStages' Fatal default (2026-07-30
        // review, boot-corestages Task 9b -- a deliberate human ruling: "Optional
        // for the editor, Fatal for the runtime"). Same mechanism RuntimeStages
        // uses above for project_open -- CoreStages declares the canonical
        // default and each host's own stage-list function tightens or loosens
        // it; the id/dependsOn/thread/weight this stage inherits from
        // CoreStages stay canonical, only .policy differs here.
        //
        // EditorApp::StagePluginLoad's own comment (EditorApp.cpp) already
        // makes the case this override relies on: "Every m_plugin-> use in
        // MainLoop is optional-guarded, so a disengaged plugin is safe." A
        // Fatal policy made a failed Load() abort BootSequence before
        // "finalize" ever ran, which made EditorApp::Run() return 1
        // (boot.ok == false) -- the process exiting with NO editor window
        // ever opened. That is backwards for this host specifically: the
        // editor is the one place a developer can go to FIX a game module
        // that fails to load. Refusing to open it is what prevents the fix,
        // not what protects anything -- there is nothing here for a Fatal
        // policy to be protecting, since every plugin access downstream is
        // already null-safe. RuntimeStages does NOT get this override and
        // keeps CoreStages' Fatal default: a game host with no module to run
        // has nothing to do, so it should refuse to boot loud rather than
        // silently sit at a black window (see RuntimeStages' own project_open
        // comment for the same "nothing useful to fall back to" reasoning).
        //
        // StagePluginLoad's failure branch was brought into line with this
        // (EditorApp.cpp): it now leaves the SAME defined, disengaged state
        // SwitchProject's switch_plugin_load stage already produces for the
        // identical failure (EditorAppProject.cpp) -- m_plugin.reset() plus a
        // detailed Console + modal banner naming the required ABI -- instead
        // of the bare "stage 'plugin_load' failed" a merely-Optional policy
        // would have left behind on its own.
        for (BootStage& stage : s)
        {
            if (stage.id != "plugin_load")
                continue;
            stage.policy = BootPolicy::Optional;
            break;
        }

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
        // project_open worker finishes.
        //
        // Task 8c (2026-07-30 correction, "the splash carries the loading UI,
        // not the editor window") does NOT touch this insertion point -- it
        // is a DIFFERENT invariant from splash_ready's (see that stage's own
        // comment below) and stays exactly here: fonts/theme/docking-flag/
        // settings-handlers still have to land on the editor's OWN ImGui
        // context before plugin_load can steal GImGui, regardless of when the
        // window is revealed. What DID change is what StageEditorShell does
        // at its own end -- it no longer binds a live swapchain presenter
        // there at all (see StageEditorShell's comment in EditorApp.cpp).
        {
            std::vector<BootStage> editorEarly;
            editorEarly.push_back(Make("editor_fonts", {"gpu_core"},      BootThread::Main, BootPolicy::Fatal, 5));
            editorEarly.push_back(Make("editor_shell",  {"editor_fonts"}, BootThread::Main, BootPolicy::Fatal, 3));

            const auto gpuCoreIt = std::find_if(s.begin(), s.end(),
                [](const BootStage& st) { return st.id == "gpu_core"; });
            // gpu_core is a CoreStages invariant guarded by BootStageParityTest's
            // pinned kCanonical list, so this is always found in practice --
            // but a silent fallback whose failure mode is "reintroduce the
            // 2026-07-30 crash" must not stay silent (review round 3 minor).
            if (gpuCoreIt == s.end())
            {
                ARC_ERROR("EditorStages: 'gpu_core' not found in CoreStages -- "
                          "editor_fonts/editor_shell fall back to appending at "
                          "the end, which REINTRODUCES the ImGui context-theft "
                          "ordering bug this file's comments document at "
                          "length (plugin_load would run first and steal "
                          "GImGui before fonts/theme/docking-flag/settings-"
                          "handlers are installed). This should be "
                          "structurally impossible -- 'gpu_core' is a "
                          "CoreStages invariant pinned by BootStageParityTest's "
                          "kCanonical list.");
            }
            const auto insertAt = (gpuCoreIt != s.end()) ? gpuCoreIt + 1 : s.end();
            s.insert(insertAt,
                     std::make_move_iterator(editorEarly.begin()),
                     std::make_move_iterator(editorEarly.end()));
        }

        s.push_back(Make("editor_lock",   {"project_open"},  BootThread::Worker, BootPolicy::Optional, 1, [&ctx]
        {
            // Arcane::EditorLock is engine-visible (Project.hpp), so this is
            // pure ctx logic, unlike editor_fonts/editor_shell.
            if (ctx.runtime)
                if (const Arcane::Project* proj = ctx.runtime->CurrentProject())
                    Arcane::EditorLock::Write(proj->Root());
            return true;
        }));

        // splash_ready: pushed LAST, and depends on "finalize" -- NOT
        // "editor_fonts"/"editor_shell" (Task 8c, 2026-07-30 correction,
        // superseding the second and third 2026-07-30 review fixes' shape,
        // which revealed the window right after editor_shell instead). The
        // human ruling: today's boot UX was backwards relative to Unreal --
        // the pre-device splash was a mute rectangle and the real editor
        // window was revealed early to show a loading bar inside IT. UE does
        // the opposite: engine init, module/plugin load, and the startup-map
        // load all run with the SPLASH up and reporting into it
        // (UnrealEdGlobals.cpp:167-194), and the main editor window is not
        // even created until loading is finished (UnrealEdGlobals.cpp:
        // 215-236: "Hide the splash screen now that everything is ready to
        // go" -> Hide() -> "Do final set up on the editor frame and show it"
        // -> CreateDefaultMainFrame). BootProgress is now RENDERED by the
        // splash for the whole run (Arcane::BootSplashPresenter,
        // BootSplashWindow.hpp; wired in EditorApp::Run) instead of the
        // editor window's ImGui bar, so there is no more reason to reveal the
        // window early -- doing so was always in service of showing that bar
        // to the user, and the bar has moved.
        //
        // Depends on `run` being a HOST override, not a ctx-only shared
        // lambda like it was before this task: revealing the window now also
        // needs to Present() one real frame through the swapchain-backed
        // BootPresenter first (see the "who draws the reveal frame" note
        // below), and that presenter is host-owned state
        // (EditorApp::m_presenter) this module cannot reach -- the same
        // structural reason render_bridge/plugin_load/etc. are host
        // overrides. `Make(...)` below is therefore called with NO trailing
        // `run` argument, which installs Make's Unpatched(id) sentinel (see
        // this file's top) -- EditorApp::Run() is REQUIRED to overwrite it
        // with StageSplashReady, same as every other host-owned id.
        //
        // Two constraints still bind even though the reveal moved to the
        // end, and they are in tension:
        //   1. Never reveal an undrawn window. Nothing has presented a frame
        //      into the swapchain by this point -- BootSequence's per-stage
        //      pump has been driven by the pre-device splash presenter for
        //      the ENTIRE run, and that presenter never touches the
        //      swapchain. StageSplashReady's body resolves this by
        //      Present()-ing ONE real frame through the swapchain-backed
        //      BootPresenter (Fullscreen, fraction=1.0) BEFORE calling
        //      Show() -- one frame the user will not perceive as a loading
        //      screen, matching UE's splash->main-frame handoff being a
        //      single hide/show pair rather than a fade.
        //   2. Never leave a gap with neither window on screen. Show() the
        //      real window (now holding that just-drawn frame) BEFORE
        //      Close()ing the pre-device splash -- reversing these two still
        //      leaves the gap this whole component exists to avoid.
        // Depending on "finalize" (Fatal) transitively guarantees editor_fonts/
        // editor_shell already ran too, without needing to name them again:
        // finalize's own dependency chain (plugin_load/input_config/
        // sprite_tables/edit_core) only becomes ready long after
        // editor_fonts/editor_shell, which are inserted right after gpu_core
        // specifically so they are the FIRST main stages ready (see that
        // insertion's own comment above) -- that ordering invariant is
        // unchanged by this task.
        s.push_back(Make("splash_ready", {"finalize"}, BootThread::Main, BootPolicy::Fatal, 2));

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
