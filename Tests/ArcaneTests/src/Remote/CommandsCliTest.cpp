#include <catch2/catch_test_macros.hpp>
#include <Servitor/Commands.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Arcane/Project/Project.hpp> // EditorLock::Self (the default --client names this pid)
#include <algorithm>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Commands/CatalogCommands.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Json.hpp>
#include "RemoteDemoArgs.hpp"
#include "RemoteTestSupport.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("servitor commands --format json needs no host", "[servitor][client]") {
    const auto manifest = Servitor::StaticManifest();
    REQUIRE(manifest.is_object());
    CHECK(manifest.at("version") == std::string(Servitor::kVersion));
    CHECK(manifest.at("protocolMajors") == nlohmann::json::array({1}));
    bool launcherNine = false;
    for (const auto& row : manifest.at("exitCodes"))
        if (row.at("code") == 9)
            launcherNine = row.at("launcherOnly").get<bool>()
                && row.at("meaning").get<std::string>().find("package.not-installed") != std::string::npos;
    CHECK(launcherNine);
    bool saw = false;
    for (const auto& row : manifest.at("verbs")) {
        if (row["name"] == "commands")
            saw = true;
        CHECK_FALSE(row["summary"].get<std::string>().empty());
    }
    CHECK(saw);
    const char* argv[] = {"servitor", "commands", "--format", "json"};
    Servitor::GlobalOptions opt;
    std::string err;
    REQUIRE(Servitor::ParseGlobals(4, const_cast<char**>(argv), opt, &err));
    CHECK(opt.format == Servitor::Format::Json);
    // The unit test never calls PrintStaticManifest: stdout is Catch2's
    // reporter channel (ruling F31). RenderStaticManifest is the same text.
    const std::string text = Servitor::RenderStaticManifest(opt);
    CHECK(text.find("\"name\"") != std::string::npos);
    CHECK(text.find("\"entity\"") != std::string::npos);
    CHECK(nlohmann::json::parse(text).is_object());
}

TEST_CASE("catalog.describe and schema.type answer through Invoke", "[servitor][client]") {
    Arcane::Remote::RegisterCatalogCommands();
    Arcane::Remote::RegisterCatalogCommands();   // a second call is a no-op
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    Arcane::Remote::Call describe;
    describe.command = "catalog.describe";
    describe.args = nlohmann::json{{"name", "catalog.describe"}};
    const auto described = reg.Invoke(describe);
    REQUIRE(described.envelope.ok);
    CHECK(described.envelope.data["name"] == "catalog.describe");
    CHECK(described.envelope.data["access"] == "observe");
    CHECK(described.envelope.data["schema"]["properties"].contains("name"));
    describe.args = nlohmann::json{{"name", "no.such"}};
    const auto missing = reg.Invoke(describe);
    CHECK(missing.envelope.errors[0].code == "command.unknown");
    CHECK(Arcane::Remote::ExitFromHost(missing.envelope) == 2);
    Arcane::Remote::Call schema;
    schema.command = "schema.type";
    schema.args = nlohmann::json{{"kind", "component"}, {"name", "RemoteDemoArgs"}};
    const auto typed = reg.Invoke(schema);
    REQUIRE(typed.envelope.ok);
    CHECK(typed.envelope.data["properties"].contains("position"));
    schema.args = nlohmann::json{{"kind", "component"}, {"name", "NoSuchType"}};
    const auto unknown = reg.Invoke(schema);
    CHECK(unknown.envelope.errors[0].code == "component.unknown");
    CHECK(Arcane::Remote::ExitFromHost(unknown.envelope) == 6);
}

TEST_CASE("the static manifest lists every exit code and every table verb", "[servitor][client]") {
    const auto manifest = Servitor::StaticManifest();
    std::vector<int> codes;
    for (const auto& row : manifest.at("exitCodes")) {
        codes.push_back(row.at("code").get<int>());
        CHECK_FALSE(row.at("meaning").get<std::string>().empty());
        CHECK(row.at("launcherOnly").get<bool>() == (row.at("code") == 9));
    }
    CHECK(codes == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 130, 143});
    for (const auto& row : manifest.at("exitCodes"))
        if (row.at("code") == 4)
            CHECK(row.at("meaning").get<std::string>().find("engine.unresolved") != std::string::npos);

    std::set<std::string> names;
    for (const auto& row : manifest.at("verbs")) {
        CHECK(names.insert(row.at("name").get<std::string>()).second);   // one row per verb
        const std::string access = row.at("access").get<std::string>();
        CHECK((access == "observe" || access == "drive" || access == "per-command" || access == "none"));
    }
    CHECK(names.size() == 27);
    for (const auto& row : manifest.at("verbs"))
        if (row.at("name") == "compare")
            CHECK(row.at("access") == "per-command");   // --bless drives
    // Drift guard: every verb the table dispatches is in the manifest.
    for (const Servitor::VerbRow& row : Servitor::VerbTable()) {
        const std::string words = row.words;
        INFO(words);
        CHECK(names.contains(words.substr(0, words.find(' '))));
    }

    Servitor::GlobalOptions human;
    human.format = Servitor::Format::Human;
    const std::string text = Servitor::RenderStaticManifest(human);
    CHECK(text.find("launcher only") != std::string::npos);
    CHECK(text.find("schema") != std::string::npos);
    CHECK_FALSE(text.starts_with("{"));
}

