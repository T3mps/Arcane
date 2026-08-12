#pragma once

// Host-boot helpers used by any runtime host (ArcaneRuntime, the Arcane Editor) that
// boots the engine via GpuContext/HostConfig -- all three now live in the
// engine DLL as Arcane/Host, so a host consumes them rather than source-
// compiling its own copy. They turn the engine's layered config + an open
// project into the two boot decisions a host makes: which input map to
// load, and which game module to host.

#include <Arcane/Base/Engine.hpp>        // BuildInfo (engine identity probe)
#include <Arcane/Base/Log.hpp>           // ARC_WARN/ARC_ERROR/ARC_INFO (not pulled in transitively by any of the below)
#include <Arcane/Base/Runtime.hpp>       // Runtime::ResetRegistry/Registry (BootScene)
#include <Arcane/Config/Config.hpp>
#include <Arcane/Host/BootSequence.hpp>  // BootStage/BootThread/BootPolicy (CoreStages)
#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // kGamePluginABIVersion (engine identity probe)
#include <Arcane/Project/AssetId.hpp>            // AssetId::FromGuid (BootSceneFile)
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // SetGpuDrawMarkersEnabled (ApplyDiagnosticsConfig)
#include <Arcane/Scene/Components.hpp>            // Arcane::Transform (VerifySharedTypeContext's default probe)
#include <Arcane/Serialization/SceneAsset.hpp>    // ReadSceneFile/ApplySceneDocument/CreateEmpty (BootScene)

#include <Astra/Component/ComponentRegistry.hpp>  // GetComponentIDFromHash (VerifySharedTypeContext)
#include <Astra/Core/TypeID.hpp>                  // TypeID<T>::Value/Hash/Name (ditto)

#include <Json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Arcane
{
    // Forward declarations for BootContext below. An elaborated-type-specifier
    // written from inside Arcane::HostBoot (e.g. "class GpuContext*") does
    // ordinary unqualified lookup; if no Arcane::GpuContext is visible yet at
    // that point (this header does not include GpuContext.hpp -- deliberately,
    // to stay light for every consumer that only wants the input/asset/scene
    // helpers), the compiler silently declares a NEW, distinct type in the
    // innermost enclosing namespace (Arcane::HostBoot::GpuContext), not
    // Arcane::GpuContext -- and which type you get would then depend on
    // whether GpuContext.hpp happened to be included first in a given TU, an
    // ODR hazard. These forward declarations pin the real Arcane:: types
    // regardless of include order. Runtime is included in full above (line 12)
    // and so is unaffected in practice, but is declared here too for the same
    // reason and for symmetry with GpuContext/BootSplashWindow.
    class Runtime;
    class GpuContext;
    class BootSplashWindow;   // Task 7 defines this, in namespace Arcane
}

namespace Arcane::HostBoot
{
    // Is THIS module on the same Astra TypeContext as the engine's registry?
    //
    // Astra resolves GetTypeContext()/SetTypeContext() through a PER-MODULE static
    // slot by design, so every binary that touches a component type must install the
    // shared context itself (Runtime's ctor covers Arcane.dll only). A host that
    // forgets gets its own empty DefaultTypeContext, and its TypeID<T>::Value()
    // assigns ids from a private counter -- which silently ALIAS the shared ids.
    // The failure is not a crash and not a miss: a view returns the wrong
    // component's entities and reinterprets its bytes.
    //
    // That cost an evening (2026-07-30): ArcaneRuntime.exe lacked the install, so
    // its TypeID<Camera> aliased Transform, ActiveSceneCamera read position.x as
    // orthographicSize, saw 0, reported "no usable camera", and every scene rendered
    // at 1 px per metre -- indistinguishable from "my sprite is missing".
    //
    // Being inline, this compiles into the CALLER's module, which is the only place
    // the question can be asked. Call it once at boot, after the Runtime exists.
    // Compares the caller-module id for a known engine component against the id the
    // registry (populated inside Arcane.dll) holds for the same STABLE name hash.
    // Returns false and logs ARC_ERROR on mismatch; true when correct or when the
    // component is not registered yet (nothing to contradict).
    template<typename Probe = Arcane::Transform>
    inline bool VerifySharedTypeContext(const Astra::Registry& reg, const char* moduleName)
    {
        const auto* creg = reg.GetComponentRegistry();
        if (!creg)
            return true;
        const auto shared = creg->GetComponentIDFromHash(Astra::TypeID<Probe>::Hash());
        if (shared.IsErr())
            return true;   // not registered yet -- no contradiction to report
        const Astra::ComponentID mine = Astra::TypeID<Probe>::Value();
        if (mine == *shared.GetValue())
            return true;
        ARC_ERROR("{}: this module is NOT on the engine's Astra TypeContext "
                  "({} resolves to id {} here but id {} in the registry). Call "
                  "Astra::SetTypeContext(ctx) in this module at boot -- every "
                  "component view and GetComponent in it is reading the WRONG "
                  "component's memory.",
                  moduleName, Astra::TypeID<Probe>::Name(),
                  (unsigned)mine, (unsigned)*shared.GetValue());
        return false;
    }

