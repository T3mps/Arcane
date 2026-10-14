#pragma once
// Servitor's live transport (plan rev 8.3 V3): one HTTP/1.1 exchange per call
// with a host's loopback listener, over ArcaneNet's TcpSocket.hpp and
// ArcaneRemoteProtocol's HttpClient.hpp. Program code: no export macro (V4).
#include <Servitor/Client.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
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

struct LiveTarget {
    Arcane::Remote::Descriptor descriptor;
    TargetWatch watch;
    std::uint32_t timeoutMs = 30000;   // the call's options.timeoutMs; the read gives up kReplyGraceMs later
};

// Connects to 127.0.0.1:desc.port (ConnectSocketWithTimeout), sends BuildPost("/remote/v1/call", ...),
// then loops PollSockets(250 ms) -> recv -> ParseHttpReply until a reply or a watch verdict.
// Every wake, including a timeout, samples ObserveTarget(watch, now) first (Task 14 Target watch);
// the first sample runs before the connect. A connect refused, or a send that fails, before the
// whole body was sent sets connectionRefusedBeforeBody with a host.busy envelope (exit 5, the
// retry rule). The exit comes from the reply's envelope, never its HTTP status; a body
// TryParseEnvelope rejects (or a reply past kMaxReplyBytes) is `internal` exit 1. Never
// half-closes the connection: the host reads a half-close on a parked call as a drop.
ExchangeResult LiveExchange(const std::string& requestBody, void* user); // user = LiveTarget*

// GET /remote/v1/status and GET /remote/v1/catalog over the same codec. HTTP 200 with a JSON
// body is that body; anything else fills *error (when non-null) and returns nullopt: the
// host's error envelope, host.unreachable (exit 7) when it cannot be reached, or internal.
std::optional<nlohmann::json> GetJson(const Arcane::Remote::Descriptor& desc, std::string_view target,
                                      std::uint32_t timeoutMs, Arcane::Remote::Envelope* error);

} // namespace Servitor

#endif
