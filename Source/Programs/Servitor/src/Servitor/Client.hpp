#pragma once
// The Servitor client core (spec s4.6, s6; plan rev 8.3 V3-V7): global flags,
// the one call path with its busy retry, and the mid-call target watch.
// Program code: no export macro (V4); protocol types stay qualified (no using
// in a header). Includes no ArcaneRemote host header (V3).
#include <Servitor/Format.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

ARC_CONSTANT("the Servitor client's own version (spec AM9); servitor --version prints it")
inline constexpr std::string_view kVersion = "0.1.0";
ARC_CONSTANT("remote protocol majors this client speaks (spec AM9)")
inline constexpr std::array<std::uint32_t, 1> kProtocolMajors = {1};

// "Servitor <kVersion> (remote protocol <kProtocolMajors, comma-joined>)".
std::string VersionText();
// kProtocolMajors comma-joined ("1"), for remediation text.
std::string ProtocolMajorsText();
// True when `major` is in kProtocolMajors.
bool SpeaksProtocolMajor(std::int64_t major);
// protocol.mismatch (exit 4) for a host speaking `hostMajor`; the message and
// remediation name both sides (rev 8.3 V6). One wording for the reply check
// and for Select's descriptor check.
Arcane::Remote::Envelope ProtocolMismatch(const std::string& command, std::int64_t hostMajor);

// system_clock milliseconds since the epoch: the one clock for heartbeats,
// IsHung and requestUnixMs.
std::int64_t NowUnixMs();
// A path as UTF-8 with forward slashes, for envelopes.
std::string PathUtf8(const std::filesystem::path& p);

struct GlobalOptions {
    Format format = Format::Human; // TTY default; non-TTY default is Json
    bool json = false;
    bool verbose = false;
    bool tty = false;
    bool readOnly = false;          // --read-only; Call posts it as options.readOnly (ruling F15)
    bool yes = false;
    bool noRetry = false;
    bool retryOnDeath = false;
    std::uint32_t timeoutMs = 30000; // milliseconds. --timeout <s> is seconds; ParseGlobals scales.
    std::string project;
    std::string hostKind;       // editor | runtime | server, optional
    std::uint32_t pid = 0;
    std::string engine;         // --engine <root>; rev 8.3 V7 (HostsDir)
};

// Starts from GlobalOptions{} and applies the global flags found anywhere in
// argv[1..]. Any other token is left for the verb. False (exit 2) sets *error,
// including a value flag whose value starts with "--" (--project --json).
bool ParseGlobals(int argc, char** argv, GlobalOptions& out, std::string* error);

// The usage problem for the first token after the verb that starts with "--"
// (an unknown flag for a verb that declares none). Run checks the tokens
// through SplitVerbTokens (Verbs.hpp), which applies the same rule to a row's
// undeclared flags. A single '-' stays a value (negative numbers). nullopt: none.
std::optional<std::string> LeftoverFlag(std::span<const std::string> rest);
// Prints "servitor: <problem>" and the usage text to stderr; returns 2.
int PrintUsage(const std::string& problem);
// Renders `env` in opt's format (stdout, diagnostics on stderr); returns client.exitCode.
int PrintEnvelope(const Arcane::Remote::Envelope& env, const Arcane::Remote::ClientBlock& client,
                  const GlobalOptions& opt);

struct CallRequest {
    std::string command;
    nlohmann::json args = nlohmann::json::object();
};

struct ExchangeResult {
    Arcane::Remote::Envelope envelope;
    bool connectionRefusedBeforeBody = false;
};

using ExchangeFn = ExchangeResult (*)(const std::string& requestBody, void* user);

// Posts {id, command, args, options:{timeoutMs[, readOnly]}} through `exchange`,
// retries a provably-not-run busy answer with backoff until opt.timeoutMs, checks
// the reply's protocol major, and renders it. Opens no socket itself.
// *finalEnvelope (when non-null) receives the envelope it rendered.
Printed Call(const CallRequest& req, const GlobalOptions& opt, ExchangeFn exchange, void* user,
             Arcane::Remote::ClientBlock* client, Arcane::Remote::Envelope* finalEnvelope = nullptr);
// servitor.exe's main calls this.
int Run(int argc, char** argv);

// The newest *.arcdiag directly under `dir` written strictly after
// requestUnixMs; equal mtimes break on the greater generic_string(). Empty
// when none qualifies (or `dir` is empty).
std::filesystem::path NewestReportAfter(const std::filesystem::path& dir, std::int64_t requestUnixMs);

struct TargetWatch {
    std::string command;
    std::uint32_t pid = 0;
    std::uint64_t processStart = 0;
    std::filesystem::path descriptorPath;
    std::filesystem::path diagnosticsDir;
    std::int64_t requestUnixMs = 0;
};

// file_clock <-> system_clock without clock_cast (R8-7: libc++ on macOS has
// none). libstdc++ and libc++ give file_clock to_sys/from_sys; MSVC's STL
// gives it to_utc/from_utc only, so that branch goes through utc_clock.
inline std::chrono::system_clock::time_point FileTimeToSys(std::filesystem::file_time_type t)
{
#if defined(_MSVC_STL_VERSION)
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::utc_clock::to_sys(std::chrono::file_clock::to_utc(t)));
#else
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(t));
#endif
}

inline std::filesystem::file_time_type SysToFileTime(std::chrono::system_clock::time_point t)
{
#if defined(_MSVC_STL_VERSION)
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        std::chrono::file_clock::from_utc(std::chrono::utc_clock::from_sys(t)));
#else
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(
        std::chrono::file_clock::from_sys(t));
#endif
}

// nullopt: the pid is live and the descriptor heartbeat is fresh.
// An engaged optional is a finished error envelope. The caller does not post it.
std::optional<Arcane::Remote::Envelope> ObserveTarget(const TargetWatch& watch, std::int64_t nowUnixMs);

} // namespace Servitor

#endif
