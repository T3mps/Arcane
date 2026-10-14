#include <Servitor/Transport.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Remote/HttpClient.hpp>

#include <algorithm>
#include <chrono>
#include <span>
#include <utility>

namespace Servitor {

namespace {

using Clock = std::chrono::steady_clock;

ARC_CONSTANT("recv chunk size: one loopback read")
constexpr std::size_t kRecvChunkBytes = 16u * 1024u;
ARC_CONSTANT("send() takes an int length on Winsock: one call never passes more than 1 GiB")
constexpr std::size_t kMaxSendChunk = static_cast<std::size_t>(1) << 30;

std::int64_t NowUnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool SocketsReady()
{
    static const bool ready = Arcane::InitSockets();   // WSAStartup once per process (no-op on POSIX)
    return ready;
}

// Linux: no SIGPIPE from a send to a closed peer (MSG_NOSIGNAL). macOS sets
// SO_NOSIGPIPE on the socket instead; Windows has no SIGPIPE. The listener
// does the same (Listener.cpp SendFlags / NoSigPipe).
int SendFlags()
{
#if defined(__linux__)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void NoSigPipe(SocketType sock)
{
#if defined(__APPLE__)
    int one = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)sock;
#endif
}

// Every byte or false. The socket is still blocking here.
bool SendEvery(SocketType sock, const std::string& bytes)
{
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const std::size_t left = bytes.size() - sent;
        const int chunk = static_cast<int>(std::min(left, kMaxSendChunk));
        const auto n = ::send(sock, bytes.data() + sent, chunk, SendFlags());
        if (n <= 0)
            return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Closes on scope exit. Never shutdown(SEND) first: the host reads a
// half-close on a parked call as the client dropping it (rule from Task 6).
struct SocketGuard {
    SocketType sock = INVALID_SOCK;
    ~SocketGuard()
    {
        if (sock != INVALID_SOCK)
            CloseSocket(sock);
    }
};

enum class ReadEnd { Reply, Closed, Truncated, Malformed, TooLarge, Deadline, Observed };

struct ReadOutcome {
    ReadEnd end = ReadEnd::Deadline;
    Arcane::Remote::HttpReply reply;
    Arcane::Remote::Envelope observed;
};

// One recv on a non-blocking socket. >0 bytes appended, 0 EOF (or a reset),
// -1 nothing available yet.
int RecvSome(SocketType sock, std::string& buf)
{
    char chunk[kRecvChunkBytes];
    const auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
    if (n > 0) {
        buf.append(chunk, chunk + n);
        return static_cast<int>(n);
    }
    if (n == 0 || !Arcane::IsSocketTimeoutError())
        return 0;   // the peer closed, or the connection was reset
    return -1;
}

// What the bytes so far say: a reply, malformed, too large, or nullopt (more needed).
std::optional<ReadEnd> Judge(const std::string& buf, bool eof, ReadOutcome& out)
{
    if (buf.size() > kMaxReplyBytes)
        return ReadEnd::TooLarge;
    std::string why;
    if (auto reply = Arcane::Remote::ParseHttpReply(buf, eof, &why)) {
        out.reply = std::move(*reply);
        return ReadEnd::Reply;
    }
    if (why == "malformed")
        return ReadEnd::Malformed;
    if (eof)
        return buf.empty() ? ReadEnd::Closed : ReadEnd::Truncated;
    return std::nullopt;
}

// Reads one response: bounded by kMaxReplyBytes and by `deadline`; the host
// closes after every response, so EOF ends it too. With a target, every
// wake (a timeout included) samples ObserveTarget before the socket is read;
// a verdict still keeps a complete response that is already buffered.
ReadOutcome ReadReply(SocketType sock, Clock::time_point deadline, const LiveTarget* target)
{
    ReadOutcome out;
    std::string buf;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const int waitMs = static_cast<int>(std::clamp<long long>(left, 0, kWatchPollMs));
        Arcane::PollInterest interest;
        interest.socket = sock;
        interest.wantRead = true;
        const int rc = Arcane::PollSockets(std::span<Arcane::PollInterest>(&interest, 1), waitMs);

        if (target != nullptr) {
            if (auto verdict = ObserveTarget(target->watch, NowUnixMs())) {
                // Drain what is already there; a complete reply wins over the verdict.
                for (int n = RecvSome(sock, buf); n > 0 && buf.size() <= kMaxReplyBytes; n = RecvSome(sock, buf)) {
                }
                std::string why;
                if (buf.size() <= kMaxReplyBytes) {
                    if (auto reply = Arcane::Remote::ParseHttpReply(buf, false, &why)) {
                        out.reply = std::move(*reply);
                        out.end = ReadEnd::Reply;
                        return out;
                    }
                }
                out.observed = std::move(*verdict);
                out.end = ReadEnd::Observed;
                return out;
            }
        }

        if (rc > 0 && (interest.readable || interest.hungup)) {
            const int n = RecvSome(sock, buf);
            if (n != -1) {
                if (const auto end = Judge(buf, n == 0, out)) {
                    out.end = *end;
                    return out;
                }
            }
        }
        if (Clock::now() >= deadline) {
            out.end = ReadEnd::Deadline;
            return out;
        }
    }
}

void StampHost(Arcane::Remote::Envelope& env, const Arcane::Remote::Descriptor& desc)
{
    env.host.pid = desc.pid;
    env.host.kind = Arcane::Remote::ToString(desc.hostKind);
}

Arcane::Remote::Envelope Internal(const std::string& command, const Arcane::Remote::Descriptor& desc, int httpStatus)
{
    auto env = Arcane::Remote::MakeError(command, "internal", "malformed host reply",
                                         "the host's reply is not a remote envelope; rerun with --verbose and "
                                         "report the servitor and host versions");
    if (httpStatus != 0)
        env.errors.front().detail["httpStatus"] = httpStatus;
    StampHost(env, desc);
    return env;
}

Arcane::Remote::Envelope Unreachable(const std::string& command, const Arcane::Remote::Descriptor& desc,
                                     std::string message)
{
    auto env = Arcane::Remote::MakeError(command, "host.unreachable", std::move(message),
                                         "check the host with servitor status; relaunch it if it is gone");
    StampHost(env, desc);
    return env;
}

// Provably not run: the host never received the whole body. host.busy keeps
// the envelope's own exit (5) equal to the retry rule's.
Arcane::Remote::Envelope NotSent(const std::string& command, const Arcane::Remote::Descriptor& desc,
                                 const char* state, std::string message)
{
    auto env = Arcane::Remote::MakeError(command, "host.busy", std::move(message),
                                         "the request was not sent; retry, and check the host with servitor status "
                                         "if it persists");
    env.errors.front().detail["state"] = state;
    StampHost(env, desc);
    return env;
}

// The reply body as an envelope, whatever the HTTP status (host.busy arrives
// as 200 or 503, auth.denied as 401): the envelope decides the exit.
Arcane::Remote::Envelope EnvelopeOf(const std::string& command, const Arcane::Remote::Descriptor& desc,
                                    const Arcane::Remote::HttpReply& reply)
{
    const auto json = nlohmann::json::parse(reply.body, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded())
        return Internal(command, desc, reply.status);
    auto env = Arcane::Remote::TryParseEnvelope(json);
    if (!env)
        return Internal(command, desc, reply.status);
    return std::move(*env);
}

std::string Endpoint(const Arcane::Remote::Descriptor& desc)
{
    return "127.0.0.1:" + std::to_string(desc.port);
}

} // namespace

ExchangeResult LiveExchange(const std::string& requestBody, void* user)
{
    ExchangeResult result;
    auto* target = static_cast<LiveTarget*>(user);
    if (target == nullptr) {
        result.envelope = Arcane::Remote::MakeError("", "internal", "no host target for the exchange",
                                                    "this is a servitor bug; report it");
        return result;
    }
    const Arcane::Remote::Descriptor& desc = target->descriptor;
    const std::string& command = target->watch.command;

    // Taken once, when the call's first exchange starts (before the first
    // sample and before the body is written); a busy retry keeps it, so a
    // report written since the call began is still found.
    if (target->watch.requestUnixMs == 0)
        target->watch.requestUnixMs = NowUnixMs();
    if (auto verdict = ObserveTarget(target->watch, NowUnixMs())) {
        result.envelope = std::move(*verdict);
        return result;
    }

    if (!SocketsReady()) {
        result.envelope = Arcane::Remote::MakeError(command, "internal", "the socket layer did not start",
                                                    "check the machine's network stack");
        return result;
    }
    SocketGuard guard;
    guard.sock = Arcane::ConnectSocketWithTimeout(
        "127.0.0.1", desc.port, static_cast<int>(std::min(target->timeoutMs, kConnectTimeoutMs)));
    if (guard.sock == INVALID_SOCK) {
        result.connectionRefusedBeforeBody = true;
        result.envelope = NotSent(command, desc, "connection-refused",
                                  "could not connect to the host at " + Endpoint(desc));
        return result;
    }
    NoSigPipe(guard.sock);
    const std::string request = Arcane::Remote::BuildPost("/remote/v1/call", desc.port, desc.token, requestBody);
    if (!SendEvery(guard.sock, request)) {
        result.connectionRefusedBeforeBody = true;
        result.envelope = NotSent(command, desc, "connection-reset",
                                  "the host at " + Endpoint(desc) + " dropped the connection before the request was sent");
        return result;
    }
    if (!Arcane::SetNonBlocking(guard.sock, true)) {
        result.envelope = Unreachable(command, desc, "could not read from the host at " + Endpoint(desc));
        return result;
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(
                                             static_cast<std::int64_t>(target->timeoutMs) + kReplyGraceMs);
    ReadOutcome read = ReadReply(guard.sock, deadline, target);
    switch (read.end) {
        case ReadEnd::Reply:
            result.envelope = EnvelopeOf(command, desc, read.reply);
            break;
        case ReadEnd::Observed:
            result.envelope = std::move(read.observed);
            break;
        case ReadEnd::Malformed:
        case ReadEnd::TooLarge:
            result.envelope = Internal(command, desc, 0);
            break;
        case ReadEnd::Closed:
        case ReadEnd::Truncated:
            // A host that died or hung is named as such; otherwise it is unreachable.
            if (auto verdict = ObserveTarget(target->watch, NowUnixMs()))
                result.envelope = std::move(*verdict);
            else
                result.envelope = Unreachable(command, desc,
                                              read.end == ReadEnd::Closed
                                                  ? "the host at " + Endpoint(desc) + " closed the connection without replying"
                                                  : "the host at " + Endpoint(desc) + " closed the connection mid-reply");
            break;
        case ReadEnd::Deadline:
            result.envelope = Unreachable(command, desc,
                                          "no reply from the host at " + Endpoint(desc) + " within " +
                                              std::to_string(static_cast<std::int64_t>(target->timeoutMs) + kReplyGraceMs) +
                                              " ms");
            break;
    }
    return result;
}

std::optional<nlohmann::json> GetJson(const Arcane::Remote::Descriptor& desc, std::string_view target,
                                      std::uint32_t timeoutMs, Arcane::Remote::Envelope* error)
{
    const auto fail = [&](Arcane::Remote::Envelope env) -> std::optional<nlohmann::json> {
        if (error != nullptr)
            *error = std::move(env);
        return std::nullopt;
    };
    const std::string command;   // a GET is not a command; the caller names it when it prints
    if (!SocketsReady())
        return fail(Arcane::Remote::MakeError(command, "internal", "the socket layer did not start",
                                              "check the machine's network stack"));
    SocketGuard guard;
    guard.sock = Arcane::ConnectSocketWithTimeout("127.0.0.1", desc.port,
                                                  static_cast<int>(std::min(timeoutMs, kConnectTimeoutMs)));
    if (guard.sock == INVALID_SOCK)
        return fail(Unreachable(command, desc, "could not connect to the host at " + Endpoint(desc)));
    NoSigPipe(guard.sock);
    if (!SendEvery(guard.sock, Arcane::Remote::BuildGet(target, desc.port, desc.token)) ||
        !Arcane::SetNonBlocking(guard.sock, true))
        return fail(Unreachable(command, desc, "the host at " + Endpoint(desc) + " dropped the connection"));

    const ReadOutcome read = ReadReply(guard.sock, Clock::now() + std::chrono::milliseconds(timeoutMs), nullptr);
    switch (read.end) {
        case ReadEnd::Reply: {
            const auto json = nlohmann::json::parse(read.reply.body, nullptr, /*allow_exceptions=*/false);
            if (read.reply.status == 200 && !json.is_discarded())
                return json;
            if (!json.is_discarded()) {
                if (auto env = Arcane::Remote::TryParseEnvelope(json); env && !env->errors.empty())
                    return fail(std::move(*env));
            }
            return fail(Internal(command, desc, read.reply.status));
        }
        case ReadEnd::Malformed:
        case ReadEnd::TooLarge:
            return fail(Internal(command, desc, 0));
        case ReadEnd::Closed:
        case ReadEnd::Truncated:
            return fail(Unreachable(command, desc, "the host at " + Endpoint(desc) + " closed the connection without a reply"));
        case ReadEnd::Deadline:
        case ReadEnd::Observed:
            break;
    }
    return fail(Unreachable(command, desc,
                            "no reply from the host at " + Endpoint(desc) + " within " + std::to_string(timeoutMs) + " ms"));
}

} // namespace Servitor

#endif
