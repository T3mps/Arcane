#include <Servitor/Client.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Servitor/Discover.hpp>
#include <Servitor/FanOut.hpp>
#include <Servitor/Transport.hpp>
#include <Servitor/Verbs.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Redact.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>     // _isatty, _fileno
#else
#include <unistd.h> // isatty, STDOUT_FILENO
#endif

namespace Servitor {

namespace {

ARC_CONSTANT("timing: how often an awaitsHost verb (wait) rescans for a host that has not written its descriptor yet")
constexpr int kDiscoveryPollMs = 100;

bool StdoutIsTty()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

bool Fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
    return false;
}

// --timeout <s>: seconds, decimal allowed (spec s4.6). The whole token must
// parse; the rounded millisecond count must fit u32; anything that rounds
// below 1 ms is stored as 1.
bool ParseTimeoutSeconds(const char* text, std::uint32_t& outMs, std::string* error)
{
    const std::string bad = std::string("--timeout needs a non-negative number of seconds, got '") + text + "'";
    if (*text == '\0')
        return Fail(error, bad);
    errno = 0;
    char* end = nullptr;
    const double seconds = std::strtod(text, &end);
    if (end != text + std::strlen(text) || errno == ERANGE || !std::isfinite(seconds) || seconds < 0.0)
        return Fail(error, bad);
    const double ms = seconds * 1000.0;
    // Range first: llround of a value outside long long is undefined. A value
    // at or past max + 0.5 rounds above the u32 maximum.
    if (!(ms < static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 0.5))
        return Fail(error, "--timeout '" + std::string(text) + "' is too large (the limit is " +
                               std::to_string(std::numeric_limits<std::uint32_t>::max() / 1000u) + " seconds)");
    outMs = std::max(1u, static_cast<std::uint32_t>(std::llround(ms)));
    return true;
}

bool ParseFormat(std::string_view text, Format& out, std::string* error)
{
    if (text == "json") { out = Format::Json; return true; }
    if (text == "human") { out = Format::Human; return true; }
    if (text == "ndjson") { out = Format::Ndjson; return true; }
    return Fail(error, "unknown format " + std::string(text) + " (json, human or ndjson)");
}

bool ParsePid(const char* text, std::uint32_t& out, std::string* error)
{
    const std::string bad = std::string("--pid needs a process id, got '") + text + "'";
    if (*text < '0' || *text > '9')
        return Fail(error, bad);
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (end != text + std::strlen(text) || errno == ERANGE || v == 0 ||
        v > std::numeric_limits<std::uint32_t>::max())
        return Fail(error, bad);
    out = static_cast<std::uint32_t>(v);
    return true;
}

// The global flags may appear anywhere; every other token is the verb's, in order.
bool ParseArgs(int argc, char** argv, GlobalOptions& out, std::string* error, std::vector<std::string>* rest)
{
    out = GlobalOptions{};
    out.tty = StdoutIsTty();
    bool formatSet = false;
    Format format = Format::Json;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        // A bare "--" ends servitor's own flags: it and every token after it
        // belong to the verb, verbatim (launch's host flags, Task 16), so a
        // host's --project or --timeout is never read as servitor's.
        if (flag == "--") {
            if (rest != nullptr) {
                for (int j = i; j < argc; ++j)
                    rest->emplace_back(argv[j]);
            }
            break;
        }
        // A flag that takes a value reads the next token, which must exist and
        // must not itself be a flag (--project --json); a single '-' is a value.
        const auto value = [&](const char*& v) {
            if (i + 1 >= argc)
                return Fail(error, std::string(flag) + " needs a value");
            if (std::string_view(argv[i + 1]).starts_with("--"))
                return Fail(error, std::string(flag) + " needs a value, got the flag " + argv[i + 1]);
            v = argv[++i];
            return true;
        };
        const char* v = nullptr;
        if (flag == "--json") {
            out.json = true;
        } else if (flag == "--format") {
            if (!value(v) || !ParseFormat(v, format, error))
                return false;
            formatSet = true;
        } else if (flag == "--verbose") {
            out.verbose = true;
        } else if (flag == "--read-only") {
            out.readOnly = true;
        } else if (flag == "--yes") {
            out.yes = true;
        } else if (flag == "--no-retry") {
            out.noRetry = true;
        } else if (flag == "--retry-on-death") {
            out.retryOnDeath = true;
        } else if (flag == "--timeout") {
            if (!value(v) || !ParseTimeoutSeconds(v, out.timeoutMs, error))
                return false;
        } else if (flag == "--project") {
            if (!value(v))
                return false;
            out.project = v;
        } else if (flag == "--host") {
            if (!value(v))
                return false;
            const std::string_view kind = v;
            if (kind != "editor" && kind != "runtime" && kind != "server")
                return Fail(error, "--host is editor, runtime or server, got '" + std::string(kind) + "'");
            out.hostKind = v;
        } else if (flag == "--pid") {
            if (!value(v) || !ParsePid(v, out.pid, error))
                return false;
        } else if (flag == "--engine") {
            if (!value(v))
                return false;
            out.engine = v;
        } else if (flag == "--world") {
            if (!value(v))
                return false;
            out.world = v;
        } else if (flag == "--all") {
            // --all [editor|runtime|server]: the token after it is a kind filter
            // only when it spells one (a verb word is never a kind).
            out.all = true;
            if (i + 1 < argc) {
                const std::string_view kind = argv[i + 1];
                if (kind == "editor" || kind == "runtime" || kind == "server") {
                    out.allKind = kind;
                    ++i;
                }
            }
        } else if (rest != nullptr) {
            rest->emplace_back(flag);
        }
    }
    // --json equals --format json and wins over TTY detection (AM4); a
    // non-TTY stdout with no format flag is JSON.
    if (out.json)
        out.format = Format::Json;
    else if (formatSet)
        out.format = format;
    else
        out.format = out.tty ? Format::Human : Format::Json;
    return true;
}