TEST_CASE("catalog.describe and schema.type refuse bad args", "[servitor][client]") {
    Arcane::Remote::RegisterCatalogCommands();
    auto& reg = Arcane::Remote::CommandRegistry::Get();
    Arcane::Remote::Call call;
    call.command = "catalog.describe";
    call.args = nlohmann::json{{"name", "catalog.describe"}, {"extra", 1}};
    CHECK(reg.Invoke(call).envelope.errors.at(0).code == "arg.unknown");
    call.args = nlohmann::json::object();
    CHECK(reg.Invoke(call).envelope.errors.at(0).code == "arg.missing");
    call.args = nlohmann::json{{"name", 3}};
    CHECK(reg.Invoke(call).envelope.errors.at(0).code == "arg.invalid");

    call.command = "schema.type";
    call.args = nlohmann::json{{"kind", "system"}, {"name", "RemoteDemoArgs"}};
    const auto badKind = reg.Invoke(call);
    CHECK(badKind.envelope.errors.at(0).code == "arg.invalid");
    CHECK(Arcane::Remote::ExitFromHost(badKind.envelope) == 2);
    call.args = nlohmann::json{{"kind", "resource"}, {"name", "NoSuchType"}};
    const auto noResource = reg.Invoke(call);
    CHECK(noResource.envelope.errors.at(0).code == "resource.not-found");
    CHECK(Arcane::Remote::ExitFromHost(noResource.envelope) == 6);
    call.args = nlohmann::json{{"kind", "resource"}, {"name", "RemoteDemoArgs"}};
    const auto resource = reg.Invoke(call);
    REQUIRE(resource.envelope.ok);
    CHECK(resource.envelope.data["properties"].contains("n"));
    // catalog.describe reports schema.type's declaration too.
    call.command = "catalog.describe";
    call.args = nlohmann::json{{"name", "schema.type"}};
    const auto described = reg.Invoke(call);
    REQUIRE(described.envelope.ok);
    CHECK(described.envelope.data["thread"] == "main");
    CHECK(described.envelope.data["needsFrame"] == false);
    CHECK(described.envelope.data["schema"]["properties"].contains("kind"));
}

namespace
{
std::vector<std::string> Tokens(std::initializer_list<const char*> list)
{
    return {list.begin(), list.end()};
}

struct Captured {
    std::string body;
};

Servitor::ExchangeResult CaptureExchange(const std::string& body, void* user)
{
    static_cast<Captured*>(user)->body = body;
    Servitor::ExchangeResult r;
    r.envelope = Arcane::Remote::MakeOk("x", nlohmann::json::object(), {});
    return r;
}
} // namespace

