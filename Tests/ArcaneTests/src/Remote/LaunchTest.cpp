#include <catch2/catch_test_macros.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/Launch.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace
{
// <root>/Binaries/<SERVITOR_OUTPUTDIR>/<Host>/<Host>[.exe]: where HostExePath
// looks for a source-checkout engine root (no sdk/sdk.json; rev 8.3 V7).
std::filesystem::path FakeHostExe(const std::filesystem::path& root, const char* host)
{
    std::filesystem::path exe = root / "Binaries" / SERVITOR_OUTPUTDIR / host / host;
#if defined(_WIN32)
    exe += ".exe";
#endif
    return exe;
}

std::filesystem::path MakeProject(const std::filesystem::path& dir)
{
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "Fake.arcproj") << "{}";
    return dir;
}

bool Contains(const std::vector<std::string>& argv, const std::string& token)
{
    for (const auto& a : argv)
        if (a == token)
            return true;
    return false;
}
} // namespace

TEST_CASE("launch with an unresolvable --engine spawns nothing", "[servitor][client]") {
    Arcane::Test::TempDir root("servitor-launch-empty");
    Servitor::GlobalOptions opt;
    opt.engine = root.path.string();
    Arcane::Remote::Envelope err;
    CHECK_FALSE(Servitor::HostExePath(Arcane::Remote::HostKind::Editor, opt, &err).has_value());
    REQUIRE_FALSE(err.errors.empty());
    CHECK(err.errors[0].code == "engine.unresolved");
    CHECK(err.errors[0].message.find(Servitor::PathUtf8(root.path)) != std::string::npos);
    Arcane::Remote::Envelope out;
    CHECK(Servitor::Launch(Arcane::Remote::HostKind::Editor, opt, {}, &out) == 4);
    REQUIRE_FALSE(out.errors.empty());
    CHECK(out.errors[0].code == "engine.unresolved");
    CHECK(Arcane::Remote::ExitFromHost(out) == 4);
}

TEST_CASE("launch plans --project and --remote and passes the host flags through verbatim", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-plan");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneRuntime");
    std::filesystem::create_directories(exe.parent_path());
    std::ofstream(exe) << "not a program";   // PlanLaunch checks the path; it spawns nothing
    const auto project = MakeProject(tmp.path / "Game");

    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = project.string();
    const std::vector<std::string> hostArgs = {"--headless", "--frames", "0", "--note", "a b \"c\""};
    Arcane::Remote::Envelope err;
    const auto plan = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, hostArgs, &err);
    REQUIRE(plan.has_value());
    CHECK(plan->exe == exe);
    CHECK(plan->log == exe.parent_path() / "ArcaneRuntime.servitor-launch.log");
    CHECK(plan->descriptorDir == Arcane::Remote::DescriptorDir(std::filesystem::absolute(project), false));
    CHECK(plan->descriptor.empty());   // <descriptorDir>/runtime-<pid>.json once the pid is known
    CHECK(plan->hideConsole);          // --headless
    const std::vector<std::string> expected = {"--project", Servitor::PathUtf8(std::filesystem::absolute(project)),
                                               "--headless", "--frames", "0", "--note", "a b \"c\"", "--remote"};
    CHECK(plan->argv == expected);

    // --remote-descriptor among the host flags names the exact descriptor to
    // wait for (made absolute: the host runs in its own folder); --remote is
    // not added twice.
    const std::vector<std::string> withDescriptor = {"--remote", "--remote-descriptor", "d/runtime.json"};
    const auto named = Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, withDescriptor, &err);
    REQUIRE(named.has_value());
    CHECK(named->descriptor == std::filesystem::absolute("d/runtime.json"));
    CHECK_FALSE(named->hideConsole);   // a windowed runtime keeps its console flags
    const std::vector<std::string> expectedNamed = {"--project", Servitor::PathUtf8(std::filesystem::absolute(project)),
                                                    "--remote", "--remote-descriptor",
                                                    Servitor::PathUtf8(std::filesystem::absolute("d/runtime.json"))};
    CHECK(named->argv == expectedNamed);

    // --project both before and after `--` is ambiguous: refused, nothing spawned.
    const std::vector<std::string> twice = {"--project", project.string()};
    Arcane::Remote::Envelope refused;
    CHECK_FALSE(Servitor::PlanLaunch(Arcane::Remote::HostKind::Runtime, opt, twice, &refused).has_value());
    CHECK(refused.errors[0].code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 2);

    // A missing exe under a resolved root is engine.unresolved naming the path tried.
    Arcane::Remote::Envelope missing;
    CHECK_FALSE(Servitor::HostExePath(Arcane::Remote::HostKind::Server, opt, &missing).has_value());
    CHECK(missing.errors[0].code == "engine.unresolved");
    CHECK(missing.errors[0].message.find("ArcaneServer") != std::string::npos);
}

