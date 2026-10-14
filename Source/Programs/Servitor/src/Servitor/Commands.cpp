#include <Servitor/Commands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>
#include <Servitor/Transport.hpp>

#include <Arcane/Remote/Redact.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace Servitor {

namespace {

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

std::string Dump(const nlohmann::json& j)
{
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// One row of the manifest's verbs (rev 8.3 V8): the P0 verb surface. The
// drift guard ([servitor][client]) requires every VerbTable row's first word
// to be here; a verb a later task has not wired yet answers "unknown verb".
struct ManifestVerb {
    const char* name;
    const char* summary;
    const char* access;   // observe | drive | per-command | none
};

constexpr ManifestVerb kManifestVerbs[] = {
    {"status", "List the project's live hosts from their descriptors (no connection)", "observe"},
    {"launch", "Start an editor, runtime or server with --remote and wait until it is ready", "drive"},
    {"quit", "Ask the host to exit cleanly; --force ends the process after --timeout", "drive"},
    {"wait", "Wait until the host is ready or settled, N frames pass, a module reloads, or it reaches a state",
     "observe"},
    {"world", "List the host's worlds", "observe"},
    {"commands", "Print this manifest (verbs and exit codes) with no host running", "none"},
    {"list", "List the host's live command catalogue", "observe"},
    {"describe", "Describe one host command: its declaration and its args schema", "observe"},
    {"schema", "Print the JSON Schema of a reflected component or resource type", "observe"},
    {"call", "Invoke any host command by its dotted name with JSON args", "per-command"},
    {"cvar", "Get, set, list or explain console variables", "per-command"},
    {"console", "Run one console command line on the host", "per-command"},
    {"logs", "Read the host's log ring", "observe"},
    {"problems", "List the host's diagnostics", "observe"},
    {"entity", "Read one entity's reflected components", "observe"},
    {"ecs", "Query entities by component, or print ECS statistics", "observe"},
    {"resource", "List or read the world's resources", "observe"},
    {"scene", "Print the scene's entity tree", "observe"},
    {"selection", "Print the editor's selection", "observe"},
    {"stats", "Print the host's frame statistics", "observe"},
    {"screenshot", "Capture the viewport or the window to a PNG", "observe"},
    {"compare", "Compare a capture against a reference image (--bless writes the reference and drives)",
     "observe"},
    {"play", "Enter play mode", "drive"},
    {"stop", "Leave play mode", "drive"},
    {"pause", "Pause the simulation", "drive"},
    {"resume", "Resume the simulation", "drive"},
    {"step", "Advance the paused simulation by N fixed ticks", "drive"},
};

// One row of the manifest's exit table (spec s6.6 + AM1/AM6/AM8, rev 8.3 V7/V8).
struct ManifestExit {
    int code;
    const char* meaning;
    bool launcherOnly;
};

constexpr ManifestExit kManifestExits[] = {
    {0, "ok (compare verdicts Passed, PassedOnFallback and Skipped included)", false},
    {1, "internal: an error inside servitor itself", false},
    {2, "usage or refusal: unknown verb, flag or argument; ambiguous target; schema failure; capture.no-renderer",
     false},
    {3, "auth or permission: auth.denied, auth.throttled, permission.denied, permission.read-only, cvar.protected",
     false},
    {4, "no host: target.none, a stale descriptor, protocol.mismatch, engine.unresolved", false},
    {5, "host.busy: provably not run, retryable (minimized, queue-full, capture-pending)", false},
    {6, "the host answered and the operation failed (entity.not-found, compare.missing-reference, ...)", false},
    {7, "unreachable, died or restarting: host.unreachable, host.died, host.hung, host.restarting", false},
    {8, "a check ran and did not pass: compare.failed, a wait timeout (ok true, no errors)", false},
    {9, "launcher: engine.not-found, package.not-installed, package.incompatible", true},
    {130, "interrupted (SIGINT)", false},
    {143, "terminated (SIGTERM)", false},
};

std::string Pad(std::string_view text, std::size_t width)
{
    std::string out(text);
    out.append(out.size() < width ? width - out.size() : 1, ' ');
    return out;
}

void Write(const std::string& text)
{
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
}

// The selected host's identity for an envelope's `host` block.
Arcane::Remote::HostBlock BlockOf(const Arcane::Remote::Descriptor& d)
{
    Arcane::Remote::HostBlock block;
    block.kind = Arcane::Remote::ToString(d.hostKind);
    block.pid = d.pid;
    block.state = d.state;
    block.frame = d.frame;
    return block;
}

std::string HumanCatalogue(const nlohmann::json& catalogue)
{
    std::string text = "ok list\ngeneration: " + Dump(catalogue.value("generation", nlohmann::json(0))) + "\n";
    std::size_t width = 0;
    for (const nlohmann::json& row : catalogue.at("commands"))
        width = std::max(width, row.value("name", std::string{}).size());
    for (const nlohmann::json& row : catalogue.at("commands")) {
        text += "  " + Pad(row.value("name", std::string{}), width + 2) +
                Pad(row.value("access", std::string{}), 9) + row.value("summary", std::string{}) + "\n";
    }
    return text;
}

} // namespace

nlohmann::json StaticManifest()
{
    nlohmann::json verbs = nlohmann::json::array();
    for (const ManifestVerb& v : kManifestVerbs)
        verbs.push_back({{"name", v.name}, {"summary", v.summary}, {"access", v.access}});
    nlohmann::json exits = nlohmann::json::array();
    for (const ManifestExit& e : kManifestExits)
        exits.push_back({{"code", e.code}, {"meaning", e.meaning}, {"launcherOnly", e.launcherOnly}});
    nlohmann::json majors = nlohmann::json::array();
    for (const std::uint32_t m : kProtocolMajors)
        majors.push_back(m);
    return {
        {"version", std::string(kVersion)},
        {"protocolMajors", std::move(majors)},
        {"verbs", std::move(verbs)},
        {"exitCodes", std::move(exits)},
    };
}

std::string RenderStaticManifest(const GlobalOptions& opt)
{
    if (opt.format != Format::Human)
        return Dump(StaticManifest()) + "\n";
    std::string text = VersionText() + "\nverbs:\n";
    for (const ManifestVerb& v : kManifestVerbs)
        text += "  " + Pad(v.name, 12) + Pad(v.access, 13) + v.summary + "\n";
    text += "exit codes:\n";
    for (const ManifestExit& e : kManifestExits)
        text += "  " + Pad(std::to_string(e.code), 5) + (e.launcherOnly ? "(launcher only) " : "") + e.meaning + "\n";
    return text;
}

int PrintStaticManifest(const GlobalOptions& opt)
{
    std::fputs(RenderStaticManifest(opt).c_str(), stdout);
    std::fflush(stdout);
    return 0;
}

namespace {

constexpr VerbFlag kCallFlags[] = {
    {"--args-json", true},   // the whole args object, inline
    {"--args-file", true},   // the whole args object, from a file
};

} // namespace

std::span<const VerbFlag> CallFlags()
{
    return kCallFlags;
}

bool ParseCallTokens(std::span<const std::string> rest, CallRequest& out, std::string* error)
{
    const VerbRow* row = FindVerb("call");
    if (row == nullptr)
        return Fail(error, "the call verb is not in the verb table");
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*row, rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.empty())
        return Fail(error, "call needs a command name (servitor list shows them)");

