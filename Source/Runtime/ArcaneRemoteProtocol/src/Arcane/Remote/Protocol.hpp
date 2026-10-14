#pragma once
#include <Arcane/Modules/ArcaneRemoteProtocolApi.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Json.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Arcane::Remote
{

ARC_CONSTANT("wire protocol: remote protocol major (spec s6)")
inline constexpr std::uint32_t kProtocol = 1;
ARC_CONSTANT("remote protocol minor; raised on every additive wire change (spec AM9); remote-protocol.json mirrors it")
inline constexpr std::uint32_t kProtocolMinor = 0;
ARC_CONSTANT("wire protocol: s10.1 request body bound")
inline constexpr std::uint32_t kMaxBodyBytes = 4u * 1024u * 1024u;
ARC_CONSTANT("wire protocol: s10.1 header block bound (16 KiB)")
inline constexpr std::uint32_t kMaxHeaderBytes = 16u * 1024u;
ARC_CONSTANT("capacity: s10.1 connection cap, under Winsock FD_SETSIZE (64); POSIX polls with poll(2)")
inline constexpr std::uint32_t kMaxConnections = 16;
ARC_CONSTANT("capacity: s10.1 SSE subscriber cap")
inline constexpr std::uint32_t kMaxSseSubscribers = 8;
ARC_CONSTANT("a change would be a bug: the one spelling of remote.server.queueDepth's default")
inline constexpr std::uint32_t kQueueDepthDefault = 64;
ARC_CONSTANT("capacity hint: s4.3 default commands drained per tick")
inline constexpr std::uint32_t kFrameBudgetCommands = 8;
ARC_CONSTANT("a change would be a bug: the one spelling of remote.server.frameBudgetMs' default")
inline constexpr double kFrameBudgetMs = 4.0;
ARC_CONSTANT("wire protocol: s10.1 idle connection close")
inline constexpr std::uint32_t kIdleConnectionSeconds = 30;

enum class HostKind : std::uint8_t { Editor, Runtime, Server };
enum class HostState : std::uint8_t { Starting, Ready, Playing, Paused, Reloading, Stopping, Hung };
enum class Access : std::uint8_t { Observe, Drive };
enum class Mutates : std::uint8_t { None, Scene, Project, Preferences, PlayMode };
enum class PlayPolicy : std::uint8_t { Any, EditOnly, PlayingOnly };
enum class ExecThread : std::uint8_t { Main, Service, Job };

struct Error {
    std::string code;
    std::string message;
    std::string remediation;
    nlohmann::json detail = nlohmann::json::object();
    nlohmann::json locator = nullptr;
    std::vector<std::string> candidates;
    std::string check;
};

struct HostBlock {
    std::string kind;                 // "editor" | "runtime" | "server"
    std::uint32_t pid = 0;
    std::string world;
    std::uint64_t frame = 0;
    std::string state;
    std::uint32_t catalog = 0;
    std::uint32_t pluginGeneration = 0;
};

struct Artifact {
    std::string kind;
    std::string path;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::string format;
};

struct UndoBlock {
    nlohmann::json label = nullptr;   // string or JSON null
    bool undoable = false;
    std::string reason;               // play-mode | runtime-host | not-undoable-command
    std::uint64_t transaction = 0;
};

struct Timing {
    double queuedMs = 0;
    double ranMs = 0;
};

struct Envelope {
    bool ok = false;
    int protocol = 1;
    std::uint32_t protocolMinor = kProtocolMinor;
    std::string id;
    std::string command;
    HostBlock host;
    nlohmann::json data = nlohmann::json::object();
    std::vector<Error> errors;
    std::vector<Error> warnings;
    std::vector<Error> notifications;
    std::vector<Artifact> artifacts;
    std::optional<UndoBlock> undo;    // nullopt serializes as null
    Timing timing;
};

struct ClientBlock {
    int exitCode = 0;
    bool retried = false;
    std::string descriptor;
    std::string token; // print-time only; ToJson(Envelope) never emits this
};

struct HostStatus {
    std::uint32_t protocol = kProtocol;
    std::uint32_t protocolMinor = kProtocolMinor;
    HostKind hostKind = HostKind::Editor;
    std::uint32_t pid = 0;
    std::uint64_t processStart = 0;
    std::uint16_t port = 0;
    std::string state;
    std::uint64_t frame = 0;
    std::string world;
    std::uint32_t abi = 0;
    std::string project;
    std::uint32_t catalogueGeneration = 0;
    std::uint32_t pluginGeneration = 0;
    bool reloadOk = true;
    std::string reloadReason;
    std::uint64_t reloadAtFrame = 0;
    std::vector<std::string> capabilities;
    std::int64_t heartbeatUnixMs = 0;
    bool minimized = false;
};

ARC_REMOTE_PROTOCOL_API const char* ToString(HostKind kind);
ARC_REMOTE_PROTOCOL_API nlohmann::json ToJson(const Envelope& env);
ARC_REMOTE_PROTOCOL_API nlohmann::json ToJson(const HostStatus& status); // no token field exists on HostStatus
ARC_REMOTE_PROTOCOL_API Envelope ParseEnvelope(const nlohmann::json& j);
ARC_REMOTE_PROTOCOL_API std::optional<Envelope> TryParseEnvelope(const nlohmann::json& j); // nullopt when malformed
ARC_REMOTE_PROTOCOL_API Envelope MakeOk(std::string command, nlohmann::json data, HostBlock host);
ARC_REMOTE_PROTOCOL_API Envelope MakeError(std::string command, std::string code, std::string message, std::string remediation);
ARC_REMOTE_PROTOCOL_API int ExitFromHost(const Envelope& env);
ARC_REMOTE_PROTOCOL_API int WorstExit(std::span<const int> codes);

} // namespace Arcane::Remote

#else
// Dist builds compile the remote surface out; this empty branch keeps the include legal.
#endif
