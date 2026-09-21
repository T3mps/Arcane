// arcbuild's core and orchestration ([build]): command + flag parsing over Arcane::Cli,
// the --sdk / ARCANE_SDK precedence, the s4.3 decision table, exit-code
// mapping, path conventions, BackendResolver's expected-based tool/context
// resolution, and every composed child command line. Nothing here spawns a
// process or reads a PE file (BackendResolver tests that need a deterministic
// "nothing on PATH" answer override PATH/PATHEXT for their own scope only);
// temporary projects cover bootstrap and missing-slot inspection. The
// opt-in [build-desk] cases at the bottom (Task 3) are the only tests that
// reach it, SKIPping unless ARCANE_BUILD_DESK names a project directory.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Driver.hpp>
#include <Bootstrap.hpp>
#include <Output.hpp>
#include <Pipeline.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace arcbuild;

    // argv for Cli::Parse: [0] is the "program" slot Parse skips (main.cpp
    // hands it argv+1, so [0] is the command word there).
    struct Argv
    {
        std::vector<std::string> storage;
        std::vector<char*>       ptrs;
        explicit Argv(std::initializer_list<const char*> words)
        {
            for (const char* w : words) storage.emplace_back(w);
            for (std::string& s : storage) ptrs.push_back(s.data());
        }
        int    argc() const { return static_cast<int>(ptrs.size()); }
        char** argv()       { return ptrs.data(); }
    };

    ProjectLayout AphelyonProject()
    {
        ProjectLayout project;
        project.root       = "D:/dev/starworks/Gacha/Game";
        project.manifest   = "D:/dev/starworks/Gacha/Game/Aphelyon.arcproj";
        project.name       = "Aphelyon";
        project.gameModule = "Aphelyon.dll";
        return project;
    }

    fs::path PremakePath()
    {
        return "D:/dev/starworks/Arcane/ThirdParty/premake5/premake5.exe";
    }

    fs::path MsBuildPath()
    {
        return "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe";
    }

    struct TempArcProject
    {
        fs::path root;

        explicit TempArcProject(std::string_view leaf)
            : root(fs::temp_directory_path() / "arcbuild_orchestration_test" / leaf)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            fs::create_directories(root);

            std::ofstream manifest(root / "Fixture.arcproj", std::ios::binary);
            manifest << R"({
  "formatVersion": 2,
  "name": "Fixture",
  "engine": { "abi": 37 },
  "gameModule": "Fixture.dll"
})";
        }

        ~TempArcProject()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    // A plain, empty scratch directory -- BackendResolver's tool-lookup
    // tests use this both as a bare "no bundled Premake here" SDK root and
    // as a controlled, empty PATH entry.
    struct TempDir
    {
        fs::path path;
        explicit TempDir(std::string_view tag)
            : path(fs::temp_directory_path() / "arcbuild_orchestration_test" /
                   (std::string("dir_") + std::string(tag)))
        {
            std::error_code ec;
            fs::remove_all(path, ec);
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    // RAII process-environment override. BackendResolver::ResolveBuilder and
    // ::ResolvePremake ultimately call into Arcane::Toolchain's Resolve*
    // functions, which read PATH/PATHEXT live via std::getenv -- so proving
    // "nothing on PATH" (and therefore a descriptive std::unexpected rather
    // than an optimistic bare name) needs a real, restored-on-scope-exit
    // environment mutation, same technique as ToolchainTest.cpp.
    class EnvOverride
    {
    public:
        EnvOverride(const char* name, const std::string& value)
            : name_(name)
        {
            if (const char* existing = std::getenv(name))
                previous_ = existing;
            Set(value);
        }

        ~EnvOverride()
        {
            Set(previous_.value_or(std::string()));
        }

        EnvOverride(const EnvOverride&) = delete;
        EnvOverride& operator=(const EnvOverride&) = delete;

    private:
        void Set(const std::string& value)
        {
#ifdef _WIN32
            _putenv_s(name_.c_str(), value.c_str());
#else
            if (value.empty())
                unsetenv(name_.c_str());
            else
                setenv(name_.c_str(), value.c_str(), 1);
#endif
        }

        std::string name_;
        std::optional<std::string> previous_;
    };

    struct FakeEnvironment final : IEnvironment
    {
        std::optional<fs::path> sdk;
        mutable int             setCalls = 0;
        mutable fs::path        exported;
        bool                    setResult = true;

        std::optional<fs::path> ArcaneSdk() const override
        {
            return sdk;
        }

        bool SetArcaneSdk(const fs::path& root) const override
        {
            ++setCalls;
            exported = root;
            return setResult;
        }
    };

    struct RecordingOutput final : IOutput
    {
        bool                     quiet = false;
        mutable std::vector<std::string> messages;

        void SetQuiet(bool value) noexcept override
        {
            quiet = value;
        }

        void Info(std::string_view message) const override
        {
            if (!quiet)
                messages.emplace_back("info:" + std::string(message));
        }

        void Always(std::string_view message) const override
        {
            messages.emplace_back("always:" + std::string(message));
        }

        void Error(std::string_view message) const override
        {
            messages.emplace_back("error:" + std::string(message));
        }

        void Child(std::string_view prefix, std::string_view message) const override
        {
            messages.emplace_back(std::string(prefix) + ":" + std::string(message));
        }
    };

    struct RecordingExecutor final : IBuildExecutor
    {
        std::vector<std::string>& events;
        int generateExit = kExitOk;
        int buildExit = kExitOk;
        int cleanExit = kExitOk;

        explicit RecordingExecutor(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        int Generate(const DriverContext&) const override
        {
            events.emplace_back("generate");
            return generateExit;
        }

        int Build(const DriverContext&, BuildOperation operation) const override
        {
            events.emplace_back(operation == BuildOperation::Rebuild
                ? "rebuild"
                : "build");
            return buildExit;
        }

        int CleanBackend(const DriverContext&) const override
        {
            events.emplace_back("backend-clean");
            return cleanExit;
        }
    };

    struct RecordingSlots final : ISlotInspector
    {
        std::vector<std::string>& events;
        SlotState state = SlotState::Match;

        explicit RecordingSlots(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        SlotProbe Inspect(const ProjectLayout&, std::string_view) const override
        {
            events.emplace_back("inspect");
            SlotProbe probe;
            probe.state = state;
            return probe;
        }

        std::string Describe(const SlotProbe&, std::string_view, const Verdict&) const override
        {
            return "probe row";
        }
    };

    struct RecordingCleaner final : IProjectCleaner
    {
        std::vector<std::string>& events;
        int cleanExit = kExitOk;

        explicit RecordingCleaner(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        int Clean(const ProjectLayout&, std::string_view) const override
        {
            events.emplace_back("filesystem-clean");
            return cleanExit;
        }
    };

    DriverContext PipelineContext(Command command)
    {
        DriverContext context;
        context.request.command = command;
        context.request.action = "vs2026";
        context.backend = BuildBackend::MsBuild;
        context.sdkRoot = fs::path("D:/sdk");
        context.project = AphelyonProject();
        return context;
    }
}

// ---- CLI -------------------------------------------------------------------

TEST_CASE("arcbuild::ParseCommand knows the five commands and nothing else", "[build]")
{
    CHECK(ParseCommand("generate") == Command::Generate);
    CHECK(ParseCommand("build")    == Command::Build);
    CHECK(ParseCommand("rebuild")  == Command::Rebuild);
    CHECK(ParseCommand("clean")    == Command::Clean);
    CHECK(ParseCommand("probe")    == Command::Probe);
    CHECK_FALSE(ParseCommand("Build").has_value());     // exact, lower-case
    CHECK_FALSE(ParseCommand("--project").has_value()); // a flag is not a command
    CHECK_FALSE(ParseCommand("").has_value());
    // Round-trip: the name printed in the log is the word that parses.
    for (Command c : { Command::Generate, Command::Build, Command::Rebuild, Command::Clean, Command::Probe })
        CHECK(ParseCommand(CommandName(c)) == c);
}

TEST_CASE("arcbuild action mapping only promises verified build backends", "[build]")
{
    CHECK(BackendForAction("vs2022") == BuildBackend::MsBuild);
    CHECK(BackendForAction("vs2026") == BuildBackend::MsBuild);
    CHECK(BackendForAction("gmake") == BuildBackend::Make);
    CHECK(BackendForAction("gmakelegacy") == BuildBackend::Make);
    CHECK(BackendForAction("ninja") == BuildBackend::Ninja);
    CHECK(BackendForAction("xcode4") == BuildBackend::XcodeBuild);
    CHECK(BackendForAction("vs2019") == BuildBackend::None);
    CHECK(BackendForAction("compilecommands") == BuildBackend::None);
}

TEST_CASE("arcbuild default action is host-specific", "[build]")
{
    CHECK(DefaultActionFor(HostPlatform::Windows) == "vs2026");
    CHECK(DefaultActionFor(HostPlatform::Linux) == "gmake");
    CHECK(DefaultActionFor(HostPlatform::MacOS) == "xcode4");
}

TEST_CASE("arcbuild::MakeCli + RequestFromCli carry every flag of spec s3", "[build]")
{
    Arcane::Cli cli = MakeCli();
    SECTION("all flags supplied")
    {
        Argv a{ "build", "--project", "D:/dev/starworks/Gacha/Game", "--config", "Release",
                "--sdk", "D:/dev/starworks/Arcane", "--action", "gmake2", "--force-rebuild", "--quiet" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Build, r);
        CHECK(req.command == Command::Build);
        CHECK(req.project == fs::path("D:/dev/starworks/Gacha/Game"));
        CHECK(req.config  == "Release");
        REQUIRE(req.sdk.has_value());
        CHECK(*req.sdk == fs::path("D:/dev/starworks/Arcane"));
        CHECK(req.action == "gmake2");
        CHECK(req.forceRebuild);
        CHECK(req.quiet);
    }
    SECTION("defaults: Debug, host action, no sdk, no force, not quiet")
    {
        Argv a{ "probe", "--project", "X" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Probe, r);
        CHECK(req.config == "Debug");
        CHECK(req.action == DefaultActionFor(CurrentHostPlatform()));
        CHECK_FALSE(req.sdk.has_value());
        CHECK_FALSE(req.forceRebuild);
        CHECK_FALSE(req.quiet);
    }
    SECTION("an injected default action is carried through parsing")
    {
        Arcane::Cli linuxCli = MakeCli("gmake");
        Argv a{ "build", "--project", "X" };
        const Arcane::Cli::Result r = linuxCli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        CHECK(RequestFromCli(Command::Build, r).action == "gmake");
    }
    SECTION("--project is required")
    {
        Argv a{ "build" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
        CHECK(r.exitCode == kExitRefused);
    }
    SECTION("--config is validated against the three configurations")
    {
        Argv a{ "build", "--project", "X", "--config", "Shipping" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
    }
    SECTION("explicit empty --sdk is engaged-but-empty, not unsupplied")
    {
        Argv a{ "build", "--project", "X", "--sdk", "" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Build, r);
        REQUIRE(req.sdk.has_value());
        CHECK(req.sdk->empty());
    }
}

TEST_CASE("arcbuild::IsValidAction is a premake identifier, not a shell fragment", "[build]")
{
    CHECK(IsValidAction("vs2026"));
    CHECK(IsValidAction("gmake2"));
    CHECK(IsValidAction("ninja"));
    CHECK(IsValidAction("vs_2026"));
    CHECK(IsValidAction("gmake-2"));
    CHECK_FALSE(IsValidAction(""));
    CHECK_FALSE(IsValidAction("vs2026 & echo hi"));
    CHECK_FALSE(IsValidAction("vs2026;whoami"));
    CHECK_FALSE(IsValidAction("vs2026|dir"));
    CHECK_FALSE(IsValidAction("vs2026 && echo"));
    CHECK_FALSE(IsValidAction("vs2026.exe"));
    CHECK_FALSE(IsValidAction("vs 2026"));
}

TEST_CASE("arcbuild::ValidateRequest refuses empty --sdk, a non-identifier --action, and probe-only --force-rebuild",
          "[build]")
{
    Request ok;
    ok.command = Command::Build;
    ok.action  = "vs2026";
    CHECK_FALSE(ValidateRequest(ok).has_value());

    Request emptySdk = ok;
    emptySdk.sdk = fs::path();
    REQUIRE(ValidateRequest(emptySdk).has_value());
    CHECK(*ValidateRequest(emptySdk) == "--sdk requires a non-empty path (an empty value is not ARCANE_SDK)");

    Request badAction = ok;
    badAction.action = "vs2026 & echo hi";
    REQUIRE(ValidateRequest(badAction).has_value());
    CHECK(ValidateRequest(badAction)->find("invalid --action") != std::string::npos);

    Request probeForce;
    probeForce.command      = Command::Probe;
    probeForce.action       = "vs2026";
    probeForce.forceRebuild = true;
    REQUIRE(ValidateRequest(probeForce).has_value());
    CHECK(ValidateRequest(probeForce)->find("--force-rebuild") != std::string::npos);

    for (const Command command : { Command::Generate, Command::Build, Command::Rebuild, Command::Clean })
    {
        Request force = ok;
        force.command = command;
        force.forceRebuild = true;
        CHECK_FALSE(ValidateRequest(force).has_value());
    }
}

// ---- SDK precedence ---------------------------------------------------------

TEST_CASE("arcbuild::ResolveSdk: --sdk beats ARCANE_SDK beats refusal", "[build]")
{
    const fs::path flag = "D:/flag/sdk";
    CHECK(ResolveSdk(flag, "D:/env/sdk") == flag);
    CHECK(ResolveSdk(flag, nullptr)      == flag);
    CHECK(ResolveSdk(std::nullopt, "D:/env/sdk") == fs::path("D:/env/sdk"));
    CHECK_FALSE(ResolveSdk(std::nullopt, nullptr).has_value());
    CHECK_FALSE(ResolveSdk(std::nullopt, "").has_value());   // set-but-empty is unset
}

// ---- bootstrap --------------------------------------------------------------

TEST_CASE("arcbuild probe bootstrap needs no SDK and does not export one", "[build]")
{
    TempArcProject project("probe_without_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Probe;
    request.project = project.root;

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->sdkRoot.has_value());
    CHECK(environment.setCalls == 0);
}

TEST_CASE("arcbuild non-probe bootstrap refuses a missing SDK", "[build]")
{
    TempArcProject project("build_without_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Build;
    request.project = project.root;

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("no SDK") != std::string::npos);
    CHECK(environment.setCalls == 0);
}

TEST_CASE("arcbuild explicit SDK is absolutized and exported for non-probe commands", "[build]")
{
    TempArcProject project("explicit_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Generate;
    request.project = project.root;
    request.sdk = fs::path("relative-sdk-root");

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE(result.has_value());
    REQUIRE(result->sdkRoot.has_value());
    CHECK(result->sdkRoot->is_absolute());
    CHECK(environment.setCalls == 1);
    CHECK(environment.exported == *result->sdkRoot);
}

// ---- the s4.3 decision table ---------------------------------------------------

TEST_CASE("arcbuild::ConfigWantsDebugCrt: Debug only; Dist maps to Release for the probe", "[build]")
{
    CHECK(ConfigWantsDebugCrt("Debug"));
    CHECK_FALSE(ConfigWantsDebugCrt("Release"));
    CHECK_FALSE(ConfigWantsDebugCrt("Dist"));
}

TEST_CASE("arcbuild::ClassifySlot lands on exactly one s4.3 row", "[build]")
{
    using Arcane::CrtFlavor;
    // absent -> nothing to be wrong about, whatever the scanner would say
    CHECK(ClassifySlot(false, CrtFlavor::Unknown, "Debug")   == SlotState::Absent);
    CHECK(ClassifySlot(false, CrtFlavor::Debug,   "Release") == SlotState::Absent);
    // present, flavor matches --config
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Debug")   == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Release") == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Dist")    == SlotState::Match);   // Dist == release CRT
    // present, flavor mismatches
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Debug")   == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Release") == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Dist")    == SlotState::Mismatch);
    // present, unreadable -> its own row, never silently "match"
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Debug")   == SlotState::Unreadable);
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Release") == SlotState::Unreadable);
}

TEST_CASE("arcbuild::Decide: plain for absent/match and rebuild for mismatch/unreadable",
          "[build]")
{
    // THE INCREMENTAL RULE (spec s4.3) -- the reason the driver exists now. A
    // game project's Binaries\ is ONE slot shared by every configuration; an
    // incremental msbuild reasons about the per-config object tree and can
    // report "up to date" over the OTHER config's DLL (observed live: a
    // 0.24s "All outputs are up-to-date" followed by "the rebuilt module
    // still failed to load"). The editor used to force /t:Rebuild on every
    // build to close that; the driver forces it ONLY when the slot's CRT
    // flavor says it must, so a wizard-made component costs one TU + a link.
    SECTION("the decision follows the slot")
    {
        CHECK_FALSE(Decide(SlotState::Absent).rebuild);
        CHECK_FALSE(Decide(SlotState::Match).rebuild);
        CHECK(Decide(SlotState::Mismatch).rebuild);
        CHECK(Decide(SlotState::Unreadable).rebuild);
    }
    SECTION("every verdict says why, and the unreadable one says so in words")
    {
        for (SlotState s : { SlotState::Absent, SlotState::Match, SlotState::Mismatch, SlotState::Unreadable })
        {
            const Verdict v = Decide(s);
            REQUIRE(v.reason != nullptr);
            CHECK(std::string(v.reason).size() > 10);
        }
        CHECK(std::string(Decide(SlotState::Unreadable).reason).find("unreadable") != std::string::npos);
        CHECK(std::string(Decide(SlotState::Mismatch).reason).find("rebuild") != std::string::npos);
    }
}

TEST_CASE("arcbuild selects rebuild only for forced or rebuilding slot verdicts", "[build]")
{
    CHECK(OperationForBuild(false, Decide(SlotState::Absent)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Match)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Mismatch)) == BuildOperation::Rebuild);
    CHECK(OperationForBuild(true, Decide(SlotState::Match)) == BuildOperation::Rebuild);
}

// ---- exit codes -------------------------------------------------------------

TEST_CASE("arcbuild exit codes: probe 0 on the plain rows, 3 on the rebuild rows; a dead pipe is a refusal", "[build]")
{
    CHECK(ProbeExitCode(SlotState::Absent)     == kExitOk);
    CHECK(ProbeExitCode(SlotState::Match)      == kExitOk);
    CHECK(ProbeExitCode(SlotState::Mismatch)   == kExitProbeRebuild);
    CHECK(ProbeExitCode(SlotState::Unreadable) == kExitProbeRebuild);
    CHECK(kExitRefused == 2);
    CHECK(kExitProbeRebuild == 3);
    // A child's own status passes through untouched (premake and msbuild
    // both exit 1 on failure; 9009 is cmd's "not found").
    CHECK(ExitFromChild(0)    == 0);
    CHECK(ExitFromChild(1)    == 1);
    CHECK(ExitFromChild(9009) == 9009);
    CHECK(ExitFromChild(std::nullopt) == kExitRefused);
}

TEST_CASE("filesystem clean failure outranks backend clean failure", "[build]")
{
    CHECK(MergeCleanResults(0, 0) == 0);
    CHECK(MergeCleanResults(7, 0) == 7);
    CHECK(MergeCleanResults(0, kExitRefused) == kExitRefused);
    CHECK(MergeCleanResults(7, kExitRefused) == kExitRefused);
}

// ---- orchestration ----------------------------------------------------------

TEST_CASE("arcbuild build orchestration orders work and short-circuits generation failures", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);

    SECTION("ordinary build generates, inspects, then builds")
    {
        CHECK(pipeline.Run(PipelineContext(Command::Build)) == kExitOk);
        CHECK(events == std::vector<std::string>{ "generate", "inspect", "build" });
    }

    SECTION("failed generation stops before inspection and build")
    {
        executor.generateExit = 17;
        CHECK(pipeline.Run(PipelineContext(Command::Build)) == 17);
        CHECK(events == std::vector<std::string>{ "generate" });
    }

    SECTION("forced build rebuilds without inspecting the slot")
    {
        DriverContext context = PipelineContext(Command::Build);
        context.request.forceRebuild = true;
        CHECK(pipeline.Run(context) == kExitOk);
        CHECK(events == std::vector<std::string>{ "generate", "rebuild" });
    }
}

TEST_CASE("arcbuild probe only inspects and reports even when output is quiet", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    output.SetQuiet(true);
    BuildPipeline pipeline(executor, slots, cleaner, output);

    DriverContext context = PipelineContext(Command::Probe);
    context.sdkRoot.reset();
    CHECK(pipeline.Run(context) == kExitOk);
    CHECK(events == std::vector<std::string>{ "inspect" });
    REQUIRE(output.messages.size() == 1);
    CHECK(output.messages[0] == "always:probe row");
}

TEST_CASE("arcbuild probe status reports an absent SDK", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);
    DriverContext context = PipelineContext(Command::Probe);
    context.sdkRoot.reset();
    slots.state = SlotState::Mismatch;

    CHECK(pipeline.Run(context) == kExitProbeRebuild);
    CHECK(events == std::vector<std::string>{ "inspect" });
    REQUIRE(output.messages.size() == 2);
    CHECK(output.messages[0].find("against SDK <none>") != std::string::npos);
    CHECK(output.messages[1] == "always:probe row");
}

TEST_CASE("arcbuild slot inspection distinguishes a missing slot from an unreadable slot", "[build]")
{
    TempArcProject fixture("slot_inspection");
    ProjectLayout project;
    project.root = fixture.root;
    project.gameModule = "Fixture.dll";
    SlotInspector inspector;

    SECTION("missing Binaries directory")
    {
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Absent);
    }

    SECTION("missing module file")
    {
        fs::create_directory(project.root / "Binaries");
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Absent);
    }

    SECTION("directory in place of module")
    {
        fs::create_directories(project.root / "Binaries" / "Fixture.dll");
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Unreadable);
    }
}