    nlohmann::json args = nlohmann::json::object();
    std::string source;
    for (const auto& [flag, value] : tokens.flags) {
        if (!source.empty())
            return Fail(error, "pass the args object once: --args-json or --args-file, not " + source + " and " + flag);
        source = flag;
        std::string text = value;
        if (flag == "--args-file") {
            const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return Fail(error, "--args-file cannot read " + value);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded())
            return Fail(error, flag + " is not valid JSON");
        if (!parsed.is_object())
            return Fail(error, flag + " must be a JSON object, got " + std::string(parsed.type_name()));
        args = std::move(parsed);
    }
    for (std::size_t i = 1; i < tokens.positional.size(); ++i) {
        const std::string& token = tokens.positional[i];
        const std::size_t eq = token.find('=');
        if (eq == std::string::npos || eq == 0)
            return Fail(error, "call arguments are name=value, got '" + token + "'");
        const std::string key = token.substr(0, eq);
        if (args.contains(key))
            return Fail(error, "argument '" + key + "' is given twice");
        const std::string text = token.substr(eq + 1);
        nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
        args[key] = parsed.is_discarded() ? nlohmann::json(text) : std::move(parsed);
    }
    out.command = tokens.positional.front();
    out.args = std::move(args);
    return true;
}

bool DescribeArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("describe"), rest, &tokens))
        return Fail(error, *problem);
    if (tokens.positional.size() != 1)
        return Fail(error, "describe needs one command name (servitor list shows them)");
    out = {{"name", tokens.positional.front()}};
    return true;
}