// A reply whose protocol major this client does not speak is protocol.mismatch
// (exit 4), whatever it says; the remediation names both sides (rev 8.3 V6).
void CheckProtocol(const std::string& command, Arcane::Remote::Envelope& env)
{
    if (SpeaksProtocolMajor(env.protocol))
        return;
    auto mismatch = ProtocolMismatch(command, env.protocol);
    mismatch.protocol = env.protocol;
    mismatch.protocolMinor = env.protocolMinor;
    mismatch.id = env.id;
    mismatch.host = env.host;
    env = std::move(mismatch);
}

// command.unknown keeps exit 2; its remediation names the command and the
// host's major.minor, so an older host reads as one (rev 8.3 V6).
void ExplainUnknownCommand(const std::string& command, Arcane::Remote::Envelope& env)
{
    if (env.errors.empty() || env.errors.front().code != "command.unknown")
        return;
    // catalog.describe names the command it could not find in detail.name.
    const nlohmann::json& detail = env.errors.front().detail;
    const std::string missing = detail.is_object() && detail.contains("name") && detail.at("name").is_string()
                                    ? detail.at("name").get<std::string>()
                                    : command;
    auto& remediation = env.errors.front().remediation;
    const std::string note = "the host (remote protocol " + std::to_string(env.protocol) + "." +
                             std::to_string(env.protocolMinor) + ") has no command " + missing +
                             "; update the host or use a servitor that matches it";
    remediation = remediation.empty() ? note : remediation + "; " + note;
}

// Only a transient, provably-not-run exit 5 is retried. host.unavailable is
// also exit 5 but means a host hook was never installed: it answers at once.
// So does host.quit's unsaved-changes guard (host.busy{state:"modal"}): it
// waits on a person, not on time, so retrying it only burns --timeout.
bool TransientBusy(const std::string& command, const ExchangeResult& r)
{
    if (r.connectionRefusedBeforeBody)
        return true;
    if (r.envelope.errors.empty())
        return false;
    const Arcane::Remote::Error& e = r.envelope.errors.front();
    if (command == "host.quit" && e.code == "host.busy" && e.detail.is_object() &&
        e.detail.value("state", std::string{}) == "modal")
        return false;
    return e.code == "host.busy" || e.code == "capture.minimized";
}

} // namespace

// FileTimeToSys, not clock_cast: libc++ (macOS) has no clock_cast (R8-7).
std::filesystem::path NewestReportAfter(const std::filesystem::path& dir, std::int64_t requestUnixMs)
{
    namespace fs = std::filesystem;
    using namespace std::chrono;
    fs::path best;
    system_clock::time_point bestTime{};
    std::string bestKey;
    if (dir.empty())
        return best;
    const sys_time<milliseconds> after{milliseconds(requestUnixMs)};
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.extension() != ".arcdiag")
            continue;
        std::error_code timeEc;
        const auto written = it->last_write_time(timeEc);
        if (timeEc)
            continue;
        const auto sys = FileTimeToSys(written);
        if (!(sys > after))
            continue;
        const std::string key = p.generic_string();
        if (best.empty() || sys > bestTime || (sys == bestTime && key > bestKey)) {
            best = p;
            bestTime = sys;
            bestKey = key;
        }
    }
    return best;
}