TEST_CASE("arcbuild errors remain visible under quiet output", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    output.SetQuiet(true);
    BuildPipeline pipeline(executor, slots, cleaner, output);

    DriverContext context = PipelineContext(Command::Build);
    context.backend = BuildBackend::None;

    CHECK(pipeline.Run(context) == kExitRefused);
    CHECK(events.empty());
    REQUIRE(output.messages.size() == 1);
    CHECK(output.messages[0].rfind("error:", 0) == 0);
}

TEST_CASE("arcbuild clean runs filesystem cleanup after a backend failure", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    executor.cleanExit = 7;
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);

    CHECK(pipeline.Run(PipelineContext(Command::Clean)) == 7);
    CHECK(events == std::vector<std::string>{ "backend-clean", "filesystem-clean" });

    events.clear();
    cleaner.cleanExit = kExitRefused;
    CHECK(pipeline.Run(PipelineContext(Command::Clean)) == kExitRefused);
    CHECK(events == std::vector<std::string>{ "backend-clean", "filesystem-clean" });
}

// ---- paths ------------------------------------------------------------------

TEST_CASE("arcbuild::SlotPath is <root>/Binaries/<gameModule>, empty for a content-only project", "[build]")
{
    ProjectLayout project = AphelyonProject();
    CHECK(SlotPath(project) == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries" / "Aphelyon.dll");
    project.gameModule.clear();
    CHECK(SlotPath(project).empty());
}

TEST_CASE("arcbuild::SolutionPath: a discovered workspace file wins over the <name>.slnx convention", "[build]")
{
    const ProjectLayout project = AphelyonProject();
    CHECK(SolutionPath(project, "D:/dev/starworks/Gacha/Game/Other.sln") == fs::path("D:/dev/starworks/Gacha/Game/Other.sln"));
    CHECK(SolutionPath(project, {}) == fs::path("D:/dev/starworks/Gacha/Game") / "Aphelyon.slnx");
    // ComposeMsBuild does not cd: a relative discovery is joined onto root
    // rather than assumed absolute (DiscoverSolution is absolute iff projectRoot is).
    CHECK(SolutionPath(project, fs::path("Other.sln")) ==
          (fs::path("D:/dev/starworks/Gacha/Game") / "Other.sln").lexically_normal());
}

// ---- composition --------------------------------------------------------------

TEST_CASE("arcbuild::ComposeGenerate is cd-first, parenthesised, stderr-folded premake", "[build]")
{
    const std::string cmd = ComposeGenerate(AphelyonProject(), PremakePath(), "vs2026");
    CHECK(cmd == "( cd /d \"D:/dev/starworks/Gacha/Game\" && "
                 "\"D:/dev/starworks/Arcane/ThirdParty/premake5/premake5.exe\" vs2026 ) 2>&1");
    // The action is the Linux seam (spec s6): it is a parameter, not a constant.
    CHECK(ComposeGenerate(AphelyonProject(), PremakePath(), "gmake2").find("premake5.exe\" gmake2 )") != std::string::npos);
}

TEST_CASE("arcbuild::ComposeMsBuild: /t:Rebuild ONLY when asked, /t:Clean for clean, no cd, absolute solution", "[build]")
{
    const fs::path sln = "D:/dev/starworks/Gacha/Game/Aphelyon.slnx";
    const BackendContext context { sln };
    const std::string plain = ComposeMsBuild(MsBuildPath(), context, "Debug", BuildOperation::Build);
    CHECK(plain == "( \"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe\" "
                   "\"D:/dev/starworks/Gacha/Game/Aphelyon.slnx\" /p:Configuration=Debug /m /nologo ) 2>&1");
    CHECK(plain.find("/t:") == std::string::npos);
    CHECK(plain.find("cd /d") == std::string::npos);

    const std::string rebuild = ComposeMsBuild(MsBuildPath(), context, "Release", BuildOperation::Rebuild);
    CHECK(rebuild.find("/p:Configuration=Release /t:Rebuild /m /nologo") != std::string::npos);

    const std::string clean = ComposeMsBuild(MsBuildPath(), context, "Dist", BuildOperation::Clean);
    CHECK(clean.find("/p:Configuration=Dist /t:Clean /m /nologo") != std::string::npos);
}

TEST_CASE("arcbuild::CleanTargets is exactly Binaries/ and Intermediate/<config>/", "[build]")
{
    const std::vector<fs::path> t = CleanTargets(AphelyonProject(), "Debug");
    REQUIRE(t.size() == 2);
    CHECK(t[0] == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries");
    CHECK(t[1] == fs::path("D:/dev/starworks/Gacha/Game") / "Intermediate" / "Debug");
    // Never Source/, Content/, Saved/, the .slnx, nor Intermediate/Artifacts (arccook's).
    for (const fs::path& p : t)
    {
        const std::string s = p.generic_string();
        CHECK(s.find("Source") == std::string::npos);
        CHECK(s.find("Content") == std::string::npos);
        CHECK(s.find("Saved") == std::string::npos);
        CHECK(s.find("Artifacts") == std::string::npos);
        CHECK(s.find(".slnx") == std::string::npos);
    }
}

// ---- backend resolver --------------------------------------------------------

TEST_CASE("arcbuild::BackendResolver refuses BuildBackend::None for build resolution", "[build]")
{
    const BackendResolver resolver;

    const auto builder = resolver.ResolveBuilder(BuildBackend::None);
    REQUIRE_FALSE(builder.has_value());
    CHECK_FALSE(builder.error().empty());

    const auto context = resolver.ResolveBackendContext(BuildBackend::None, AphelyonProject());
    REQUIRE_FALSE(context.has_value());
    CHECK_FALSE(context.error().empty());
}

TEST_CASE("arcbuild::BackendResolver turns an empty low-level tool lookup into a descriptive std::unexpected",
          "[build]")
{
    // A controlled, empty PATH: none of Premake/Make/Ninja are really
    // findable there, so each lookup below exercises the "concrete
    // discovery came back empty" branch deterministically, regardless of
    // what happens to be installed on this desk.
    TempDir emptyPath("backend_resolver_missing_tools");
    EnvOverride path("PATH", emptyPath.path.string());
    EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

    const BackendResolver resolver;

    SECTION("Premake: no bundled copy in the SDK root, nothing on PATH")
    {
        TempDir sdkRoot("backend_resolver_premake_sdk");
        const auto result = resolver.ResolvePremake(sdkRoot.path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Premake") != std::string::npos);
    }

    SECTION("Make: nothing on PATH")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::Make);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Make") != std::string::npos);
    }

    SECTION("Ninja: nothing on PATH")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::Ninja);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Ninja") != std::string::npos);
    }

    SECTION("XcodeBuild: no macOS toolchain to find")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::XcodeBuild);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("xcodebuild") != std::string::npos);
    }
}

