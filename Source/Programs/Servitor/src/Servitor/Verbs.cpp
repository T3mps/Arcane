#include <Servitor/Verbs.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>
#include <Servitor/Launch.hpp>

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Platform/Process.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string_view>
#include <system_error>
#include <thread>

namespace Servitor {

namespace {

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

ARC_CONSTANT("timing: how often quit --force samples whether the target process is gone")
constexpr int kQuitPollMs = 50;
ARC_CONSTANT("timing: how long quit --force waits for a killed host to leave the process table")
constexpr int kKillSettleMs = 5000;

// quit [--force] [--discard-changes] -> host.quit {force, discardChanges} (Task 16).
constexpr VerbFlag kQuitFlags[] = {
    {"--force", false},             // terminate the process if it is still alive after --timeout (spec s4.7)
    {"--discard-changes", false},   // the editor's guard, answered "drop them" (a drive command, audited)
    // Controller ruling (fix round 1): the report path is the host's own --report flag.
    {"--report", true, "quit takes no --report; pass --report at launch: servitor launch <kind> -- --report <path>"},
};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
int QuitVerb(std::span<const std::string> rest, const GlobalOptions& opt);
bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);

// Only `wait ready` waits for a host to appear (spec s4.4).
bool WaitAwaitsHost(const nlohmann::json& args)
{
    return args.is_object() && args.value("what", std::string{}) == "ready";
}

// Tasks 15-29 add their rows here (ruling F17).
const std::array<VerbRow, 5> kVerbs = {{
    {"status", nullptr, nullptr, &StatusVerb},   // client-side: lists live hosts, no connect (Task 15)
    {"stats frame", "stats.frame", nullptr, nullptr},
    // Task 16. launch is client-side: it spawns the host and waits for its descriptor.
    {"launch", nullptr, &LaunchArgs, &LaunchVerb, LaunchFlags(), true, nullptr,
     "editor|runtime|server [--headless] [--backend dx12|vulkan] [--remote-descriptor <path>] [-- <host flags>]"},
    // quit posts host.quit; --force also ends the process after --timeout, so it is client-side.
    {"quit", "host.quit", &QuitArgs, &QuitVerb, kQuitFlags, false, nullptr, "[--force] [--discard-changes]"},
    // `frames N` counts host.frame, the host's sim advances (AM1): one per fixed
    // tick on a server, a headless runtime or a step; a windowed runtime in Run
    // mode advances it once per loop pass, whatever its sub-steps.
    {"wait", "host.wait", &WaitArgs, nullptr, {}, false, &WaitAwaitsHost,
     "ready|settled|frames N|reload|state <s>   (frames counts host.frame: sim advances, not rendered frames)"},
}};

bool QuitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("quit"), rest, &tokens))
        return Fail(error, *problem);
    if (!tokens.positional.empty())
        return Fail(error, "quit takes no arguments, got " + tokens.positional.front());
    bool force = false;
    bool discard = false;
    for (const auto& [flag, value] : tokens.flags) {
        (void)value;
        force = force || flag == "--force";
        discard = discard || flag == "--discard-changes";
    }
    out = {{"force", force}, {"discardChanges", discard}};
    return true;
}

bool WaitArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error)
{
    const std::string usage = "wait needs ready, settled, frames N, reload or state <s>";
    VerbTokens tokens;
    if (const auto problem = SplitVerbTokens(*FindVerb("wait"), rest, &tokens))
        return Fail(error, *problem);
    const auto& p = tokens.positional;
    if (p.empty())
        return Fail(error, usage);
    const std::string& what = p.front();
    if (what == "ready" || what == "settled" || what == "reload") {
        if (p.size() != 1)
            return Fail(error, "wait " + what + " takes nothing more, got " + p[1]);
        out = {{"what", what}};
        return true;
    }
    if (what == "frames") {
        if (p.size() != 2)
            return Fail(error, "wait frames needs one count N");
        const std::string& n = p[1];
        errno = 0;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(n.c_str(), &end, 10);
        if (n.empty() || n[0] < '0' || n[0] > '9' || end != n.c_str() + n.size() || errno == ERANGE ||
            v > std::numeric_limits<std::uint32_t>::max())
            return Fail(error, "wait frames needs a non-negative whole number, got '" + n + "'");
        out = {{"what", "frames"}, {"frames", static_cast<std::uint32_t>(v)}};
        return true;
    }
    if (what == "state") {
        std::string names;
        for (const std::string_view name : Arcane::Remote::kHostStateNames)
            names += (names.empty() ? "" : ", ") + std::string(name);
        if (p.size() != 2)
            return Fail(error, "wait state needs one state: " + names);
        if (!Arcane::Remote::IsHostStateName(p[1]))
            return Fail(error, "wait state needs one of " + names + ", got '" + p[1] + "'");
        out = {{"what", "state"}, {"state", p[1]}};
        return true;
    }
    return Fail(error, usage + ", got " + what);
}

