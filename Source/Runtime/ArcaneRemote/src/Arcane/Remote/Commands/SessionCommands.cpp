#include <Arcane/Remote/Commands/SessionCommands.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/HostSession.hpp>
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Reflection.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace Arcane::Remote
{

// The wire args of the two commands, reflected for their schema (spec s5.3).
// Each command reads its args JSON itself and refuses unknown keys.
struct HostQuitArgs {
    bool force = false;            // the client ends the process after --timeout (spec s4.7); the host quits as usual
    bool discardChanges = false;   // quit even with unsaved changes
};
ARC_REFLECT_TYPE(HostQuitArgs)
    ARC_REFLECT_FIELD(HostQuitArgs, force)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(HostQuitArgs, discardChanges)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

struct HostWaitArgs {
    std::string what;              // ready | settled | frames | reload | state
    std::uint32_t frames = 0;      // what = frames
    std::string state;             // what = state
};
ARC_REFLECT_TYPE(HostWaitArgs)
    ARC_REFLECT_FIELD(HostWaitArgs, what)
    ARC_REFLECT_FIELD(HostWaitArgs, frames)
        ARC_REFLECT_ATTR(Optional)
    ARC_REFLECT_FIELD(HostWaitArgs, state)
        ARC_REFLECT_ATTR(Optional)
ARC_REFLECT_TYPE_END()

namespace
{

// One armed host.wait job's inputs, recorded when the command armed it.
struct WaitRow {
    WaitArgs args;
    std::uint64_t startedFrame = 0;
    std::uint32_t generationAtArm = 0;
    bool hasRenderer = true;
    std::chrono::steady_clock::time_point started{};
};

// The stored pointers (ruling F11) and the armed waits, keyed by job id.
// Main thread only: commands and job steps both run inside Drain.
struct SessionCommandState {
    HostSession* session = nullptr;
    JobTable* jobs = nullptr;
    std::map<std::uint64_t, WaitRow> waits;
};

SessionCommandState& State()
{
    static SessionCommandState state;
    return state;
}

Envelope Unavailable(const std::string& command)
{
    return MakeError(command, "host.unavailable", "no armed host session serves " + command,
                     "relaunch the host with --remote");
}

bool IsPending(const Envelope& env)
{
    return env.ok && env.data.is_object() && !env.data.value("done", true);
}

// args must be an object (null reads as empty) whose keys are all in `known`.
std::optional<Envelope> CheckKeys(const Call& call, std::initializer_list<std::string_view> known,
                                  std::string_view usage)
{
    if (call.args.is_null())
        return std::nullopt;
    if (!call.args.is_object())
        return MakeError(call.command, "arg.invalid", call.command + " args must be a JSON object",
                         std::string(usage));
    for (const auto& [key, value] : call.args.items()) {
        (void)value;
        if (std::find(known.begin(), known.end(), std::string_view(key)) == known.end())
            return MakeError(call.command, "arg.unknown", call.command + " has no argument '" + key + "'",
                             std::string(usage));
    }
    return std::nullopt;
}

std::optional<Envelope> ReadBool(const Call& call, const char* key, bool& out)
{
    if (!call.args.is_object() || !call.args.contains(key))
        return std::nullopt;
    const nlohmann::json& v = call.args.at(key);
    if (!v.is_boolean())
        return MakeError(call.command, "arg.invalid", std::string(key) + " must be true or false",
                         std::string("pass ") + key + ": true");
    out = v.get<bool>();
    return std::nullopt;
}

std::string StateUsage()
{
    std::string text = "pass state: one of";
    for (const std::string_view name : kHostStateNames)
        text += " " + std::string(name);
    return text;
}

constexpr std::string_view kQuitUsage = "pass force or discardChanges (both optional booleans)";
constexpr std::string_view kWaitUsage = "pass what: ready, settled, frames (with frames: N), reload or state (with state: <s>)";

std::optional<Envelope> ParseWait(const Call& call, WaitArgs& out)
{
    if (auto refusal = CheckKeys(call, {"what", "frames", "state"}, kWaitUsage))
        return refusal;
    const nlohmann::json args = call.args.is_object() ? call.args : nlohmann::json::object();
    const auto what = args.find("what");
    if (what == args.end())
        return MakeError(call.command, "arg.missing", "host.wait needs what", std::string(kWaitUsage));
    if (!what->is_string())
        return MakeError(call.command, "arg.invalid", "what must be a string", std::string(kWaitUsage));
    out.what = what->get<std::string>();
    const std::string_view w = out.what;
    if (w != "ready" && w != "settled" && w != "frames" && w != "reload" && w != "state")
        return MakeError(call.command, "arg.invalid", "host.wait cannot wait for '" + out.what + "'",
                         std::string(kWaitUsage));

    if (const auto frames = args.find("frames"); frames != args.end()) {
        if (w != "frames")
            return MakeError(call.command, "arg.inapplicable", "frames applies to what: frames only",
                             std::string(kWaitUsage));
        if (!frames->is_number_integer() || frames->get<std::int64_t>() < 0 ||
            frames->get<std::int64_t>() > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
            return MakeError(call.command, "arg.invalid", "frames must be a non-negative integer",
                             "pass frames: N");
        out.frames = static_cast<std::uint32_t>(frames->get<std::int64_t>());
    } else if (w == "frames") {
        return MakeError(call.command, "arg.missing", "what: frames needs frames: N", "pass frames: N");
    }

    if (const auto state = args.find("state"); state != args.end()) {
        if (w != "state")
            return MakeError(call.command, "arg.inapplicable", "state applies to what: state only",
                             std::string(kWaitUsage));
        if (!state->is_string() || !IsHostStateName(state->get<std::string>()))
            return MakeError(call.command, "arg.invalid", "state must be a host state (spec s4.3)",
                             StateUsage());
        out.state = state->get<std::string>();
    } else if (w == "state") {
        return MakeError(call.command, "arg.missing", "what: state needs state: <s>", StateUsage());
    }
    return std::nullopt;
}

CommandResult HostQuit(const Call& call, void* /*user*/)
{
    CommandResult result;
    SessionCommandState& s = State();
    if (s.session == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    bool force = false;
    bool discardChanges = false;
    std::optional<Envelope> refusal = CheckKeys(call, {"force", "discardChanges"}, kQuitUsage);
    if (!refusal)
        refusal = ReadBool(call, "force", force);
    if (!refusal)
        refusal = ReadBool(call, "discardChanges", discardChanges);
    if (refusal) {
        result.envelope = std::move(*refusal);
        return result;
    }

    // The editor's unsaved-changes guard (spec s4.7): a dirty host does not
    // quit unless the caller says to drop the changes. Nothing has run.
    // `force` is the client's (it ends the process after --timeout); it does
    // not answer the guard -- the spec lists --discard-changes separately.
    (void)force;
    const HostSession::Config& config = s.session->GetConfig();
    const bool dirty = config.dirty != nullptr && config.dirty(config.hookUser);
    if (dirty && !discardChanges) {
        result.envelope = MakeError(call.command, "host.busy",
                                    "the host has unsaved changes; quitting now would drop them",
                                    "save them first, or pass --discard-changes");
        result.envelope.errors.front().detail["state"] = "modal";
        return result;
    }

    s.session->RequestQuit(); // the host loop's DrainFrame exits remote-quit after this Drain
    ARC_INFO("remote: host.quit accepted{}", dirty ? "; unsaved changes are discarded" : "");
    result.envelope = MakeOk(call.command, {{"quitting", true}, {"discardedChanges", dirty}}, {});
    return result;
}

bool StepWait(Job& job, HostGate& gate, void* user)
{
    SessionCommandState& s = *static_cast<SessionCommandState*>(user);
    const auto it = s.waits.find(job.id.value);
    if (it == s.waits.end() || s.session == nullptr) {
        // The session that armed this wait is gone (stopped, or replaced).
        job.envelope = Unavailable(job.command);
        if (it != s.waits.end())
            s.waits.erase(it);
        return true;
    }
    const WaitRow& row = it->second;
    Envelope env = WaitPoll(*s.session, row.args, row.startedFrame, row.generationAtArm, row.hasRenderer,
                            row.started);
    if (IsPending(env)) {
        if (row.args.what == "frames") {
            const std::uint64_t now = gate.frame.load(std::memory_order_acquire);
            job.progress.done = std::min<std::uint64_t>(now >= row.startedFrame ? now - row.startedFrame : 0,
                                                        row.args.frames);
        }
        return false;
    }
    job.envelope = std::move(env); // done, timed out, or a failure (MakeError): the row's ok envelope is replaced
    s.waits.erase(it);
    return true;
}

CommandResult HostWait(const Call& call, void* /*user*/)
{
    CommandResult result;
    SessionCommandState& s = State();
    if (s.session == nullptr || s.jobs == nullptr) {
        result.envelope = Unavailable(call.command);
        return result;
    }
    WaitArgs args;
    if (auto refusal = ParseWait(call, args)) {
        result.envelope = std::move(*refusal);
        return result;
    }
    args.timeoutMs = call.timeoutMs; // milliseconds from the wire; not scaled again (ruling AX (1))

    // Armed now; the dispatcher binds the call's waiter to the job, and each
    // later Drain's StepAll polls it once (s7.7: nothing blocks the main thread).
    const HostSession& session = *s.session;
    WaitRow row;
    row.args = args;
    row.startedFrame = session.Gate().frame.load(std::memory_order_acquire);
    row.generationAtArm = session.Gate().pluginGeneration;
    row.hasRenderer = session.GetConfig().kind != HostKind::Server;
    row.started = std::chrono::steady_clock::now();
    const JobId id = s.jobs->Arm(call.command, args.what == "frames" ? args.frames : 0u, &StepWait, &s);
    s.waits[id.value] = std::move(row);
    result.envelope = MakeOk(call.command, {{"done", false}, {"what", args.what}}, {});
    result.completesJob = id.value;
    return result;
}

void RegisterOnce(Desc desc)
{
    CommandRegistry& registry = CommandRegistry::Get();
    if (registry.Find(desc.name) != nullptr)
        return;
    std::string error;
    if (!registry.Register(std::move(desc), &error))
        ARC_ERROR("remote: session command refused: {}", error);
}

} // namespace

Envelope WaitPoll(const HostSession& session, const WaitArgs& args, std::uint64_t startedFrame,
                  std::uint32_t generationAtArm, bool hasRenderer, std::chrono::steady_clock::time_point started)
{
    const HostGate& gate = session.Gate();
    const std::string command = "host.wait";
    nlohmann::json data = {{"done", true}, {"what", args.what}};
    bool done = false;

    if (args.what == "ready") {
        const std::string state = gate.State();
        done = IsReadyState(state);
        data["state"] = state;
    } else if (args.what == "settled") {
        if (!hasRenderer)
            return MakeError(command, "capture.no-renderer", "this host has no renderer",
                             "wait on an editor or a runtime");
        const HostSession::Config& config = session.GetConfig();
        if (config.settled == nullptr)
            return MakeError(command, "host.unavailable", "this host installed no settle hook",
                             "wait on an editor or a runtime host");
        done = config.settled(config.hookUser);
    } else if (args.what == "frames") {
        const std::uint64_t frame = gate.frame.load(std::memory_order_acquire);
        done = frame >= startedFrame + args.frames;
        data["frame"] = frame;
        data["startedFrame"] = startedFrame;
    } else if (args.what == "reload") {
        done = gate.pluginGeneration != generationAtArm;
        data["pluginGeneration"] = gate.pluginGeneration;
    } else if (args.what == "state") {
        if (!IsHostStateName(args.state))
            return MakeError(command, "arg.invalid", "'" + args.state + "' is not a host state (spec s4.3)",
                             StateUsage());
        const std::string state = gate.State();
        done = state == args.state;
        data["state"] = state;
    } else {
        return MakeError(command, "arg.invalid", "host.wait cannot wait for '" + args.what + "'",
                         std::string(kWaitUsage));
    }

    if (done)
        return MakeOk(command, std::move(data), gate.Block());
    if (args.timeoutMs == 0 ||
        std::chrono::steady_clock::now() - started > std::chrono::milliseconds(args.timeoutMs))
        return MakeOk(command, {{"timedOut", true}, {"what", args.what}}, gate.Block());
    return MakeOk(command, {{"done", false}, {"what", args.what}}, gate.Block());
}

void RegisterSessionCommands(HostSession* session, JobTable* jobs)
{
    SessionCommandState& s = State();
    s.session = session;
    s.jobs = jobs;
    s.waits.clear(); // a wait armed under the previous pointers answers host.unavailable

    Desc quit;
    quit.name = "host.quit";
    quit.summary = "Exit the host cleanly at the next frame boundary (exitReason remote-quit)";
    quit.access = Access::Drive;
    quit.mutates = Mutates::Project; // ends the host's session with its project
    quit.play = PlayPolicy::Any;
    quit.thread = ExecThread::Main;
    quit.argsMeta = Astra::GetMeta<HostQuitArgs>();
    quit.fn = &HostQuit;
    RegisterOnce(std::move(quit));

    Desc wait;
    wait.name = "host.wait";
    wait.summary = "Wait until the host is ready or settled, N frames pass, a module reloads, or it reaches a state";
    wait.access = Access::Observe;
    wait.mutates = Mutates::None;
    wait.play = PlayPolicy::Any;
    wait.thread = ExecThread::Job;
    wait.argsMeta = Astra::GetMeta<HostWaitArgs>();
    wait.fn = &HostWait;
    RegisterOnce(std::move(wait));
}

void ReleaseSessionCommands(const HostSession* session)
{
    SessionCommandState& s = State();
    if (session == nullptr || s.session != session)
        return;
    s.session = nullptr;
    s.jobs = nullptr;
    s.waits.clear();
}

} // namespace Arcane::Remote

#endif