TEST_CASE("servitor call parses its command and args", "[servitor][client]") {
    Servitor::CallRequest req;
    std::string error;
    auto t = Tokens({"host.wait", "what=frames", "frames=3"});
    REQUIRE(Servitor::ParseCallTokens(t, req, &error));
    CHECK(req.command == "host.wait");
    CHECK(req.args == nlohmann::json{{"what", "frames"}, {"frames", 3}});

    t = Tokens({"entity.get", "--args-json", R"({"entity":"name:MeshCube","components":["Transform"]})", "world=client"});
    REQUIRE(Servitor::ParseCallTokens(t, req, &error));
    CHECK(req.args["entity"] == "name:MeshCube");
    CHECK(req.args["components"] == nlohmann::json::array({"Transform"}));
    CHECK(req.args["world"] == "client");

    Arcane::Test::TempDir dir("servitor-call-args");
    const auto file = dir.path / "args.json";
    std::ofstream(file) << R"({"what":"ready"})";
    t = {"host.wait", "--args-file", file.string()};
    REQUIRE(Servitor::ParseCallTokens(t, req, &error));
    CHECK(req.args == nlohmann::json{{"what", "ready"}});

    // Refusals (usage, exit 2), nothing sent.
    const std::vector<std::vector<std::string>> bad = {
        {},                                                     // no command
        {"--args-json", "{}"},                                  // still no command
        {"a.b", "--args-json", "[1]"},                          // not an object
        {"a.b", "--args-json", "{"},                            // not JSON
        {"a.b", "--args-json", "{}", "--args-file", file.string()}, // both
        {"a.b", "--args-file", (dir.path / "missing.json").string()},
        {"a.b", "x=1", "x=2"},                                  // a key twice
        {"a.b", "--args-json", R"({"x":1})", "x=2"},            // a key twice across sources
        {"a.b", "loose"},                                       // not name=value
        {"a.b", "=1"},                                          // empty name
        {"a.b", "--world", "client"},                           // unknown flag
    };
    for (const auto& tokens : bad) {
        INFO((tokens.empty() ? std::string("<none>") : tokens.back()));
        Servitor::CallRequest out;
        error.clear();
        CHECK_FALSE(Servitor::ParseCallTokens(tokens, out, &error));
        CHECK_FALSE(error.empty());
    }

    // Run's flag check (SplitVerbTokens over the call row) refuses an unknown
    // flag before anything is sent, and accepts the two declared ones.
    const Servitor::VerbRow* row = Servitor::FindVerb("call");
    REQUIRE(row != nullptr);
    const auto bogus = Tokens({"a.b", "--bogus"});
    const auto problem = Servitor::SplitVerbTokens(*row, bogus, nullptr);
    REQUIRE(problem.has_value());
    CHECK(problem->find("--bogus") != std::string::npos);
    const auto declared = Tokens({"a.b", "--args-json", "{}"});
    CHECK_FALSE(Servitor::SplitVerbTokens(*row, declared, nullptr).has_value());
}

TEST_CASE("servitor call posts the command, its args and the read-only option", "[servitor][client]") {
    Servitor::CallRequest req;
    std::string error;
    auto t = Tokens({"host.quit", "force=true"});
    REQUIRE(Servitor::ParseCallTokens(t, req, &error));
    Servitor::GlobalOptions opt;
    opt.readOnly = true;   // the host's gate refuses a Drive command (permission.read-only)
    Captured captured;
    Arcane::Remote::ClientBlock client;
    (void)Servitor::Call(req, opt, &CaptureExchange, &captured, &client);
    const auto body = nlohmann::json::parse(captured.body);
    CHECK(body["command"] == "host.quit");
    CHECK(body["args"] == nlohmann::json{{"force", true}});
    CHECK(body["options"]["readOnly"] == true);
}

namespace
{
Servitor::ExchangeResult DescribeMissExchange(const std::string& /*body*/, void* /*user*/)
{
    // What catalog.describe answers for a name the host does not register.
    Arcane::Remote::RegisterCatalogCommands();
    Arcane::Remote::Call call;
    call.command = "catalog.describe";
    call.args = nlohmann::json{{"name", "no.such"}};
    Servitor::ExchangeResult r;
    r.envelope = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    return r;
}
} // namespace

TEST_CASE("describe of an unknown command names that command, not catalog.describe", "[servitor][client]") {
    Servitor::CallRequest req;
    req.command = "catalog.describe";
    req.args = nlohmann::json{{"name", "no.such"}};
    Servitor::GlobalOptions opt;
    Arcane::Remote::ClientBlock client;
    Arcane::Remote::Envelope answered;
    (void)Servitor::Call(req, opt, &DescribeMissExchange, nullptr, &client, &answered);
    CHECK(client.exitCode == 2);
    REQUIRE_FALSE(answered.errors.empty());
    const std::string& remediation = answered.errors.front().remediation;
    CHECK(remediation.find("has no command no.such") != std::string::npos);
    CHECK(remediation.find("has no command catalog.describe") == std::string::npos);
}

TEST_CASE("describe and schema build their wire args", "[servitor][client]") {
    const Servitor::VerbRow* describe = Servitor::FindVerb("describe");
    const Servitor::VerbRow* schema = Servitor::FindVerb("schema");
    REQUIRE(describe != nullptr);
    REQUIRE(schema != nullptr);
    CHECK(std::string(describe->command) == "catalog.describe");
    CHECK(std::string(schema->command) == "schema.type");
    nlohmann::json out;
    std::string error;
    auto t = Tokens({"entity.get"});
    REQUIRE(describe->args(t, out, &error));
    CHECK(out == nlohmann::json{{"name", "entity.get"}});
    t = Tokens({"component", "Transform"});
    REQUIRE(schema->args(t, out, &error));
    CHECK(out == nlohmann::json{{"kind", "component"}, {"name", "Transform"}});
    for (const auto& bad : std::vector<std::vector<std::string>>{{}, {"a", "b"}})
        CHECK_FALSE(describe->args(bad, out, &error));
    for (const auto& bad : std::vector<std::vector<std::string>>{{}, {"component"}, {"system", "X"}, {"resource", "A", "B"}})
        CHECK_FALSE(schema->args(bad, out, &error));
    REQUIRE(Servitor::FindVerb("commands") != nullptr);
    REQUIRE(Servitor::FindVerb("list") != nullptr);
    REQUIRE(Servitor::FindVerb("call") != nullptr);
}

