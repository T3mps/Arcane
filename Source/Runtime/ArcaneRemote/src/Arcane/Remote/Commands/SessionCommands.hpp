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
//  - `settled` on a host with no renderer: capture.no-renderer (exit 2);
//    on a renderer host with no settle hook installed: host.unavailable (exit 5);
//  - the condition met: MakeOk(host.wait, {done: true, what, ...}) (exit 0);
//  - timeoutMs == 0, or more than timeoutMs since `started`:
//    MakeOk(host.wait, {timedOut: true, what}) -- ok, no errors, exit 8;
//  - otherwise MakeOk(host.wait, {done: false, what}): still pending.
// `ready` is state ready, playing or paused; `frames` is gate.frame >=
// startedFrame + frames (host.frame counts sim advances, AM1); `reload` is
// gate.pluginGeneration != generationAtArm (HostSession::NoteReload moves it;
// PluginHost leaves the generation alone on a failed or refused reload, so a
// failed reload keeps the wait pending until it times out); `state` is
// gate.State() == state; `settled` is the host's settled(hookUser) hook.
ARC_REMOTE_API Envelope WaitPoll(const HostSession& session, const WaitArgs& args, std::uint64_t startedFrame,
                                 std::uint32_t generationAtArm, bool hasRenderer,
                                 std::chrono::steady_clock::time_point started);

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
