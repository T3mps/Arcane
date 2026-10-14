#pragma once
// Servitor's live transport (plan rev 8.3 V3): one HTTP/1.1 exchange per call
// with a host's loopback listener, over ArcaneNet's TcpSocket.hpp and
// ArcaneRemoteProtocol's HttpClient.hpp. Program code: no export macro (V4).
#include <Servitor/Client.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

ARC_CONSTANT("sanity bound, not a tunable: a host reply past 64 MiB is malformed (ParseHttpReply has no upper bound)")
inline constexpr std::size_t kMaxReplyBytes = 64u * 1024u * 1024u;
ARC_CONSTANT("spec s4.7 / ruling AX (2): the mid-call liveness sample period")
inline constexpr int kWatchPollMs = 250;
ARC_CONSTANT("loopback connect bound: a listener that has not accepted in 2 s is not there")
inline constexpr std::uint32_t kConnectTimeoutMs = 2000;
ARC_CONSTANT("slack past options.timeoutMs before the client stops reading: the host answers its own timeout first")
inline constexpr std::uint32_t kReplyGraceMs = 5000;

using ObserveFn = std::optional<Arcane::Remote::Envelope> (*)(const TargetWatch& watch, std::int64_t nowUnixMs);

struct LiveTarget {
    Arcane::Remote::Descriptor descriptor;
    TargetWatch watch;
    // The call's whole budget (--timeout), shared by every exchange of one
    // Call: each attempt gets what is left of it since `started`, so retries
    // never stretch a run past --timeout (+ the connect cap + the grace).
    std::uint32_t timeoutMs = 30000;
    std::uint32_t replyGraceMs = kReplyGraceMs;      // the host answers its own timeout first
    std::size_t maxReplyBytes = kMaxReplyBytes;
    ObserveFn observe = &ObserveTarget;              // the Task 14 target watch; tests substitute it
    std::chrono::steady_clock::time_point started{}; // set by the call's first exchange
};

// Connects to 127.0.0.1:desc.port (ConnectSocketWithTimeout), sends BuildPost("/remote/v1/call", ...)
// non-blocking under the call's deadline, then loops PollSockets(250 ms) -> recv -> ParseHttpReply
// until a reply or a watch verdict. Every wake, including a timeout, samples the watch first (Task 14
// Target watch); the first sample runs before the connect. A connect refused, or a send that fails,
// before the whole body was sent is re-sampled: a died/hung verdict is returned as is (exit 7);
// otherwise it sets connectionRefusedBeforeBody with host.busy{state: connection-refused |
// connection-reset} (exit 5, the retry rule). The exit comes from the reply's envelope, never its
// HTTP status; a body TryParseEnvelope rejects (or a reply past maxReplyBytes) is `internal` exit 1;
// no reply by the deadline is host.unreachable exit 7. EINTR is retried. Never half-closes the
// connection: the host reads a half-close on a parked call as a drop.
ExchangeResult LiveExchange(const std::string& requestBody, void* user); // user = LiveTarget*

// GET /remote/v1/status and GET /remote/v1/catalog over the same codec. HTTP 200 with a JSON
// body is that body; anything else fills *error (when non-null) and returns nullopt: the
// host's error envelope, host.unreachable (exit 7) when it cannot be reached, or internal.
std::optional<nlohmann::json> GetJson(const Arcane::Remote::Descriptor& desc, std::string_view target,
                                      std::uint32_t timeoutMs, Arcane::Remote::Envelope* error);

} // namespace Servitor

#endif