namespace
{
struct FakeCatalog {
    std::optional<nlohmann::json> reply;
    Arcane::Remote::Envelope error;
    std::uint32_t seenTimeoutMs = 0;
    std::uint16_t seenPort = 0;
};

std::optional<nlohmann::json> FakeFetch(const Arcane::Remote::Descriptor& desc, std::uint32_t timeoutMs,
                                        Arcane::Remote::Envelope* error, void* user)
{
    auto& fake = *static_cast<FakeCatalog*>(user);
    fake.seenTimeoutMs = timeoutMs;
    fake.seenPort = desc.port;
    if (!fake.reply && error != nullptr)
        *error = fake.error;
    return fake.reply;
}

Servitor::LiveHost FakeHost()
{
    Servitor::LiveHost host;
    host.descriptor.hostKind = Arcane::Remote::HostKind::Runtime;
    host.descriptor.pid = 4242;
    host.descriptor.port = 50123;
    host.descriptor.state = "ready";
    host.descriptor.frame = 9;
    host.descriptor.token = std::string(64, 'f');
    host.path = std::filesystem::path("Saved") / "Remote" / "runtime-4242.json";
    return host;
}
} // namespace

TEST_CASE("servitor list renders the host catalogue, a failed fetch and a malformed one", "[servitor][client]") {
    const Servitor::LiveHost host = FakeHost();
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.timeoutMs = 1234;

    // Happy path: MakeOk("list", catalogue), host.catalog = generation.
    FakeCatalog fake;
    fake.reply = nlohmann::json{
        {"generation", 7},
        {"commands", nlohmann::json::array({{{"name", "a.b"}, {"access", "observe"}, {"summary", "does a"}},
                                            {{"name", "host.quit"}, {"access", "drive"}, {"summary", "quits"}}})}};
    Arcane::Remote::ClientBlock client;
    Arcane::Remote::Envelope env;
    auto printed = Servitor::ListCatalogue(host, opt, &FakeFetch, &fake, &client, &env);
    CHECK(fake.seenTimeoutMs == 1234);
    CHECK(fake.seenPort == 50123);
    CHECK(client.exitCode == 0);
    CHECK(env.ok);
    CHECK(env.command == "list");
    CHECK(env.host.kind == "runtime");
    CHECK(env.host.pid == 4242u);
    CHECK(env.host.catalog == 7u);
    CHECK(env.data["commands"].size() == 2);
    const auto json = nlohmann::json::parse(printed.stdoutText);
    CHECK(json["client"]["exitCode"] == 0);
    CHECK(json["client"]["descriptor"] == "Saved/Remote/runtime-4242.json");
    CHECK(json["data"]["commands"][1]["name"] == "host.quit");
    CHECK(printed.stdoutText.find(host.descriptor.token) == std::string::npos);

    Servitor::GlobalOptions human = opt;
    human.format = Servitor::Format::Human;
    printed = Servitor::ListCatalogue(host, human, &FakeFetch, &fake, &client);
    CHECK(client.exitCode == 0);
    CHECK(printed.stdoutText.starts_with("ok list\ngeneration: 7\n"));
    CHECK(printed.stdoutText.find("host.quit") != std::string::npos);
    CHECK(printed.stdoutText.find("drive") != std::string::npos);
    CHECK(printed.stderrText.empty());

    // The GET failed: its envelope, as command "list", with the selected host.
    FakeCatalog down;
    down.error = Arcane::Remote::MakeError("", "host.unreachable", "could not connect", "servitor status");
    printed = Servitor::ListCatalogue(host, opt, &FakeFetch, &down, &client, &env);
    CHECK(client.exitCode == 7);
    CHECK_FALSE(env.ok);
    CHECK(env.command == "list");
    CHECK(env.errors.at(0).code == "host.unreachable");
    CHECK(env.host.pid == 4242u);
    CHECK(nlohmann::json::parse(printed.stdoutText)["client"]["exitCode"] == 7);
    printed = Servitor::ListCatalogue(host, human, &FakeFetch, &down, &client);
    CHECK(printed.stdoutText.starts_with("error host.unreachable"));
    CHECK(printed.stderrText.find("could not connect") != std::string::npos);

    // A fetch that failed without an envelope is still an error, never ok.
    FakeCatalog silent;
    (void)Servitor::ListCatalogue(host, opt, &FakeFetch, &silent, &client, &env);
    CHECK(client.exitCode == 1);
    CHECK(env.errors.at(0).code == "internal");

    // Malformed catalogues: internal, exit 1.
    for (const nlohmann::json& bad : {nlohmann::json::object(), nlohmann::json{{"commands", "x"}},
                                      nlohmann::json::array(), nlohmann::json("text")}) {
        INFO(bad.dump());
        FakeCatalog broken;
        broken.reply = bad;
        (void)Servitor::ListCatalogue(host, human, &FakeFetch, &broken, &client, &env);
        CHECK(client.exitCode == 1);
        CHECK_FALSE(env.ok);
        CHECK(env.command == "list");
        CHECK(env.errors.at(0).code == "internal");
        CHECK(env.host.pid == 4242u);
    }
    // Rows that are not objects do not throw in the human table.
    FakeCatalog odd;
    odd.reply = nlohmann::json{{"generation", 1}, {"commands", nlohmann::json::array({1, nullptr, {{"name", 3}}})}};
    printed = Servitor::ListCatalogue(host, human, &FakeFetch, &odd, &client);
    CHECK(client.exitCode == 0);
}

