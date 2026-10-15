#include "Bootstrap.hpp"

#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <filesystem>
#include <utility>

namespace arcbuild
{
    BootstrapResult
        Bootstrap::Prepare(
            Request request) const
    {
        DriverContext context;

        context.request =
            std::move(request);

        context.backend =
            BackendForAction(
                context.request.action);

        std::error_code ec;

        const auto manifestFile =
            Arcane::Project::ResolveManifestFile(
                context.request.project);

        if (!manifestFile)
        {
            return std::unexpected(
                "no project: '" +
                context.request.project.string() +
                "' is not a project directory or .arcproj");
        }

        const auto manifest =
            Arcane::ProjectManifest::LoadFile(
                *manifestFile);

        if (!manifest)
        {
            return std::unexpected(
                "'" +
                manifestFile->string() +
                "' is not a valid .arcproj");
        }

        const auto absoluteManifest =
            std::filesystem::absolute(
                *manifestFile,
                ec);

        if (ec)
        {
            return std::unexpected(
                "could not absolutise '" +
                manifestFile->string() +
                "': " +
                ec.message());
        }

        context.project.manifest =
            absoluteManifest.lexically_normal();

        context.project.root =
            context.project.manifest.parent_path();

        context.project.name =
            manifest->name;

        context.project.gameModule =
            manifest->gameModule;

        if (context.request.command == Command::Probe)
            return context;

        const auto requestedSdk =
            ResolveSdk(
                context.request.sdk,
                environment_.ArcaneSdk());

        if (!requestedSdk)
        {
            return std::unexpected(
                "no SDK: pass --sdk <root> or set ARCANE_SDK "
                "to an Arcane engine checkout");
        }

        context.sdkRoot =
            std::filesystem::absolute(
                *requestedSdk,
                ec).lexically_normal();

        if (ec)
        {
            return std::unexpected(
                "could not absolutise SDK '" +
                requestedSdk->string() +
                "': " +
                ec.message());
        }

        if (!environment_.SetArcaneSdk(
            *context.sdkRoot))
        {
            return std::unexpected(
                "could not set ARCANE_SDK for child processes");
        }

        return context;
    }
}