namespace {

#if defined(_WIN32)
// The exit code of {pid, processStart} once that exact process has exited.
// A second open (IsProcessLive closed its own handle): a creation-time
// mismatch is a different process, and a failed open says nothing.
std::optional<DWORD> ExitedCode(std::uint32_t pid, std::uint64_t processStart)
{
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return std::nullopt;
    std::optional<DWORD> code;
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        const std::uint64_t start =
            (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const bool hasExited = exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0;
        DWORD value = 0;
        if (start == processStart && hasExited && ::GetExitCodeProcess(h, &value))
            code = value;
    }
    ::CloseHandle(h);
    return code;
}
#endif

void Write(std::FILE* stream, const std::string& text)
{
    if (!text.empty()) {
        std::fwrite(text.data(), 1, text.size(), stream);
        std::fflush(stream);
    }
}

} // namespace

int Emit(const Printed& printed, int exitCode)
{
    Write(stdout, printed.stdoutText);
    Write(stderr, printed.stderrText);
    return exitCode;
}

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string PathUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

Arcane::Remote::Envelope ProtocolMismatch(const std::string& command, std::int64_t hostMajor)
{
    const std::string host = std::to_string(hostMajor);
    auto env = Arcane::Remote::MakeError(
        command, "protocol.mismatch",
        "the host speaks remote protocol " + host + "; this servitor speaks " + ProtocolMajorsText(),
        "use a servitor that speaks remote protocol " + host + ", or a host that speaks remote protocol " +
            ProtocolMajorsText());
    env.protocol = static_cast<int>(hostMajor);
    return env;
}

int PrintEnvelope(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client,
                  const GlobalOptions& opt)
{
    RenderOptions ro;
    ro.format = opt.format;
    ro.tty = opt.tty;
    return Emit(Render(env, client, ro), client.exitCode);
}

int PrintUsage(const std::string& problem)
{
    std::string text = "servitor: " + problem + "\nusage: servitor [global flags] <verb> [args]\nverbs:";
    for (const VerbRow& row : VerbTable()) {
        text += "\n  ";
        text += row.words;
        if (row.usage != nullptr) {
            text += ' ';
            text += row.usage;
        }
    }
    text += "\nglobal flags: --json, --format json|human|ndjson, --timeout <s>, --project <dir>, "
            "--host editor|runtime|server, --pid <n>, --engine <root>, --world <name>, --all [editor|runtime|server], --read-only, --yes, --no-retry, "
            "--retry-on-death, --verbose, --version\n";
    Write(stderr, text);
    return 2;
}

std::string VersionText()
{
    return "Servitor " + std::string(kVersion) + " (remote protocol " + ProtocolMajorsText() + ")";
}

std::string ProtocolMajorsText()
{
    std::string text;
    for (const std::uint32_t m : kProtocolMajors) {
        if (!text.empty())
            text += ", ";
        text += std::to_string(m);
    }
    return text;
}

bool SpeaksProtocolMajor(std::int64_t major)
{
    return major >= 0 && std::find(kProtocolMajors.begin(), kProtocolMajors.end(),
                                   static_cast<std::uint64_t>(major)) != kProtocolMajors.end();
}

std::optional<std::string> LeftoverFlag(std::span<const std::string> rest)
{
    for (const std::string& token : rest) {
        if (token.starts_with("--"))
            return "unknown flag " + token;
    }
    return std::nullopt;
}

bool ParseGlobals(int argc, char** argv, GlobalOptions& out, std::string* error)
{
    return ParseArgs(argc, argv, out, error, nullptr);
}

