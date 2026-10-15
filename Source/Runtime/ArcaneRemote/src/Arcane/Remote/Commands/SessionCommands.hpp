#pragma once
// host.quit and host.wait (spec s4.7, s7.1; Servitor P0 Task 16): the session
// commands every armed host serves. HostSession::Start registers them with its
// own session and job table (ruling F12); HostSession::Stop releases them.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/Protocol.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class HostSession; // HostSession.hpp
class JobTable;    // Jobs.hpp

struct WaitArgs {
    std::string what;          // ready | settled | frames | reload | state
    std::uint32_t frames = 0;  // Optional
    std::string state;         // Optional
    std::uint32_t timeoutMs = 30000; // milliseconds, copied from Call::timeoutMs; host.wait does not scale
};

// One poll of a host.wait condition (ruling F9): reads session.Gate() and
// session.GetConfig() through the const accessors; `started` is when the wait
// job was armed. Returns, in order of precedence:
//  - `settled` on a host with no renderer (a server), or one that installed
//    no capture host pair: capture.no-renderer (exit 2; spec AM1: settled
//    captures, and captures are refused there); on a host with no
//    producersIdle hook: host.unavailable (exit 5);
//  - `reload` when a reload noted after the arm (session.ReloadSerial() moved
//    past reloadSerialAtArm) failed and the generation did not move:
//    module.reload-failed (exit 6), detail {pluginGeneration, atFrame};
//  - the condition met: MakeOk(host.wait, {done: true, what, ...}) (exit 0);
//  - timeoutMs == 0, or more than timeoutMs since `started`:
//    MakeOk(host.wait, {timedOut: true, what}) -- ok, no errors, exit 8;
//  - otherwise MakeOk(host.wait, {done: false, what}): still pending.
// `ready` is state ready, playing or paused; `frames` is gate.frame >=
// startedFrame + frames (host.frame counts sim advances, AM1); `reload` is
// gate.pluginGeneration != generationAtArm (HostSession::NoteReload moves it;
// PluginHost leaves the generation alone on a failed or refused reload, which
// DrainFrame notes from its attempt count); `state` is gate.State() == state.
// `settled` is never met here: the host.wait job decides it from its own
// captures (spec s4.7: two byte-equal captures through the capture host pair,
// with config.producersIdle true when each was armed and when it retired), so
// a poll of `settled` answers only its refusals, its timeout, or pending. The
// pair is byte-stable only under a frozen clock: launch the host with
// --fixed-time (the verb's help says so).
ARC_REMOTE_API Envelope WaitPoll(const HostSession& session, const WaitArgs& args, std::uint64_t startedFrame,
                                 std::uint32_t generationAtArm, bool hasRenderer,
                                 std::chrono::steady_clock::time_point started,
                                 std::uint64_t reloadSerialAtArm = 0);

// Registers host.quit and host.wait. Safe to call twice; a later call replaces
// the stored pointers (ruling F11). Null pointers make both commands answer
// host.unavailable (exit 5) without dereferencing.
ARC_REMOTE_API void RegisterSessionCommands(HostSession* session, JobTable* jobs);
// Clears the stored pointers when they name `session` (HostSession's teardown),
// so a stopped session's commands answer host.unavailable instead of reaching
// a dead object. Another session's pointers are left alone.
ARC_REMOTE_API void ReleaseSessionCommands(const HostSession* session);

} // namespace Arcane::Remote

#endif