TEST_CASE("servitor list reads a live host's catalogue over the wire", "[servitor][client]") {
    RemoteTest::ScopedHostKind kindGuard(Arcane::Remote::HostKind::Editor);
    Arcane::Test::TempDir tmp("servitor-list-live");
    Arcane::Remote::HostSession::Config cfg;
    cfg.kind = Arcane::Remote::HostKind::Runtime;
    cfg.projectRoot = tmp.path;
    Arcane::Remote::HostSession session(cfg);
    REQUIRE(session.Start());
    const auto desc = Arcane::Remote::ReadDescriptor(session.DescriptorFile());
    REQUIRE(desc);
    Servitor::LiveHost host;
    host.descriptor = *desc;
    host.path = session.DescriptorFile();
    Servitor::GlobalOptions opt;
    opt.format = Servitor::Format::Json;
    opt.timeoutMs = 5000;
    Arcane::Remote::ClientBlock client;
    Arcane::Remote::Envelope env;
    const auto printed = Servitor::ListCatalogue(host, opt, &Servitor::LiveCatalog, nullptr, &client, &env);
    CHECK(client.exitCode == 0);
    REQUIRE(env.ok);
    std::set<std::string> names;
    for (const auto& row : env.data.at("commands"))
        names.insert(row.at("name").get<std::string>());
    CHECK(names.contains("catalog.describe"));   // HostSession::Start registers both (ruling F13)
    CHECK(names.contains("schema.type"));
    CHECK(names.contains("host.quit"));
    CHECK(env.host.kind == "runtime");
    CHECK(env.host.catalog == Arcane::Remote::CommandRegistry::Get().Generation());
    CHECK(printed.stdoutText.find(desc->token) == std::string::npos);
    session.Stop("test");
}
// ---- Final fix pass ----------------------------------------------------------

namespace {

// Parses `argv` (argv[0] is "servitor") with ParseGlobals.
bool ParseFlags(std::vector<std::string> argv, Servitor::GlobalOptions& opt, std::string& error)
{
    std::vector<char*> raw;
    for (std::string& a : argv)
        raw.push_back(a.data());
    return Servitor::ParseGlobals(static_cast<int>(raw.size()), raw.data(), opt, &error);
}

} // namespace

// C-I1: every call names its caller (spec s4.5/s4.6): --client <name>, else
// "servitor/<version> pid <pid>". A name the host would refuse is refused here.
TEST_CASE("--client names the caller, the default names servitor and its pid, and a bad name is refused",
          "[servitor][client]") {
    Servitor::GlobalOptions opt;
    std::string error;
    REQUIRE(ParseFlags({"servitor", "--client", "agent-7 (ci)", "status"}, opt, error));
    CHECK(opt.client == "agent-7 (ci)");
    REQUIRE(ParseFlags({"servitor", "status"}, opt, error));
    const std::string expected =
        "servitor/" + std::string(Servitor::kVersion) + " pid " + std::to_string(Arcane::EditorLock::Self().pid);
    CHECK(Servitor::DefaultClientId() == expected);
    CHECK(opt.client == expected);
    CHECK(Arcane::Remote::IsValidClientId(opt.client));
    CHECK_FALSE(ParseFlags({"servitor", "--client", "caf\xc3\xa9", "status"}, opt, error));
    CHECK(error.find("--client") != std::string::npos);
    CHECK_FALSE(ParseFlags({"servitor", "--client", std::string(Arcane::Remote::kMaxClientIdBytes + 1, 'a'), "status"},
                           opt, error));
    CHECK_FALSE(ParseFlags({"servitor", "status", "--client"}, opt, error));
    CHECK(Servitor::UsageText("x").find("--client <name>") != std::string::npos);
}

