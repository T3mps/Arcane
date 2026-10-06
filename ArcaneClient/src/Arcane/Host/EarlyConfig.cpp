#include <Arcane/Host/EarlyConfig.hpp>

#include <Arcane/Base/Runtime.hpp>   // ApplyEngineConfigRung
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Project/Project.hpp>

#include <filesystem>

namespace Arcane::HostBoot
{
    void ApplyEarlyConfigRungs(const HostConfig& cfg, CVarContext ctx, bool editor)
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
        ApplyCVarCommandLine(cvars, cfg.cvarSets, ctx);
        cvars.PublishImmediate();
    }
}