TEST_CASE("a launched host that exits before it is ready is host.died exit 7, and its log stays", "[servitor][client]") {
    Arcane::Test::TempDir tmp("servitor-launch-dies");
    const auto exe = FakeHostExe(tmp.path / "engine", "ArcaneServer");
    std::filesystem::create_directories(exe.parent_path());
    // A real program that exits at once whatever its arguments: cmd.exe with
    // the null device as stdin (Windows), false (POSIX).
#if defined(_WIN32)
    const char* systemRoot = std::getenv("SystemRoot");
    REQUIRE(systemRoot != nullptr);
    std::filesystem::copy_file(std::filesystem::path(systemRoot) / "System32" / "cmd.exe", exe);
#else
    const std::filesystem::path falseExe =
        std::filesystem::exists("/bin/false") ? std::filesystem::path("/bin/false") : std::filesystem::path("/usr/bin/false");
    std::filesystem::copy_file(falseExe, exe);
    std::filesystem::permissions(exe, std::filesystem::perms::owner_all);
#endif
    Servitor::GlobalOptions opt;
    opt.engine = (tmp.path / "engine").string();
    opt.project = MakeProject(tmp.path / "Game").string();
    opt.timeoutMs = 20000;
    Arcane::Remote::Envelope out;
    const auto started = std::chrono::steady_clock::now();
    CHECK(Servitor::Launch(Arcane::Remote::HostKind::Server, opt, {}, &out) == 7);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(15));   // noticed, not timed out
    REQUIRE_FALSE(out.errors.empty());
    CHECK(out.errors[0].code == "host.died");
    CHECK(out.command == "launch");
    const auto log = exe.parent_path() / "ArcaneServer.servitor-launch.log";
    CHECK(std::filesystem::exists(log));
    CHECK(out.errors[0].detail["log"] == Servitor::PathUtf8(log));
    CHECK(out.errors[0].detail["pid"].get<std::uint32_t>() != 0);
}

TEST_CASE("verb rows declare their flags; unknown ones are refused and launch passes -- through", "[servitor][client]") {
    const Servitor::VerbRow* launch = Servitor::FindVerb("launch");
    const Servitor::VerbRow* quit = Servitor::FindVerb("quit");
    const Servitor::VerbRow* wait = Servitor::FindVerb("wait");
    const Servitor::VerbRow* status = Servitor::FindVerb("status");
    REQUIRE(launch != nullptr);
    REQUIRE(quit != nullptr);
    REQUIRE(wait != nullptr);
    REQUIRE(status != nullptr);

    const std::vector<std::string> launchRest = {"runtime", "--headless", "--backend", "vulkan", "--",
                                                 "--frames", "3", "--json", "--bogus"};
    Servitor::VerbTokens tokens;
    CHECK_FALSE(Servitor::SplitVerbTokens(*launch, launchRest, &tokens).has_value());
    CHECK(tokens.positional == std::vector<std::string>{"runtime"});
    CHECK(tokens.passThrough == std::vector<std::string>{"--frames", "3", "--json", "--bogus"});
    nlohmann::json args;
    std::string error;
    REQUIRE(Servitor::LaunchArgs(launchRest, args, &error));
    CHECK(args["kind"] == "runtime");
    CHECK(args["hostArgs"] == nlohmann::json::array({"--headless", "--backend", "vulkan", "--frames", "3", "--json", "--bogus"}));
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"--headless"}, args, &error));      // no kind
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"client"}, args, &error));          // not a host kind
    CHECK_FALSE(Servitor::LaunchArgs(std::vector<std::string>{"runtime", "--backend"}, args, &error));   // a value flag needs its value

    const std::vector<std::string> unknownBeforeSeparator = {"runtime", "--bogus"};
    const auto problem = Servitor::SplitVerbTokens(*launch, unknownBeforeSeparator, nullptr);
    REQUIRE(problem.has_value());
    CHECK(problem->find("--bogus") != std::string::npos);
    // A bare `--` is only a separator for a row that passes tokens through.
    CHECK(Servitor::SplitVerbTokens(*status, std::vector<std::string>{"--"}, nullptr).has_value());
    CHECK(Servitor::SplitVerbTokens(*status, std::vector<std::string>{"--bogus"}, nullptr)->find("unknown flag --bogus") !=
          std::string::npos);

    REQUIRE(quit->args(std::vector<std::string>{"--force"}, args, &error));
    CHECK(args == nlohmann::json{{"force", true}, {"discardChanges", false}});
    REQUIRE(quit->args(std::vector<std::string>{"--discard-changes"}, args, &error));
    CHECK(args == nlohmann::json{{"force", false}, {"discardChanges", true}});
    CHECK(Servitor::SplitVerbTokens(*quit, std::vector<std::string>{"--report", "r.json"}, nullptr).has_value());
    CHECK_FALSE(quit->args(std::vector<std::string>{"now"}, args, &error));

    REQUIRE(wait->args(std::vector<std::string>{"frames", "10"}, args, &error));
    CHECK(args == nlohmann::json{{"what", "frames"}, {"frames", 10}});
    REQUIRE(wait->args(std::vector<std::string>{"state", "playing"}, args, &error));
    CHECK(args == nlohmann::json{{"what", "state"}, {"state", "playing"}});
    for (const char* what : {"ready", "settled", "reload"}) {
        REQUIRE(wait->args(std::vector<std::string>{what}, args, &error));
        CHECK(args == nlohmann::json{{"what", what}});
    }
    CHECK_FALSE(wait->args(std::vector<std::string>{}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames", "x"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"frames", "-1"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"state"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"soon"}, args, &error));
    CHECK_FALSE(wait->args(std::vector<std::string>{"ready", "now"}, args, &error));
    CHECK(wait->awaitsHost);   // wait ready on a host that is still booting finds it within --timeout
}

TEST_CASE("global flags after a bare -- belong to the host, not to servitor", "[servitor][client]") {
    std::vector<std::string> words = {"servitor", "--json", "launch", "runtime", "--", "--project", "X", "--timeout", "9"};
    std::vector<char*> argv;
    for (auto& w : words)
        argv.push_back(w.data());
    Servitor::GlobalOptions opt;
    std::string error;
    REQUIRE(Servitor::ParseGlobals(static_cast<int>(argv.size()), argv.data(), opt, &error));
    CHECK(opt.json);
    CHECK(opt.project.empty());
    CHECK(opt.timeoutMs == 30000);
}

#endif