// C-M9: --verbose logs the whole request, and the help says so.
TEST_CASE("the usage text says --verbose prints the request payload, values included", "[servitor][client]") {
    const std::string text = Servitor::UsageText("x");
    CHECK(text.find("--verbose prints each request's payload, including the values you pass") != std::string::npos);
}

namespace {

// A scripted exchange: answers `codes` in order (the last repeats); "" is ok.
struct Scripted {
    std::vector<std::string> codes;
    int calls = 0;
    int rediscovered = 0;
    bool rediscoverFinds = false;
};

Servitor::ExchangeResult ScriptedExchange(const std::string&, void* user)
{
    auto* s = static_cast<Scripted*>(user);
    const std::size_t at = std::min<std::size_t>(static_cast<std::size_t>(s->calls), s->codes.size() - 1);
    const std::string& code = s->codes[at];
    ++s->calls;
    Servitor::ExchangeResult r;
    r.envelope = code.empty() ? Arcane::Remote::MakeOk("play.step", nlohmann::json{{"done", true}}, {})
                              : Arcane::Remote::MakeError("play.step", code, "m", "r");
    return r;
}

bool ScriptedRediscover(void* user, const Arcane::Remote::Envelope&)
{
    auto* s = static_cast<Scripted*>(user);
    ++s->rediscovered;
    return s->rediscoverFinds;
}

} // namespace

// C-I2 (d) / C-M6: AM6's "rediscover first, then retry against the new pid".
// A live host (host.unreachable: it closed the connection; call.timeout: it is
// still running the command) is never re-posted, and a dead or hung one only
// when rediscovery found a different process.
TEST_CASE("--retry-on-death re-posts only after a death or hang that rediscovery replaced", "[servitor][client]") {
    Servitor::CallRequest req;
    req.command = "play.step";
    req.args = {{"n", 100000}};
    Servitor::GlobalOptions opt;
    opt.retryOnDeath = true;
    opt.timeoutMs = 400;
    Arcane::Remote::ClientBlock client;
    {
        Scripted live{{"host.unreachable"}};
        (void)Servitor::Call(req, opt, &ScriptedExchange, &live, &client, nullptr, &ScriptedRediscover);
        CHECK(live.calls == 1);
        CHECK(live.rediscovered == 0);
        CHECK(client.exitCode == 7);
    }
    {
        Scripted running{{"call.timeout"}};
        (void)Servitor::Call(req, opt, &ScriptedExchange, &running, &client, nullptr, &ScriptedRediscover);
        CHECK(running.calls == 1);
        CHECK(client.exitCode == 8);
    }
    {
        Scripted gone{{"host.died"}};   // nothing new came up
        (void)Servitor::Call(req, opt, &ScriptedExchange, &gone, &client, nullptr, &ScriptedRediscover);
        CHECK(gone.calls == 1);
        CHECK(gone.rediscovered >= 1);
        CHECK(client.exitCode == 7);
    }
    {
        Scripted replaced{{"host.died", ""}};
        replaced.rediscoverFinds = true;
        (void)Servitor::Call(req, opt, &ScriptedExchange, &replaced, &client, nullptr, &ScriptedRediscover);
        CHECK(replaced.calls == 2);
        CHECK(client.exitCode == 0);
        CHECK(client.retried);
    }
    {
        Scripted hung{{"host.hung"}};
        (void)Servitor::Call(req, opt, &ScriptedExchange, &hung, &client, nullptr, nullptr);   // no rediscovery
        CHECK(hung.calls == 1);
    }
    {
        Scripted off{{"host.died", ""}};
        off.rediscoverFinds = true;
        Servitor::GlobalOptions noFlag = opt;
        noFlag.retryOnDeath = false;
        (void)Servitor::Call(req, noFlag, &ScriptedExchange, &off, &client, nullptr, &ScriptedRediscover);
        CHECK(off.calls == 1);
        CHECK(off.rediscovered == 0);
    }
}

