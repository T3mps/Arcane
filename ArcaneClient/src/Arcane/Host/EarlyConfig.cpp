#include <Arcane/Host/EarlyConfig.hpp>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/Project.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::HostBoot
{
    void ApplyEarlyConfigRungs(const HostConfig& cfg, CVarContext ctx, bool editor)
    {
        CVarRegistry& cvars = CVarRegistry::Get();
        const std::filesystem::path exeDir = std::filesystem::path(ExecutablePathUtf8()).parent_path();
        ApplyCVarDirectory(cvars, exeDir / "data" / "EngineConfig", SetBy::EngineConfig, "engine-config");
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
                if (const auto projectManifest = ProjectManifest::LoadFile(*manifest))
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
        ApplyCVarCommandLine(cvars, cfg.cvarSets, ctx);
        cvars.PublishImmediate();
    }
}