TEST_CASE("arcbuild::BackendResolver reports a missing generated build context descriptively", "[build]")
{
    TempArcProject fixture("backend_resolver_context");
    ProjectLayout project;
    project.root = fixture.root;
    project.name = "Fixture";

    const BackendResolver resolver;

    SECTION("MsBuild: no generated solution file")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::MsBuild, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("Make: no Makefile")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::Make, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("Ninja: no build.ninja")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::Ninja, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("XcodeBuild: no .xcodeproj directory")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::XcodeBuild, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }
}

// ---------------------------------------------------------------------------
// The opt-in desk probe. Off by default: SKIPs unless ARCANE_BUILD_DESK names
// an ABSOLUTE game-project directory (one with an .arcproj -- e.g.
// D:\dev\starworks\Gacha\Game), so the ordinary suite never spawns the
// driver. Runs the BUILT arcbuild.exe from the dev bin layout
// (../arcbuild/ beside this exe -- the editor's own ResolveDriver rule) via
// the editor's synchronous RunCapture. `probe` must answer with one row and
// exit 0 or 3; `generate` must exit 0 and leave a workspace file behind.
// Both are idempotent against a real project (premake rewrites only files
// whose content changed).
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <Project/ModuleBuild.hpp>        // RunCapture + ExeDir (editor helpers compiled into the tests)
#include <Arcane/Build/Toolchain.hpp>     // DiscoverSolution (the post-generate check)

namespace
{
    fs::path DeskDriverExe()
    {
        return (Arcane::Editor::ModuleBuild::ExeDir() / ".." / "arcbuild" / "arcbuild.exe").lexically_normal();
    }

    std::string DeskLine(const char* command, const fs::path& project)
    {
        std::string cmd = "( \"";
        cmd += DeskDriverExe().string();
        cmd += "\" ";
        cmd += command;
        cmd += " --project \"";
        cmd += project.string();
        cmd += "\" --config Debug ) 2>&1";
        return cmd;
    }
}

TEST_CASE("arcbuild probe answers one s4.3 row against a real project on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("probe", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK((*r.exit == kExitOk || *r.exit == kExitProbeRebuild));

    bool sawRow = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[arcbuild] probe:", 0) == 0 && line.find(" state=") != std::string::npos)
            sawRow = true;
    CHECK(sawRow);
}

TEST_CASE("arcbuild generate writes the project's workspace file on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("generate", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK(*r.exit == kExitOk);

    bool sawPremake = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[premake]", 0) == 0)
            sawPremake = true;
    CHECK(sawPremake);
    CHECK_FALSE(Arcane::Toolchain::DiscoverSolution(fs::path(env)).empty());
}