// C-I4: Rule 3 in the client. launch and commands select no host, so --host
// and --pid would be dropped: refused. status lists hosts, so it honours them
// as a filter. --yes confirms a Drive command fanned out by --all; alone it
// confirms nothing and is refused.
TEST_CASE("launch and commands refuse --host and --pid, status filters by them, and --yes needs --all",
          "[servitor][client]") {
    Servitor::GlobalOptions host;
    host.hostKind = "editor";
    Servitor::GlobalOptions pid;
    pid.pid = 4242;
    for (const char* words : {"launch", "commands"}) {
        INFO(words);
        const Servitor::VerbRow* row = Servitor::FindVerb(words);
        REQUIRE(row != nullptr);
        const auto h = Servitor::UnusedGlobal(*row, host);
        REQUIRE(h.has_value());
        CHECK(*h == "--host does not apply to " + std::string(words));
        const auto p = Servitor::UnusedGlobal(*row, pid);
        REQUIRE(p.has_value());
        CHECK(*p == "--pid does not apply to " + std::string(words));
    }
    for (const char* words : {"status", "list", "cvar get", "call", "wait"}) {
        INFO(words);
        CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb(words), host).has_value());
        CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb(words), pid).has_value());
    }
    Servitor::GlobalOptions yes;
    yes.yes = true;
    for (const Servitor::VerbRow& row : Servitor::VerbTable()) {
        INFO(row.words);
        const auto problem = Servitor::UnusedGlobal(row, yes);
        REQUIRE(problem.has_value());
        CHECK(problem->find("--yes") != std::string::npos);
        CHECK(problem->find("--all") != std::string::npos);
    }
    yes.all = true;
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("cvar set"), yes).has_value());

    // status: --host / --pid filter the hosts it lists.
    Servitor::Scan scan;
    Servitor::LiveHost editor;
    editor.descriptor.hostKind = Arcane::Remote::HostKind::Editor;
    editor.descriptor.pid = 11;
    Servitor::LiveHost runtime;
    runtime.descriptor.hostKind = Arcane::Remote::HostKind::Runtime;
    runtime.descriptor.pid = 22;
    scan.live = {editor, runtime};
    CHECK(Servitor::StatusData(scan)["hosts"].size() == 2);
    const auto onlyEditor = Servitor::StatusData(scan, host)["hosts"];
    REQUIRE(onlyEditor.size() == 1);
    CHECK(onlyEditor[0]["hostKind"] == "editor");
    Servitor::GlobalOptions pid22;
    pid22.pid = 22;
    const auto only22 = Servitor::StatusData(scan, pid22)["hosts"];
    REQUIRE(only22.size() == 1);
    CHECK(only22[0]["pid"] == 22);
}
// ---- Final fix round 2 ----------------------------------------------------

// M-1: an explicit --client on a verb that posts nothing (status, launch,
// commands, list) would be dropped: refused. The default name is not a flag.
TEST_CASE("an explicit --client is refused by verbs that post no call", "[servitor][client]") {
    Servitor::GlobalOptions opt;
    std::string error;
    REQUIRE(ParseFlags({"servitor", "status"}, opt, error));
    CHECK_FALSE(opt.clientSet);
    for (const Servitor::VerbRow& row : Servitor::VerbTable())
        CHECK_FALSE(Servitor::UnusedGlobal(row, opt).has_value());   // the default is never a refusal
    REQUIRE(ParseFlags({"servitor", "--client", "ci", "status"}, opt, error));
    CHECK(opt.clientSet);
    for (const char* words : {"status", "launch", "commands", "list"}) {
        INFO(words);
        const auto problem = Servitor::UnusedGlobal(*Servitor::FindVerb(words), opt);
        REQUIRE(problem.has_value());
        CHECK(*problem == "--client does not apply to " + std::string(words));
    }
    for (const char* words : {"cvar set", "call", "wait", "quit"})
        CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb(words), opt).has_value());
}

// I-1: under --all a rediscovered "replacement" could be a sibling the fan-out
// already reached, so the two flags never combine.
TEST_CASE("--retry-on-death with --all is refused before anything is sent", "[servitor][client]") {
    Servitor::GlobalOptions opt;
    opt.all = true;
    opt.yes = true;
    opt.retryOnDeath = true;
    for (const char* words : {"cvar set", "cvar get", "quit"}) {
        INFO(words);
        const auto problem = Servitor::UnusedGlobal(*Servitor::FindVerb(words), opt);
        REQUIRE(problem.has_value());
        CHECK(problem->find("--retry-on-death") != std::string::npos);
        CHECK(problem->find("--all") != std::string::npos);
        // Round 4 (N3-1): the advice is a kind, never a pid (a restart has a new pid).
        CHECK(problem->find("--host <kind>") != std::string::npos);
        CHECK(problem->find("--pid") == std::string::npos);
    }
    opt.retryOnDeath = false;
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("cvar set"), opt).has_value());
}

// ---- Final fix round 3 ----------------------------------------------------

