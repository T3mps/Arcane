#include <Arcane/Host/EarlyConfig.hpp>

#include <Arcane/Base/Diagnostics.hpp>   // the local Diagnostic that keeps the early manifest read silent
#include <Arcane/Base/Runtime.hpp>   // ApplyEngineConfigRung
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::HostBoot
{
    void ApplyEarlyConfigRungs(HostConfig& cfg, CVarContext ctx, bool editor)
    {
        CVarRegistry& cvars = CVarRegistry::Get();
        // The ONE engine-config folder (S2-H): Paths' engine dir (a host's,
        // else the exe dir), recorded so the first Runtime does not re-read it.
        (void)ApplyEngineConfigRung();
        if (!cfg.projectPath.empty())
        {
            if (const auto manifest = Project::ResolveManifestFile(cfg.projectPath))
            {
                const std::filesystem::path root = manifest->parent_path();
                // Same directories and source strings as Runtime::OpenProject
                // (ProjectCVarDir / UserCVarDir / EditorUser).
                const Paths::Config projectPaths = Paths::ForProject(root);
                ApplyCVarDirectory(cvars, Paths::Resolve(Paths::Location::ProjectConfig, projectPaths),
                                   SetBy::Project, "project");
                // A local Diagnostic keeps this early read silent: a malformed
                // manifest is published once, by Project::Open (S6-7 deferral).
                Diagnostic manifestDiag;
                if (const auto projectManifest = ProjectManifest::LoadFile(*manifest, &manifestDiag))
                    ApplyLegacyManifestSettings(cvars, *projectManifest);
                if (editor)
                {
                    const std::filesystem::path editorUser = Paths::Get(Paths::Location::EditorUserDir);
                    if (!editorUser.empty())
                        ApplyCVarDirectory(cvars, editorUser / "Config", SetBy::EditorUser, "editor-user");
                }
                ApplyCVarDirectory(cvars, Paths::Join(Paths::Location::GameUserDir, projectPaths, "Config"),
                                   SetBy::User, "user");
            }
        }
        // --perf is diagnostics.perfLog on the CommandLine rung (settings arc
        // S6-2), ahead of the --set list so an explicit --set still wins. It is
        // the host's own flag, not a free-form --set, so it applies in the
        // Editor context in every build: in Dist `ctx` is LocalHost, whose
        // table refuses a Game setting, and perf logging stays usable there
        // (user decision 2026-10-06). The --set list keeps `ctx`.
        if (cfg.perf)
            ApplyCVarCommandLine(cvars, { "diagnostics.perfLog=1" }, CVarContext::Editor);
        // --backend / --no-vsync are render.backend / render.vsync on the
        // CommandLine rung (settings arc S6-16), ahead of the --set list so an
        // explicit --set still wins. An absent --backend leaves the rung alone.
        std::vector<std::string> sets;
        if (cfg.backendSupplied)
            sets.emplace_back(cfg.backend == GraphicsBackend::Vulkan ? "render.backend=Vulkan" : "render.backend=D3D12");
        if (!cfg.vsync) sets.emplace_back("render.vsync=false");
        sets.insert(sets.end(), cfg.cvarSets.begin(), cfg.cvarSets.end());
        ApplyCVarCommandLine(cvars, sets, ctx);
        cvars.PublishImmediate();
        // A FramesInFlight() read before this publish has already frozen the
        // pacing depth for the process; say so if it differs (FramePacing.hpp).
        // The graph's canvas and depth formats latch the same way
        // (RenderDeviceSettings.hpp).
        CheckFramesInFlightLatch();
        CheckGraphFormatLatch();

        // Restart settings the host carries to GpuContext::Create, the device
        // and the swapchain: from here on the HostConfig holds the published
        // values (GpuContext may still fall Vulkan back to D3D12).
        const RenderSettings& render = Settings<RenderSettings>();
        cfg.backend = render.backend;
        cfg.vsync   = render.vsync;
    }
}