Printed Call(const CallRequest& req, const GlobalOptions& opt, ExchangeFn exchange, void* user,
             Arcane::Remote::ClientBlock* client, Arcane::Remote::Envelope* finalEnvelope)
{
    nlohmann::json body = {
        {"id", "c1"},
        {"command", req.command},
        {"args", req.args},
        {"options", {{"timeoutMs", opt.timeoutMs}}},
    };
    if (opt.readOnly)
        body["options"]["readOnly"] = true;   // ruling F15
    if (!opt.world.empty())
        body["options"]["world"] = opt.world; // ruling F14; the listener copies it onto Call::world
    const std::string payload = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    Arcane::Remote::ClientBlock local;
    if (client == nullptr)
        client = &local;
    client->retried = false;
    if (opt.verbose) {
        const std::string line = "servitor request: POST /remote/v1/call Authorization: Bearer " +
                                 client->token + " " + payload;
        ARC_INFO("{}", Arcane::Remote::RedactSecrets(line, client->token));   // stderr only (R8-8)
    }
    ExchangeResult last{};
    const auto started = std::chrono::steady_clock::now();
    int pauseMs = 10;
    for (;;) {
        last = exchange(payload, user);
        if (!last.connectionRefusedBeforeBody)
            CheckProtocol(req.command, last.envelope);
        const int code = last.connectionRefusedBeforeBody ? 5 : Arcane::Remote::ExitFromHost(last.envelope);
        const bool busy = code == 5 && !opt.noRetry && TransientBusy(req.command, last);
        const bool death = code == 7 && opt.retryOnDeath;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if ((busy || death) && elapsed + pauseMs < static_cast<long long>(opt.timeoutMs)) {
            client->retried = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
            pauseMs = pauseMs < 50 ? 50 : std::min(1000, pauseMs * 2);
            continue;
        }
        client->exitCode = code;
        break;
    }
    ExplainUnknownCommand(req.command, last.envelope);
    if (finalEnvelope != nullptr)
        *finalEnvelope = last.envelope;
    RenderOptions ro;
    ro.format = opt.format;
    ro.tty = opt.tty;
    return Render(last.envelope, *client, ro);
}

int Run(int argc, char** argv)
{
    // --version answers before flag parsing and verb matching (rev 8.3 V6).
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            Write(stdout, VersionText() + "\n");
            return 0;
        }
    }
    GlobalOptions opt;
    std::string error;
    std::vector<std::string> tokens;
    if (!ParseArgs(argc, argv, opt, &error, &tokens))
        return PrintUsage(error);
    if (tokens.empty())
        return PrintUsage("no verb given");
    std::size_t consumed = 0;
    const VerbRow* row = MatchVerb(tokens, &consumed);
    if (row == nullptr)
        return PrintUsage(tokens.front().starts_with("-") ? "unknown flag " + tokens.front()
                                                          : "unknown verb " + tokens.front());
    const auto rest = std::span<const std::string>(tokens).subspan(consumed);
    // Every global flag was consumed above, so a leftover "--x" is unknown
    // unless the verb's row declares it (Task 14 review carry; Task 16 rows
    // declare their flags, and launch passes everything after "--" through).
    if (const auto problem = SplitVerbTokens(*row, rest, nullptr))
        return PrintUsage(*problem);
    if (row->args == nullptr && !rest.empty())
        return PrintUsage(std::string(row->words) + " takes no arguments, got " + rest.front());
    // Rule 3: --world / --read-only on a verb that posts no call is refused, never dropped.
    if (const auto problem = UnusedGlobal(*row, opt))
        return PrintUsage(*problem);
    if (opt.all)
        return RunFanOut(*row, rest, opt);   // spec AM2: one command to every matching live host
    if (row->local != nullptr)
        return row->local(rest, opt);
    CallRequest req;
    req.command = row->command;
    if (row->args != nullptr && !row->args(rest, req.args, &error))
        return PrintUsage(error);
    const PostedCall posted = PostVerb(*row, req, opt, false);
    return PrintEnvelope(posted.envelope, posted.client, opt);
}