// quit: posts host.quit. With --force (spec s4.7) it then watches the target
// process (pid + creation stamp from its descriptor) until --timeout, measured
// from the start of the verb, and terminates it if it is still alive -- also
// when the host answered host.busy{modal} (--force does not discard changes;
// --discard-changes does) or did not answer at all. A host that is gone is
// exit 0 with data.forced (true when servitor killed it); what the host
// answered before the kill is kept in `warnings`.
int QuitVerb(std::span<const std::string> rest, const GlobalOptions& opt)
{
    CallRequest req;
    req.command = "host.quit";
    std::string error;
    if (!QuitArgs(rest, req.args, &error))
        return PrintUsage(error);
    const bool force = req.args.value("force", false);
    const auto started = std::chrono::steady_clock::now();
    PostedCall posted = PostVerb(*FindVerb("quit"), req, opt, force);
    if (!force || !posted.host)
        return PrintEnvelope(posted.envelope, posted.client, opt);

    const Arcane::Remote::Descriptor& d = posted.host->descriptor;
    const auto alive = [&d] { return Arcane::Platform::IsProcessLive(d.pid, d.processStart); };
    const auto deadline = started + std::chrono::milliseconds(opt.timeoutMs);
    while (alive() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(kQuitPollMs));
    bool forced = false;
    if (alive()) {
        forced = Arcane::Platform::TerminateProcess(d.pid, d.processStart);
        const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(kKillSettleMs);
        while (alive() && std::chrono::steady_clock::now() < settle)
            std::this_thread::sleep_for(std::chrono::milliseconds(kQuitPollMs));
    }

    Arcane::Remote::Envelope env;
    if (!alive()) {
        nlohmann::json data = posted.envelope.ok && posted.envelope.data.is_object() ? posted.envelope.data
                                                                                   : nlohmann::json::object();
        data["forced"] = forced;
        data["exited"] = true;
        if (forced) {
            // A killed host cannot delete its own descriptor; it is stale now
            // (IsStale), so remove it the way a scan would.
            std::error_code ec;
            data["descriptorRemoved"] = std::filesystem::remove(posted.host->path, ec) && !ec;
        }
        env = Arcane::Remote::MakeOk("host.quit", std::move(data), posted.envelope.host);
        env.id = posted.envelope.id;
        env.timing = posted.envelope.timing;
        env.warnings = posted.envelope.warnings;
        if (!posted.envelope.ok) {
            for (const Arcane::Remote::Error& e : posted.envelope.errors)
                env.warnings.push_back(e);   // e.g. host.busy{modal}, or host.unreachable from a stuck host
        }
    } else {
        env = Arcane::Remote::MakeError("host.quit", "quit.force-failed",
                                        "host process " + std::to_string(d.pid) +
                                            " is still running after --force: the kill was refused",
                                        "end process " + std::to_string(d.pid) + " by hand");
        env.errors.front().detail["pid"] = d.pid;
        env.host = posted.envelope.host;
    }
    if (env.host.pid == 0) {
        env.host.pid = d.pid;
        env.host.kind = Arcane::Remote::ToString(d.hostKind);
    }
    posted.client.exitCode = Arcane::Remote::ExitFromHost(env);
    return PrintEnvelope(env, posted.client, opt);
}

// How many leading tokens spell `words` (space-separated); 0 when they do not.
std::size_t WordsMatched(std::string_view words, std::span<const std::string> tokens)
{
    std::size_t used = 0;
    while (!words.empty()) {
        const std::size_t space = words.find(' ');
        const std::string_view word = words.substr(0, space);
        if (used >= tokens.size() || tokens[used] != word)
            return 0;
        ++used;
        words = space == std::string_view::npos ? std::string_view{} : words.substr(space + 1);
    }
    return used;
}

} // namespace

std::span<const VerbRow> VerbTable()
{
    return kVerbs;
}

const VerbRow* MatchVerb(std::span<const std::string> tokens, std::size_t* consumed)
{
    const VerbRow* best = nullptr;
    std::size_t bestUsed = 0;
    for (const VerbRow& row : kVerbs) {
        const std::size_t used = WordsMatched(row.words, tokens);
        if (used > bestUsed) {
            best = &row;
            bestUsed = used;
        }
    }
    if (consumed != nullptr)
        *consumed = bestUsed;
    return best;
}

const VerbRow* FindVerb(std::string_view words)
{
    for (const VerbRow& row : kVerbs) {
        if (words == row.words)
            return &row;
    }
    return nullptr;
}

std::optional<std::string> SplitVerbTokens(const VerbRow& row, std::span<const std::string> rest, VerbTokens* out)
{
    VerbTokens local;
    VerbTokens& tokens = out != nullptr ? *out : local;
    tokens = VerbTokens{};
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string& token = rest[i];
        if (token == "--" && row.passThrough) {
            tokens.passThrough.assign(rest.begin() + static_cast<std::ptrdiff_t>(i) + 1, rest.end());
            return std::nullopt;
        }
        if (!token.starts_with("--")) {   // a single '-' stays a value (negative numbers)
            tokens.positional.push_back(token);
            continue;
        }
        const VerbFlag* flag = nullptr;
        for (const VerbFlag& f : row.flags) {
            if (token == f.name)
                flag = &f;
        }
        if (flag == nullptr)
            return "unknown flag " + token;
        if (flag->refused != nullptr)
            return std::string(flag->refused);
        if (!flag->takesValue) {
            tokens.flags.emplace_back(token, std::string{});
            continue;
        }
        if (i + 1 >= rest.size() || rest[i + 1].starts_with("--"))
            return token + " needs a value";
        tokens.flags.emplace_back(token, rest[++i]);
    }
    return std::nullopt;
}

} // namespace Servitor

#endif