    // One-line JSON describing this engine build, for `--print-engine-info`.
    //
    // This exists so the Arcane Hub never HARDCODES a plugin ABI. A .arcproj
    // requires `engine.abi` (ProjectManifest.hpp), so a hub that guessed it
    // would mint stale-ABI projects the moment the engine bumps, and those
    // crash on open. The Hub probes, then stamps whatever the engine reports.
    //
    // Single line on purpose: the caller reads one line from stdout.
    //
    // `exePathUtf8` is UTF-8 BYTES, not a std::filesystem::path: callers pass
    // Arcane::ExecutablePathUtf8(). Taking a path here and calling
    // generic_string() would re-encode through the implementation's native narrow
    // encoding -- the ANSI-codepage round-trip that made this throw on non-ASCII
    // install paths in the first place. Backslashes are normalised here since we
    // no longer get generic_string()'s normalisation for free.
    inline std::string EngineInfoJson(std::string exePathUtf8)
    {
        std::replace(exePathUtf8.begin(), exePathUtf8.end(), '\\', '/');
        nlohmann::json j;
        // PluginABIVersion(), NOT the kGamePluginABIVersion header constant: the
        // constant is whatever THIS module was compiled against, while the gate
        // that rejects a plugin lives in Arcane.dll. A partially-updated install
        // would otherwise publish a number the runtime refuses.
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = std::move(exePathUtf8);
        // error_handler_t::replace, not the default throw: this probe is the ONE
        // thing the Hub relies on to learn the ABI, so a malformed byte must
        // degrade to U+FFFD in the output, never an exception escaping main()
        // into terminate(). ExecutablePathUtf8 already yields well-formed UTF-8;
        // this is the belt for every other caller.
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    // Load the input action maps from the layered config's "input" category (engine
    // default EngineConfig/input.json, deep-merged with the project's Config/input.json).
    // Sets the "demo" base context on success. Returns false if the category is
    // absent/malformed (the host logs and continues -- input stays inert).
    inline bool LoadInputConfig(Arcane::InputActions& input, const Arcane::Config& config)
    {
        if (!input.LoadJson(config.Category("input")))
            return false;
        input.SetBaseContext("demo");
        return true;
    }

    // Apply the layered config's "diagnostics" category (engine default
    // EngineConfig/diagnostics.json, deep-merged with a project's
    // Config/diagnostics.json) to the render-side instrumentation.
    //
    // Only `drawMarkers` today: per-draw GPU markers, off by default because
    // they are a debugging aid, not telemetry -- they cost a marker pair per
    // batch run and exist for the sessions where someone is chasing a hang with
    // PIX/RenderDoc open. PASS-level scopes are unconditional and are NOT
    // configurable: they are what a crash report is built from, so a config file
    // must never be able to turn the diagnostics off.
    inline void ApplyDiagnosticsConfig(const Arcane::Config& config)
    {
        // is_object() before value(): a category whose file was authored as an
        // array or a scalar would make value() THROW, and a malformed config
        // file must not take a host down over a debugging toggle.
        const nlohmann::json& diagnostics = config.Category("diagnostics");
        SetGpuDrawMarkersEnabled(diagnostics.is_object() &&
                                 diagnostics.value("drawMarkers", false));
    }

    // The game module to host: the project's gameModule when a project is open and it
    // names one, else the fallback --plugin path.
    //
    // A project builds its own game DLL into <project>/Binaries/ (engine-as-SDK model),
    // so when that built copy exists we return its absolute path -- the host loads the
    // project's OWN module rather than a same-named DLL sitting beside the exe. If the
    // Binaries/ copy isn't there (a demo project that BORROWS a host-adjacent DLL, e.g.
    // ReferenceProject -> Sandbox.dll), we fall through to the bare name resolved beside the
    // host exe -- keeping the borrowing path working.
    inline std::string GameModule(const Arcane::Project* project, const std::string& fallback)
    {
        if (project && !project->Manifest().gameModule.empty())
        {
            const std::string& mod = project->Manifest().gameModule;
            std::error_code ec;
            const std::filesystem::path built = project->Root() / "Binaries" / mod;
            if (std::filesystem::exists(built, ec))
                return built.string();
            return mod;
        }
        return fallback;
    }

    // The secondary plugin modules a host should load: each enabled manifest plugin that
    // has built a DLL at <root>/Plugins/<name>/Binaries/<name>.dll. A content-only plugin
    // (no Source/ -> no DLL) contributes only its plugin:// content mount (added at
    // Project::Open) and is skipped here. Feed each to PluginHost::AddPlugin before Load().
    inline std::vector<std::filesystem::path> PluginModules(const Arcane::Project* project)
    {
        std::vector<std::filesystem::path> out;
        if (!project)
            return out;
        std::error_code ec;
        for (const auto& ref : project->Manifest().plugins)
        {
            if (!ref.enabled)
                continue;
            std::filesystem::path dll = project->Root() / "Plugins" / ref.name / "Binaries" / (ref.name + ".dll");
            if (std::filesystem::exists(dll, ec))
                out.push_back(std::move(dll));
        }
        return out;
    }

    // The project's boot scene as a physical file, or empty when it has none /
    // the id names nothing this project contains.
    //
    // Split out from BootScene so the RESOLUTION is unit-testable without a
    // Runtime: it is the part with the interesting failure modes.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project)
    {
        const std::string& text = project.Manifest().bootScene;
        if (text.empty()) return {};

        const std::optional<Arcane::Guid> id = Arcane::Guid::FromString(text);
        if (!id || !id->IsValid())
        {
            ARC_WARN("bootScene '{}' is not a valid asset id", text);
            return {};
        }

        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(*id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", text);
            return {};
        }
        return *file;
    }

    // Resolve `id` directly to a physical file in `project`'s AssetRegistry -- the
    // Guid-known counterpart to BootSceneFile(project) above, for a caller that
    // already HAS a Guid instead of the manifest's bootScene text (a runtime
    // host's `--scene` override, HostConfig::sceneOverride). Same failure mode/
    // message as the manifest path's "does not resolve to a file" case.
    inline std::filesystem::path BootSceneFile(const Arcane::Project& project, const Arcane::Guid& id)
    {
        const std::optional<std::filesystem::path> file =
            project.ResolveAsset(Arcane::AssetId::FromGuid(id));
        if (!file)
        {
            ARC_WARN("bootScene {} does not resolve to a file in this project", id.ToString());
            return {};
        }
        return *file;
    }

    // What BootScene loaded, handed back so the caller (the editor's
    // SceneSession::Adopt) can record the session's file + id WITHOUT a second
    // ReadSceneFile of the same path just to recover the Guid, which is what
    // the original plan for this function would have made every caller do.
    struct BootSceneResult
    {
        std::filesystem::path file;   // the .arcscene BootScene just applied
        Arcane::Guid          id;     // its asset id, straight from the parsed document
    };

    namespace Detail
    {
        // Shared body of both BootScene overloads below: read + apply an
        // already-resolved scene file into `runtime`. `file` empty means the
        // caller's resolution step (either BootSceneFile overload) already
        // failed and already logged its own ARC_WARN reason -- this returns
        // nullopt silently here rather than logging again.
        //
        // Read before reset, same ordering rule as the editor's Open Scene: a
        // boot scene that fails to parse must not leave the host holding a
        // half-built registry. There is less to protect at boot than Open
        // Scene protects (no prior authored scene, only whatever the plugin's
        // Init happened to spawn), but the order is the same regardless.
        inline std::optional<BootSceneResult> ApplySceneFile(Arcane::Runtime& runtime,
                                                              const std::filesystem::path& file)
        {
            if (file.empty()) return std::nullopt;

            std::string err;
            const auto doc = Arcane::Scene::ReadSceneFile(file, &err);
            if (!doc)
            {
                ARC_ERROR("bootScene: {}", err);
                return std::nullopt;
            }

            runtime.ResetRegistry();
            if (!Arcane::Scene::ApplySceneDocument(*doc, runtime.Registry()))
            {
                // Validated but unloadable -- the failure mode ReadSceneFile's
                // structural gate cannot see (a component whose reflected field
                // type is unsupported; SceneAsset.hpp's E02-3 note). The registry
                // is already reset by contract, so leave a well-formed empty scene
                // rather than an empty-but-rootless one.
                ARC_ERROR("bootScene: {} parsed but could not be loaded", file.generic_string());
                Arcane::Scene::CreateEmpty(runtime.Registry());
                return std::nullopt;
            }

            ARC_INFO("Loaded boot scene {}", file.generic_string());
            return BootSceneResult{file, doc->id};
        }
    }

    // Load the project's boot scene into `runtime`, replacing whatever the
    // registry holds. nullopt when there is no boot scene or it could not be
    // loaded -- the reason is logged HERE, and callers simply continue with
    // whatever the registry already held (a project with no boot scene, or one
    // whose boot scene fails to
    // resolve/parse, is left exactly as it was -- nothing is reset until a
    // valid document is in hand) rather than refusing to open the project,
    // because the editor is how a broken boot scene gets fixed.
    //
    // Call AFTER the plugin loads: a scene naming a component the game module
    // registers would otherwise silently drop it.
    inline std::optional<BootSceneResult> BootScene(Arcane::Runtime& runtime, const Arcane::Project& project)
    {
        return Detail::ApplySceneFile(runtime, BootSceneFile(project));
    }

    // Load `id`'s scene file into `runtime` -- the Guid-known counterpart to
    // BootScene(runtime, project) above, for a runtime host's `--scene` override
    // (HostConfig::sceneOverride) once the override text has already parsed as a
    // Guid. Shares Detail::ApplySceneFile with the manifest-path overload above
    // for the read/reset/apply/log body; only the resolution step differs
    // (BootSceneFile(project, id) instead of the manifest's bootScene text), so
    // an override Guid that resolves to no asset in this project hits the exact
    // same "does not resolve to a file" path.
    inline std::optional<BootSceneResult> BootScene(Arcane::Runtime& runtime, const Arcane::Project& project,
                                                     const Arcane::Guid& id)
    {
        return Detail::ApplySceneFile(runtime, BootSceneFile(project, id));
    }

    // What a boot stage needs to do its work. Pointers are host-owned and
    // outlive the sequence; null members mean "that facility is absent in this
    // host", which stages must tolerate (the parity tests build one with all
    // members null).
    struct BootContext
    {
        Runtime*          runtime     = nullptr;
        GpuContext*       gpu         = nullptr;
        BootSplashWindow* splash      = nullptr;   // pre-device splash; closed by splash_ready
        const char*       projectPath = nullptr;
        const char*       pluginPath  = nullptr;
        // "ArcaneEditor.exe" / "ArcaneRuntime.exe" (Task 8): the identity the
        // genuinely-shared stage bodies (type_context_install, project_open,
        // input_config) log under -- VerifySharedTypeContext's diagnostic and
        // the --project-failed warning both need to say which host they ran
        // in. Null degrades to a generic "HostBoot" label, never a crash.
        const char*       moduleName  = nullptr;
    };

    // THE CANONICAL BOOT SEQUENCE. Both hosts take this LIST whole: the ids,
    // dependsOn, thread and weight below may not be omitted, reordered, or
    // rewritten by a host -- so divergence between the editor and the runtime
    // has to be written deliberately instead of forgotten. Three shipped bugs
    // (camera, sprite tables, Astra TypeContext) were exactly that forgetting.
    //
    // A stage's BODY is a different story (Task 8, 2026-07-30 review). Some
    // ids get a real, shared implementation right here (type_context_install,
    // project_open, input_config, and the editor-only editor_lock) because
    // their work is fully expressible through BootContext alone. The rest
    // are declared here with NO run callable (Make's `run` parameter
    // defaults to an empty std::function) because their real work needs a
    // HOST-OWNED object (EditorApp::m_gpu/m_runtime/m_plugin/m_resolver/
    // m_presenter, ArcaneRuntime's equivalents) or an editor-exe-only type
    // (EditorTheme/EditorFonts/ShaderEditorDocument) that this module,
    // compiled into Arcane.dll, cannot reach or see -- Arcane.dll cannot
    // depend on ArcaneEditor.exe. Each host is REQUIRED to overwrite that
    // stage's `.run` by id, after calling EditorStages/RuntimeStages, before
    // constructing a BootSequence (see EditorApp::Run / RuntimeApp::Run).
    // The editor-only splash_ready is one of these host-owned ids (Task 8c,
    // 2026-07-30 correction) -- it used to be ctx-only shared logic (plain
    // Window::Show()/BootSplashWindow::Close()), but revealing the window
    // now also means Present()-ing one real frame through the swapchain-
    // backed BootPresenter first (EditorApp::m_presenter), which this module
    // cannot reach; see EditorStages' splash_ready push_back for the reveal-
    // ordering reasoning and EditorApp::StageSplashReady for the body.
    //
    // An id with no host override left empty is NOT tolerated silently: Make()
    // substitutes a sentinel body that logs ARC_ERROR naming the exact id and
    // returns false, so a Fatal stage hard-aborts boot instead of the host
    // quietly skipping the step and reporting success -- the sentinel exists
    // BECAUSE "a host forgot a step and nothing said so" is the literal shape
    // of all three shipped bugs above, and BootStageParityTest's id-only
    // comparison cannot catch a host that received the right id but never
    // patched it. A host for which a given id is LEGITIMATELY a no-op (e.g.
    // ArcaneRuntime's edit_core, which has no scene-session/undo-history
    // analog) must patch it to an explicit `[]{ return true; }` with a
    // comment saying so -- relying on the sentinel to happen to look like
    // success is exactly what this paragraph exists to forbid.
    //
    // Adding an engine-wide install/publish step? Add the id HERE (with a
    // shared body if one is possible) and both hosts' lists gain it;
    // BootStageParityTest fails if a host's id list drops one, and the
    // sentinel fails loudly if a host's id list keeps it but never patches
    // (or renames/typos) it.
    [[nodiscard]] ARCANE_API std::vector<BootStage> CoreStages(BootContext& ctx);

    // Ids only -- no context needed, so tests and tooling can ask "what is the
    // canonical list?" without constructing a host.
    [[nodiscard]] ARCANE_API std::vector<std::string> CoreStageIds();

    // Exactly what each host builds, exposed for BootStageParityTest. These must
    // be the SAME functions the hosts call, not reimplementations -- a parallel
    // copy would test itself and prove nothing.
    [[nodiscard]] ARCANE_API std::vector<BootStage> EditorStages(BootContext& ctx);
    [[nodiscard]] ARCANE_API std::vector<BootStage> RuntimeStages(BootContext& ctx);
    [[nodiscard]] ARCANE_API std::vector<std::string> EditorStageIdsForTest(BootContext& ctx);
    [[nodiscard]] ARCANE_API std::vector<std::string> RuntimeStageIdsForTest(BootContext& ctx);
}