PostedCall PostVerb(const VerbRow& row, const CallRequest& req, GlobalOptions opt, bool noReplyGrace)
{
    PostedCall posted;
    // Discovery (spec s4.4): the project's descriptors, then exactly one host.
    // Only a row whose awaitsHost predicate accepts these args (`wait ready`)
    // keeps looking for up to --timeout while there is no host yet, so a wait
    // right after a launch finds it booting; the call then gets what is left
    // of the budget. Every other verb with no host is target.none at once.
    const bool awaits = row.awaitsHost != nullptr && row.awaitsHost(req.args);
    Arcane::Remote::Envelope refusal;
    const auto discoveryStart = std::chrono::steady_clock::now();
    for (;;) {
        refusal = Arcane::Remote::Envelope{};
        if (const auto dir = RemoteDirFor(opt, &refusal))
            posted.host = Select(ScanDirectory(*dir), opt, &refusal);
        const bool none = !posted.host && !refusal.errors.empty() && refusal.errors.front().code == "target.none";
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - discoveryStart).count();
        if (!awaits || !none || elapsed + kDiscoveryPollMs >= static_cast<long long>(opt.timeoutMs))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(kDiscoveryPollMs));
    }
    if (posted.host && awaits) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - discoveryStart).count();
        opt.timeoutMs = static_cast<std::uint32_t>(std::max<long long>(1, static_cast<long long>(opt.timeoutMs) - spent));
    }
    if (!posted.host) {
        refusal.command = req.command;
        posted.client.exitCode = Arcane::Remote::ExitFromHost(refusal);
        posted.envelope = std::move(refusal);
        return posted;
    }

    return PostToHost(*posted.host, req, opt, noReplyGrace);
}

PostedCall PostToHost(const LiveHost& host, const CallRequest& req, GlobalOptions opt, bool noReplyGrace)
{
    PostedCall posted;
    posted.host = host;
    LiveTarget target;
    target.descriptor = host.descriptor;
    target.timeoutMs = opt.timeoutMs;
    if (noReplyGrace)
        target.replyGraceMs = 0;
    target.watch.command = req.command;
    target.watch.pid = host.descriptor.pid;
    target.watch.processStart = host.descriptor.processStart;
    target.watch.descriptorPath = host.path;
    // The host's report directory (ruling AX (2)): <project>/Saved/Diagnostics
    // when the descriptor names a project, else the exe-relative one.
    if (!host.descriptor.project.empty()) {
        const std::string& p = host.descriptor.project;
        const std::filesystem::path project(std::u8string(reinterpret_cast<const char8_t*>(p.data()), p.size()));
        target.watch.diagnosticsDir =
            Arcane::Paths::Resolve(Arcane::Paths::Location::DiagnosticsDir, Arcane::Paths::ForProject(project));
    } else {
        target.watch.diagnosticsDir = HostDiagnosticsDir(host.descriptor.hostKind, opt);
    }
    posted.client.token = host.descriptor.token;   // print-time redaction only; never printed
    posted.client.descriptor = PathUtf8(host.path);
    (void)Call(req, opt, &LiveExchange, &target, &posted.client, &posted.envelope);
    return posted;
}

std::optional<Arcane::Remote::Envelope> ObserveTarget(const TargetWatch& watch, std::int64_t nowUnixMs)
{
    const bool live = Arcane::Platform::IsProcessLive(watch.pid, watch.processStart);
    const std::optional<Arcane::Remote::Descriptor> desc = Arcane::Remote::ReadDescriptor(watch.descriptorPath);
    // Died wins over a stale heartbeat; a live process with no readable
    // heartbeat is hung, because a fresh one cannot be proved.
    if (live && desc && !Arcane::Remote::IsHung(*desc, nowUnixMs))
        return std::nullopt;

    std::string message;
    if (!live)
        message = "host process " + std::to_string(watch.pid) + " exited during the call";
    else if (!desc)
        message = "host process " + std::to_string(watch.pid) +
                  " is running but its descriptor is unreadable, so a fresh heartbeat cannot be proved";
    else
        message = "host process " + std::to_string(watch.pid) + " has not refreshed its heartbeat for " +
                  std::to_string(nowUnixMs - desc->heartbeatUnixMs) + " ms";
    auto env = Arcane::Remote::MakeError(watch.command, live ? "host.hung" : "host.died", std::move(message),
                                         "relaunch");
    env.host.pid = watch.pid;
    if (desc) {
        env.host.state = desc->state;
        env.host.kind = Arcane::Remote::ToString(desc->hostKind);
    }
    auto& detail = env.errors.front().detail;
    detail["lastState"] = desc ? desc->state : std::string{};
#if defined(_WIN32)
    // POSIX gives an exit status only to the parent, and the target is not
    // servitor's child, so detail.exitCode exists on Windows alone.
    if (!live) {
        if (const auto code = ExitedCode(watch.pid, watch.processStart))
            detail["exitCode"] = *code;
    }
#endif
    if (const auto report = NewestReportAfter(watch.diagnosticsDir, watch.requestUnixMs); !report.empty())
        detail["arcdiag"] = PathUtf8(report);
    return env;
}

} // namespace Servitor

#endif