// M-1: logs --follow and problems --follow open a GET stream that carries no
// X-Remote-Client, so an explicit --client is refused there, beside --world
// and --read-only (FollowVerb runs this check first).
TEST_CASE("the --follow streams refuse an explicit --client, --world and --read-only", "[servitor][client]") {
    Servitor::GlobalOptions opt;
    CHECK_FALSE(Servitor::FollowUnusedGlobal("logs", opt).has_value());
    opt.client = Servitor::DefaultClientId();   // the default is not a flag
    CHECK_FALSE(Servitor::FollowUnusedGlobal("logs", opt).has_value());
    Servitor::GlobalOptions client;
    client.client = "ci";
    client.clientSet = true;
    const auto problem = Servitor::FollowUnusedGlobal("problems", client);
    REQUIRE(problem.has_value());
    CHECK(problem->starts_with("--client does not apply to problems --follow"));
    Servitor::GlobalOptions world;
    world.world = "main";
    CHECK(Servitor::FollowUnusedGlobal("logs", world)->starts_with("--world does not apply to logs --follow"));
    Servitor::GlobalOptions readOnly;
    readOnly.readOnly = true;
    CHECK(Servitor::FollowUnusedGlobal("logs", readOnly)->starts_with("--read-only does not apply to logs --follow"));
}

// ---- Final fix round 4 ----------------------------------------------------

// N3-1: a restarted host always has a new pid, so --pid can never be
// retargeted by --retry-on-death: the pair is refused before anything is sent.
TEST_CASE("--retry-on-death with --pid is refused before anything is sent", "[servitor][client]") {
    Servitor::GlobalOptions opt;
    opt.retryOnDeath = true;
    opt.pid = 4242;
    for (const char* words : {"cvar set", "wait", "call", "quit"}) {
        INFO(words);
        const auto problem = Servitor::UnusedGlobal(*Servitor::FindVerb(words), opt);
        REQUIRE(problem.has_value());
        CHECK(problem->find("--retry-on-death") != std::string::npos);
        CHECK(problem->find("--pid") != std::string::npos);
        CHECK(problem->find("--host <kind>") != std::string::npos);
    }
    opt.pid = 0;
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("wait"), opt).has_value());
    opt.retryOnDeath = false;
    opt.pid = 4242;
    CHECK_FALSE(Servitor::UnusedGlobal(*Servitor::FindVerb("wait"), opt).has_value());
}

// N3-4: a --follow stream neither retries a busy answer nor rediscovers a
// death (it ends Failed), so an explicit --retry-on-death or --no-retry would
// be dropped: refused, beside --client, --world and --read-only.
TEST_CASE("the --follow streams refuse --retry-on-death and --no-retry", "[servitor][client]") {
    Servitor::GlobalOptions death;
    death.retryOnDeath = true;
    const auto d = Servitor::FollowUnusedGlobal("logs", death);
    REQUIRE(d.has_value());
    CHECK(d->starts_with("--retry-on-death does not apply to logs --follow"));
    Servitor::GlobalOptions noRetry;
    noRetry.noRetry = true;
    const auto n = Servitor::FollowUnusedGlobal("problems", noRetry);
    REQUIRE(n.has_value());
    CHECK(n->starts_with("--no-retry does not apply to problems --follow"));
}

// Round 4 add-on: `servitor --help` and `servitor -h` with no verb print the
// usage on stdout and exit 0 (they were "unknown flag --help", exit 2). With
// a verb, the token stays the verb's unknown flag; `servitor help` is
// untouched (the spec is silent on it).
TEST_CASE("servitor --help and -h with no verb print the usage and exit 0", "[servitor][client]") {
    CHECK(Servitor::IsHelpRequest(std::vector<std::string>{"--help"}));
    CHECK(Servitor::IsHelpRequest(std::vector<std::string>{"-h"}));
    CHECK_FALSE(Servitor::IsHelpRequest(std::vector<std::string>{}));
    CHECK_FALSE(Servitor::IsHelpRequest(std::vector<std::string>{"cvar", "get", "--help"}));
    CHECK_FALSE(Servitor::IsHelpRequest(std::vector<std::string>{"help"}));
    const std::string help = Servitor::HelpText();
    CHECK(help.starts_with("usage: servitor [global flags] <verb> [args]"));
    CHECK(help.find("unknown flag") == std::string::npos);
    CHECK(help.find("global flags:") != std::string::npos);
    CHECK(help.find("  status") != std::string::npos);
    // Run answers it before verb matching; it prints the text above to stdout once.
    const char* longForm[] = {"servitor", "--help"};
    CHECK(Servitor::Run(2, const_cast<char**>(longForm)) == 0);
    const char* shortForm[] = {"servitor", "--json", "-h"};
    CHECK(Servitor::Run(3, const_cast<char**>(shortForm)) == 0);
}

#endif
