#include <catch2/catch_test_macros.hpp>
#include <Servitor/Commands.hpp>
#include <Servitor/Client.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Commands/CatalogCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Json.hpp>
#include "RemoteDemoArgs.hpp"
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <filesystem>
#include <fstream>
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

#endif
