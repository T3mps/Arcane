#pragma once
// play.start, play.stop, play.pause, play.resume and play.step (spec s7.3,
// AM1; Servitor P0 Task 29): the host side of `servitor play|stop|pause|resume|step`.
// All five are Drive commands (the dispatcher's read-only gate and audit
// apply), Mutates::PlayMode, needsFrame false, not world scoped, and answer
// undo {label: null, undoable: false, reason: PlayHooks::undoReason}.
//
// Runtime and server drive their AM1 TickGate: start and resume run it, stop
// holds it and drops pending steps, pause holds it. The editor drives its
// PlaySession and RunLoop through the function pointers below, which the
// editor program installs (R8-2: this module includes no editor header).
//
// play.step N is a frame-spanning job (s7.7, 0.12): the command arms one job
// and returns. It never calls Advance, AdvanceSim or RequestSingleStep itself.
// Runtime and server: the command requests N gate steps and the host loop's
// Consume performs one Advance per iteration. Editor: each job step (one per
// Drain) requests one single step until the frame delta reaches N. The job
// completes when HostGate::frame minus the frame at arm time reaches N.
#include <Arcane/Modules/ArcaneRemoteApi.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/TickGate.hpp>

#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

class JobTable;  // Jobs.hpp
struct HostGate; // Dispatcher.hpp

struct PlayHooks {
    TickGate* gate = nullptr;                 // runtime and server
    bool (*editorPlay)(void*, std::string_view topology) = nullptr; // PlaySession::Play; topology is the CLI spelling
    bool (*editorStop)(void*) = nullptr;      // PlaySession::Stop
    void (*editorSetPaused)(bool paused, void*) = nullptr;
    void (*editorRequestSingleStep)(void*) = nullptr;
    void* editorUser = nullptr;
    bool editor = false;
    FrameStats* stats = nullptr;
    std::string undoReason; // "play-mode" on the editor, "runtime-host" on runtime and server
    // Task 29 addition (last, so the fields above keep their brace order):
    // the editor's PlaySession::IsPlaying. Pause, resume and step act only
    // inside Play on the editor (the toolbar disables them in Edit, where the
    // RunLoop's pause IS the edit-mode sim freeze), so they answer
    // play.not-playing when it returns false, and a running step ends
    // play.stopped when Play ends under it. Null: not checked.
    bool (*editorPlaying)(void*) = nullptr;
};

// The CLI spellings of play.start's topology (spec s7.3): standalone (the
// default), listen, client+server. Anything else answers arg.invalid.
ARC_REMOTE_API bool IsPlayTopology(std::string_view topology);

// All three are safe to call twice; a later call replaces the stored hooks /
// pointer (ruling F11). A null JobTable* or HostGate* makes play.* answer
// host.unavailable (exit 5) without dereferencing it (ruling S3-9).
// RegisterPlayCommands with a different table (nullptr included) finishes
// every play.step job still running in the previous one (Cancelled,
// host.unavailable), so no waiter is left parked on a table nobody steps.
ARC_REMOTE_API void SetPlayHooks(PlayHooks hooks);
ARC_REMOTE_API void SetPlayGate(HostGate* gate);       // the gate play.step reads its arm-time frame from
ARC_REMOTE_API void RegisterPlayCommands(JobTable* jobs);

} // namespace Arcane::Remote

#endif