bool SchemaArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("schema"), rest, &tokens))
        return Fail(error, *problem);
    const auto& p = tokens.positional;
    if (p.size() != 2 || (p[0] != "component" && p[0] != "resource"))
        return Fail(error, "schema needs component or resource, then one type name");
    out = {{"kind", p[0]}, {"name", p[1]}};
    return true;
}

bool CallArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    CallRequest req;
    if (!ParseCallTokens(rest, req, error))
        return false;
    out = {{"command", req.command}, {"args", req.args}};
    return true;
}

int CommandsVerb(std::span<const std::string> /*rest: Run refuses any; the row takes none*/, const GlobalOptions& opt)
{
    return PrintStaticManifest(opt);
}

int ListVerb(std::span<const std::string> /*rest: Run refuses any; the row takes none*/, const GlobalOptions& opt)
{
    Arcane::Remote::ClientBlock client;
    Arcane::Remote::Envelope env;
    std::optional<LiveHost> host;
    if (const auto dir = RemoteDirFor(opt, &env))
        host = Select(ScanDirectory(*dir), opt, &env);
    if (!host) {
        env.command = "list";
        client.exitCode = Arcane::Remote::ExitFromHost(env);
        return PrintEnvelope(env, client, opt);
    }
    client.token = host->descriptor.token;   // print-time redaction only; never printed
    client.descriptor = PathUtf8(host->path);
    Arcane::Remote::HostBlock block = BlockOf(host->descriptor);

    Arcane::Remote::Envelope failure;
    const std::optional<nlohmann::json> catalogue =
        GetJson(host->descriptor, "/remote/v1/catalog", opt.timeoutMs, &failure);
    if (!catalogue) {
        failure.command = "list";
        if (failure.host.pid == 0)
            failure.host = block;
        client.exitCode = Arcane::Remote::ExitFromHost(failure);
        return PrintEnvelope(failure, client, opt);
    }
    const auto commands = catalogue->find("commands");
    if (!catalogue->is_object() || commands == catalogue->end() || !commands->is_array()) {
        env = Arcane::Remote::MakeError("list", "internal", "the host's catalogue has no commands array",
                                        "report the host build; GET /remote/v1/catalog is malformed");
        env.host = block;
        client.exitCode = Arcane::Remote::ExitFromHost(env);
        return PrintEnvelope(env, client, opt);
    }
    block.catalog = catalogue->value("generation", 0u);
    env = Arcane::Remote::MakeOk("list", *catalogue, block);
    client.exitCode = Arcane::Remote::ExitFromHost(env);
    if (opt.format == Format::Human) {
        Write(Arcane::Remote::RedactSecrets(HumanCatalogue(*catalogue), client.token));
        return client.exitCode;
    }
    return PrintEnvelope(env, client, opt);
}

int CallVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    std::string error;
    if (!ParseCallTokens(rest, req, &error))
        return PrintUsage(error);
    const PostedCall posted = PostVerb(*FindVerb("call"), req, opt, false);
    return PrintEnvelope(posted.envelope, posted.client, opt);
}

} // namespace Servitor

#endif
