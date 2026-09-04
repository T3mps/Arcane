// arccook -- F2b Task 5: the offline asset cook CLI. Cooks a project's Content/ texture
// sources into Intermediate/Artifacts via Arcane::AssetPipeline::CookSession, the SAME
// session type the editor's future in-process cook (Task 12) drives -- the CLI never
// diverges from the editor on what "stale" or "cooked" means.
//
// Exit codes: cook path -- 0 on success (failed == 0), 1 on ANY cook failure.
// `--check` -- 0 clean (nothing stale), 2 stale (DISTINCT from a cook failure, so a CI
// script can tell "needs a cook" apart from "the cook itself is broken").
// `--dump-dds` -- 0 on a successful DDS write, 1 otherwise; cooks first so the artifact
// it dumps is current, then writes purely for eyeballing (never a load path).

#include "DdsDump.hpp"

#include <Arcane/AssetPipeline/ArtifactFormat.hpp>
#include <Arcane/AssetPipeline/ArtifactStore.hpp>
#include <Arcane/AssetPipeline/CookSession.hpp>
#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Guid.hpp>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
    void PrintLine(const std::filesystem::path& source, bool ok, const std::string& detail)
    {
        std::printf("%s %s%s%s\n", ok ? "OK  " : "FAIL", source.generic_string().c_str(),
                     detail.empty() ? "" : " -- ", detail.c_str());
    }
}

int main(int argc, char** argv)
{
    using namespace Arcane;
    using namespace Arcane::AssetPipeline;

    Cli cli{ "arccook", "Arcane offline asset cook (F2b Task 5) -- textures today" };
    cli.Option("project", "", "project folder to cook (required)").Required();
    cli.Flag("check", "report whether any source is stale without cooking "
                       "(exit 0 clean / 2 stale)");
    cli.Flag("verbose", "print one line per attempted source (OK/FAIL, cooked/up-to-date/failed)");
    cli.Option("dump-dds", "", "cook, then write the artifact for this source Guid to "
                               "<guid>.dds beside the current directory, for eyeballing only "
                               "(never a load path)");

    const Cli::Result r = cli.Parse(argc, argv);
    if (!r.ok) return r.exitCode;

    const std::filesystem::path projectDir = r.Get("project");
    std::error_code dirEc;
    if (projectDir.empty() || !std::filesystem::is_directory(projectDir, dirEc))
    {
        std::fprintf(stderr, "arccook: --project '%s' is not a directory\n", projectDir.string().c_str());
        return 1;
    }

    CookSession session;
    if (r.Flag("verbose"))
        session.SetProgress(&PrintLine);

    const std::string dumpGuidArg = r.Get("dump-dds");
    if (!dumpGuidArg.empty())
    {
        const std::optional<Guid> guid = Guid::FromString(dumpGuidArg);
        if (!guid || !guid->IsValid())
        {
            std::fprintf(stderr, "arccook: --dump-dds '%s' is not a valid Guid\n", dumpGuidArg.c_str());
            return 1;
        }

        // Cook first -- --dump-dds is a debug AID over a current artifact, not a bare
        // reader of whatever happens to already be on disk. The result itself is
        // unused here: a failed cook for THIS guid surfaces below as "no cooked
        // artifact for guid", which is the more specific, actionable message.
        [[maybe_unused]] const CookResult dumpCookResult = session.CookProject(projectDir);

        ArtifactStore store(projectDir / "Intermediate");
        store.RebuildIndexFromScan();
        const std::optional<std::uint64_t> key = store.Lookup(*guid);
        if (!key)
        {
            std::fprintf(stderr, "arccook: no cooked artifact for guid %s\n", guid->ToString().c_str());
            return 1;
        }

        const std::optional<LoadedArtifact> artifact = ReadTextureArtifact(store.PathFor(*key));
        if (!artifact)
        {
            std::fprintf(stderr, "arccook: artifact for guid %s failed to load\n", guid->ToString().c_str());
            return 1;
        }

        const std::filesystem::path ddsPath = guid->ToString() + ".dds";
        if (!arccook::WriteDds(ddsPath, *artifact))
        {
            std::fprintf(stderr, "arccook: failed to write %s\n", ddsPath.string().c_str());
            return 1;
        }

        std::printf("arccook: wrote %s\n", ddsPath.string().c_str());
        return 0;
    }

    if (r.Flag("check"))
    {
        const bool stale = session.CheckProject(projectDir);
        std::printf("arccook --check: %s\n", stale ? "stale" : "clean");
        return stale ? 2 : 0;
    }

    const CookResult result = session.CookProject(projectDir);
    std::printf("arccook: cooked=%zu upToDate=%zu failed=%zu\n", result.cooked, result.upToDate, result.failed);
    return (result.failed > 0) ? 1 : 0;
}
