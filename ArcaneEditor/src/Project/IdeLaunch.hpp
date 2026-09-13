#pragma once

// IdeLaunch: Build -> Open Visual Studio, and "open this source file in
// Visual Studio" from the Asset Browser's Source/ rows (the second step of
// the editor<->IDE surface; Source/ in the browser was the first).
//
// The model is Unreal's VisualStudioSourceCodeAccessor (read in the vendored
// .example tree before this was written): find the RUNNING Visual Studio that
// has THIS project's solution open and drive it -- activate its window, open
// the file in it -- and only when no such instance exists launch
// `devenv "<solution>" "<file>"` so the new instance comes up with the
// solution AND the file. Detection goes through the COM Running Object Table
// exactly as UE's primary (DTE) path does, with one deliberate difference in
// mechanism: every DTE call here is LATE-BOUND through IDispatch (Solution.
// FullName / MainWindow.Activate / ItemOperations.OpenFile resolved by name at
// runtime), so no EnvDTE type library has to exist on the machine that
// BUILDS the editor -- UE pays that cost with a WITH_VISUALSTUDIO_DTE build
// switch; this pays it with ~40 lines of IDispatch plumbing instead.
//
// Same split as ModuleBuild/RuntimeLaunch: the PURE halves (moniker
// predicate, solution-path equivalence, argv composition, outcome wording)
// are [editor]-tested; the COM half and the CreateProcessW launch are
// desk-verify territory, plus one opt-in [ide-desk] probe (IdeLaunchTest.cpp).

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor::IdeLaunch
{
    // ---- pure halves ([editor]-tested) --------------------------------------

    // True for a Running-Object-Table display name of a Visual Studio
    // automation object: "!VisualStudio.DTE.<major>.0:<pid>" (UE's ROTMoniker
    // shape, any major version). Express editions ("!WDExpress.DTE...") and
    // every other ROT entry are not ours.
    [[nodiscard]] bool IsVisualStudioMoniker(std::wstring_view rotDisplayName);

    // Do two solution paths name the same file? DTE's Solution.FullName comes
    // back backslashed and in whatever case VS holds; ours comes from
    // ModuleBuild::DiscoverSolution. Lexically normalised, separators unified,
    // case-insensitive (Windows paths). An EMPTY side never matches -- a VS
    // with no solution open reports an empty FullName.
    [[nodiscard]] bool SameSolutionPath(const std::filesystem::path& a,
                                        const std::filesystem::path& b);

    // The devenv argv for a NEW instance: the solution, then the file when
    // there is one (UE's RunVisualStudioAndOpenSolutionAndFiles shape). Tokens
    // are UNQUOTED -- RuntimeLaunch::QuoteArg escapes them at spawn.
    [[nodiscard]] std::vector<std::wstring> ComposeLaunchArgs(
        const std::filesystem::path& solution, const std::filesystem::path& file);

    // What a probe of the ROT concluded about THIS solution (UE's
    // EAccessVisualStudioResult, same four answers, same meanings):
    //   Open     -- a running VS has this solution open.
    //   NotOpen  -- no running VS has it (none running, or other solutions).
    //   Blocked  -- a VS is running but could not be queried (still starting,
    //               or parked in a modal) -- NEVER launch a second instance
    //               on this answer; it may well be about to become Open.
    //   Unknown  -- the ROT itself could not be reached; do nothing rather
    //               than loop on launches nobody can detect.
    enum class Access { Open, NotOpen, Blocked, Unknown };

    // What a click did, for the Console line. Count is the sentinel.
    enum class Outcome
    {
        Activated,          // solution already open -> its window brought forward
        OpenedInInstance,   // file opened in the already-running instance
        Launched,           // devenv started with the solution (+ file)
        Blocked,            // Access::Blocked -- try again in a moment
        DetectionFailed,    // Access::Unknown -- COM/ROT unavailable
        NoDevenv,           // vswhere found no devenv.exe (nothing to launch)
        NoSolution,         // no .slnx to open (generation must have failed)
        NoFile,             // the requested source file is not on disk
        LaunchFailed,       // CreateProcessW refused
        ActivateFailed,     // instance found, MainWindow.Activate failed
        OpenFailed,         // instance found, ItemOperations.OpenFile failed
        Count
    };
    [[nodiscard]] const char* Describe(Outcome outcome);

    // ---- resolution + COM (probe the machine; desk-verify) -----------------

    // devenv.exe via ModuleBuild::VsWhere("-latest -find Common7\IDE\devenv.exe");
    // empty when vswhere is absent or no IDE install answers (Build Tools
    // alone has no devenv). Resolve once and cache -- it spawns a process.
    [[nodiscard]] std::filesystem::path ResolveDevenv();

    // The ROT walk alone, for the [ide-desk] probe: which Access would a click
    // see right now for `solution`? Initialises COM for the call's duration.
    [[nodiscard]] Access Probe(const std::filesystem::path& solution);

    // Build -> Open Visual Studio. `solution` must already exist (the caller
    // generates it first when missing); `devenv` may be empty, in which case
    // only an already-running instance can satisfy the request.
    Outcome OpenSolution(const std::filesystem::path& devenv,
                         const std::filesystem::path& solution);

    // Open `file` in Visual Studio, in the instance that has `solution` open
    // when there is one, else in a new instance launched with both.
    Outcome OpenFile(const std::filesystem::path& devenv,
                     const std::filesystem::path& solution,
                     const std::filesystem::path& file);
}
