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
// argv[1..]. Any other token is left for the verb. False (exit 2) sets *error.
bool ParseGlobals(int argc, char** argv, GlobalOptions& out, std::string* error);

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
Printed Call(const CallRequest& req, const GlobalOptions& opt, ExchangeFn exchange, void* user,
             Arcane::Remote::ClientBlock* client);
// servitor.exe's main calls this.
int Run(int argc, char** argv);

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
