#pragma once
// --all fan-out (spec AM2; plan Task 18): one command to every matching live
// host, in parallel, one envelope per host. Program code: no export macro (V4);
// protocol types stay qualified (no using in a header). Includes no ArcaneRemote
// host header (V3).
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>
#include <Servitor/Format.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <span>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

struct FanOutResult {
    nlohmann::json wrapper;  // {"hosts":[{"pid","hostKind","envelope"}]}; envelope carries "client":{exitCode,descriptor}
    int exitCode = 0;        // WorstExit of the per-host exits (7 > 6 > 8 > 5 > 4 > 3 > 2 > 1 > 0); a refusal is its own
    std::string text;        // stdout in the requested Format, every secret redacted with its own host's token
    std::string diagnostics; // stderr (human format and refusals only)
};

// Posts `call` ({command, args}) to one host and fills *out; returns that host's exit code.
// Runs on a worker thread, one per host, so it must not write shared state unguarded.
using FanOutPost = int (*)(const Arcane::Remote::Descriptor& descriptor, const nlohmann::json& call,
                           Arcane::Remote::Envelope* out, void* user);

// A drive command: cvar.set, console.exec, host.quit, play.*, or capture.compare
// with args.bless true. Everything else observes.
bool IsDriveCommand(const nlohmann::json& call);

// Sends `call` to every live host of `scan` (all kinds when allKinds, else only
// hostKindFilter) in parallel. A drive call without `yes` is arg.inapplicable
// (exit 2) and sends nothing; no matching host is target.none (exit 4).
// Nothing is printed: the caller emits `text` / `diagnostics`.
FanOutResult FanOut(const Scan& scan, std::string_view hostKindFilter, bool allKinds, const nlohmann::json& call,
                    bool drive, bool yes, Format format, FanOutPost post, void* user);

// Run's --all path for one matched verb row: status and commands run as usual
// (status already lists every host); a posting row, call and quit fan out; any
// other client-side verb is a usage error. Discovers the project's hosts,
// FanOuts through the live transport, prints, and returns the worst exit.
int RunFanOut(const VerbRow& row, std::span<const std::string> rest, const GlobalOptions& opt);

// RunFanOut after discovery: parses the verb's args, applies --pid / --host /
// --all <kind>, decides drive (a `call` is always a drive under --all: it can
// name any host command), FanOuts through `post`, and prints. captured non-null: stores the result instead of printing (the test seam).
int FanOutVerb(const VerbRow& row, std::span<const std::string> rest, const GlobalOptions& opt, const Scan& scan,
               FanOutPost post, void* user, FanOutResult* captured = nullptr);

} // namespace Servitor

#endif
