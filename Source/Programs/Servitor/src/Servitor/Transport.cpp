#include <Servitor/Transport.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Net/TcpSocket.hpp>
#include <Arcane/Remote/HttpClient.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <span>
#include <thread>
#include <utility>

namespace Servitor {

namespace {

using Clock = std::chrono::steady_clock;

ARC_CONSTANT("recv chunk size: one loopback read")
constexpr std::size_t kRecvChunkBytes = 16u * 1024u;
ARC_CONSTANT("send() takes an int length on Winsock: one call never passes more than 1 GiB")
constexpr std::size_t kMaxSendChunk = static_cast<std::size_t>(1) << 30;
ARC_CONSTANT("poll(2)/select failing for a reason other than EINTR: back off this long between tries")
constexpr int kPollErrorBackoffMs = 10;
ARC_CONSTANT("consecutive poll failures (not EINTR) before the connection counts as unreachable")
constexpr int kMaxPollErrors = 20;

bool SocketsReady()
{
    static const bool ready = Arcane::InitSockets();   // WSAStartup once per process (no-op on POSIX)
    return ready;
}

// The last socket call was interrupted by a signal (POSIX EINTR; the 130/143
// signal path makes it real): the call is retried, never read as a failure.
bool Interrupted()
{
#if defined(_WIN32)
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
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

int WaitMs(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<long long>(left, 0, kWatchPollMs));
}

// Every byte before `deadline`, or false. The socket is non-blocking: a full
// send buffer waits on PollSockets(write) in slices of at most kWatchPollMs.
bool SendUntil(SocketType sock, const std::string& bytes, Clock::time_point deadline)
{
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int chunk = static_cast<int>(std::min(bytes.size() - sent, kMaxSendChunk));
        const auto n = ::send(sock, bytes.data() + sent, chunk, SendFlags());
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && Interrupted())
            continue;
        if (n == 0 || !Arcane::IsSocketTimeoutError())
            return false;   // reset or refused mid-send
        if (Clock::now() >= deadline)
            return false;
        Arcane::PollInterest interest;
        interest.socket = sock;
        interest.wantWrite = true;
        if (Arcane::PollSockets(std::span<Arcane::PollInterest>(&interest, 1), WaitMs(deadline)) < 0 &&
            !Interrupted())
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollErrorBackoffMs));
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

enum class ReadEnd { Reply, Closed, Truncated, Malformed, TooLarge, Deadline, PollFailed, Observed };

struct ReadOutcome {
    ReadEnd end = ReadEnd::Deadline;
    Arcane::Remote::HttpReply reply;
    Arcane::Remote::Envelope observed;
};

// One recv on a non-blocking socket, EINTR retried. >0 bytes appended, 0 EOF
// (or a reset), -1 nothing available yet.
int RecvSome(SocketType sock, std::string& buf)
{
    char chunk[kRecvChunkBytes];
    for (;;) {
        const auto n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
        if (n > 0) {
            buf.append(chunk, chunk + n);
            return static_cast<int>(n);
        }
        if (n < 0 && Interrupted())
            continue;
        if (n == 0 || !Arcane::IsSocketTimeoutError())
            return 0;   // the peer closed, or the connection was reset
        return -1;
    }
}

// What the bytes so far say: a reply, malformed, too large, or nullopt (more needed).
std::optional<ReadEnd> Judge(const std::string& buf, bool eof, std::size_t maxBytes, ReadOutcome& out)
{
    if (buf.size() > maxBytes)
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

// Reads one response: bounded by maxBytes and by `deadline`; the host closes
// after every response, so EOF ends it too. With a target, every wake (a
// timeout included) samples its watch before the socket is read; a verdict
// still keeps a complete response that is already buffered.
ReadOutcome ReadReply(SocketType sock, Clock::time_point deadline, std::size_t maxBytes, const LiveTarget* target)
{
    ReadOutcome out;
    std::string buf;
    int pollErrors = 0;
    for (;;) {
        Arcane::PollInterest interest;
        interest.socket = sock;
        interest.wantRead = true;
        const int rc = Arcane::PollSockets(std::span<Arcane::PollInterest>(&interest, 1), WaitMs(deadline));
        const bool interrupted = rc < 0 && Interrupted();

        if (target != nullptr && target->observe != nullptr) {
            if (auto verdict = target->observe(target->watch, NowUnixMs())) {
                // Drain what is already there; a complete reply wins over the verdict.
                bool eof = false;
                for (;;) {
                    const int n = RecvSome(sock, buf);
                    if (n == 0)
                        eof = true;
                    if (n <= 0 || buf.size() > maxBytes)
                        break;
                }
                std::string why;
                if (buf.size() <= maxBytes) {
                    if (auto reply = Arcane::Remote::ParseHttpReply(buf, eof, &why)) {
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

        if (rc < 0 && !interrupted) {
            // Not a signal: back off instead of spinning, and give up when it persists.
            if (++pollErrors >= kMaxPollErrors) {
                out.end = ReadEnd::PollFailed;
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollErrorBackoffMs));
        } else if (rc >= 0) {
            pollErrors = 0;
        }
        if (rc > 0 && (interest.readable || interest.hungup)) {
            const int n = RecvSome(sock, buf);
            if (n != -1) {
                if (const auto end = Judge(buf, n == 0, maxBytes, out)) {
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

Arcane::Remote::Envelope Internal(const std::string& command, const Arcane::Remote::Descriptor& desc, int httpStatus,
                                  const char* reason)
{
    auto env = Arcane::Remote::MakeError(command, "internal", "malformed host reply",
                                         "the host's reply is not a remote envelope; rerun with --verbose and "
                                         "report the servitor and host versions");
    auto& detail = env.errors.front().detail;
    detail["reason"] = reason;
    if (httpStatus != 0)
        detail["httpStatus"] = httpStatus;
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
        return Internal(command, desc, reply.status, "not-json");
    auto env = Arcane::Remote::TryParseEnvelope(json);
    if (!env)
        return Internal(command, desc, reply.status, "not-an-envelope");
    return std::move(*env);
}

std::string Endpoint(const Arcane::Remote::Descriptor& desc)
{
    return "127.0.0.1:" + std::to_string(desc.port);
}

// The envelope one read ended in.
Arcane::Remote::Envelope FromReadEnd(ReadOutcome& read, const std::string& command,
                                     const Arcane::Remote::Descriptor& desc, long long waitedMs)
{
    switch (read.end) {
        case ReadEnd::Reply:
            return EnvelopeOf(command, desc, read.reply);
        case ReadEnd::Observed:
            return std::move(read.observed);
        case ReadEnd::Malformed:
            return Internal(command, desc, 0, "malformed-http");
        case ReadEnd::TooLarge:
            return Internal(command, desc, 0, "too-large");
        case ReadEnd::Closed:
            return Unreachable(command, desc, "the host at " + Endpoint(desc) + " closed the connection without replying");
        case ReadEnd::Truncated:
            return Unreachable(command, desc, "the host at " + Endpoint(desc) + " closed the connection mid-reply");
        case ReadEnd::PollFailed:
            return Unreachable(command, desc, "waiting on the connection to " + Endpoint(desc) + " kept failing");
        case ReadEnd::Deadline:
            break;
    }
    return Unreachable(command, desc,
                       "no reply from the host at " + Endpoint(desc) + " within " + std::to_string(waitedMs) + " ms");
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
    const auto observe = [&]() -> std::optional<Arcane::Remote::Envelope> {
        if (target->observe == nullptr)
            return std::nullopt;
        return target->observe(target->watch, NowUnixMs());
    };

    // Stamped once, when the call's first exchange starts (before the first
    // sample and before the body is written). A busy retry keeps both: a
    // report written since the call began is still found, and every attempt
    // shares the one --timeout budget.
    if (target->watch.requestUnixMs == 0)
        target->watch.requestUnixMs = NowUnixMs();
    if (target->started == Clock::time_point{})
        target->started = Clock::now();
    const Clock::time_point budgetEnd = target->started + std::chrono::milliseconds(target->timeoutMs);
    const Clock::time_point deadline = budgetEnd + std::chrono::milliseconds(target->replyGraceMs);

    if (auto verdict = observe()) {
        result.envelope = std::move(*verdict);
        return result;
    }
    // Not sent. A host that died or hung meanwhile is named (exit 7; a refused
    // loopback connect alone takes ~2 s on Windows); otherwise the answer is
    // provably-not-run and retryable (exit 5).
    const auto notSent = [&](const char* state, std::string message) {
        if (auto verdict = observe()) {
            result.envelope = std::move(*verdict);
            return result;
        }
        result.connectionRefusedBeforeBody = true;
        result.envelope = NotSent(command, desc, state, std::move(message));
        return result;
    };

    if (!SocketsReady()) {
        result.envelope = Arcane::Remote::MakeError(command, "internal", "the socket layer did not start",
                                                    "check the machine's network stack");
        return result;
    }
    const auto budgetLeft = std::chrono::duration_cast<std::chrono::milliseconds>(budgetEnd - Clock::now()).count();
    const int connectMs = static_cast<int>(std::clamp<long long>(budgetLeft, 1, kConnectTimeoutMs));
    SocketGuard guard;
    guard.sock = Arcane::ConnectSocketWithTimeout("127.0.0.1", desc.port, connectMs);
    if (guard.sock == INVALID_SOCK)
        return notSent("connection-refused", "could not connect to the host at " + Endpoint(desc));
    NoSigPipe(guard.sock);
    const std::string request = Arcane::Remote::BuildPost("/remote/v1/call", desc.port, desc.token, requestBody);
    if (!Arcane::SetNonBlocking(guard.sock, true) || !SendUntil(guard.sock, request, deadline))
        return notSent("connection-reset",
                       "the host at " + Endpoint(desc) + " did not take the whole request; it was not run");

    ReadOutcome read = ReadReply(guard.sock, deadline, target->maxReplyBytes, target);
    if (read.end == ReadEnd::Closed || read.end == ReadEnd::Truncated) {
        // A host that died or hung is named as such; otherwise it is unreachable.
        if (auto verdict = observe()) {
            result.envelope = std::move(*verdict);
            return result;
        }
    }
    result.envelope = FromReadEnd(read, command, desc,
                                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - target->started).count());
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
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    SocketGuard guard;
    guard.sock = Arcane::ConnectSocketWithTimeout(
        "127.0.0.1", desc.port, static_cast<int>(std::clamp<std::uint32_t>(timeoutMs, 1, kConnectTimeoutMs)));
    if (guard.sock == INVALID_SOCK)
        return fail(Unreachable(command, desc, "could not connect to the host at " + Endpoint(desc)));
    NoSigPipe(guard.sock);
    if (!Arcane::SetNonBlocking(guard.sock, true) ||
        !SendUntil(guard.sock, Arcane::Remote::BuildGet(target, desc.port, desc.token), deadline))
        return fail(Unreachable(command, desc, "the host at " + Endpoint(desc) + " dropped the connection"));

    ReadOutcome read = ReadReply(guard.sock, deadline, kMaxReplyBytes, nullptr);
    if (read.end == ReadEnd::Reply) {
        const auto json = nlohmann::json::parse(read.reply.body, nullptr, /*allow_exceptions=*/false);
        if (read.reply.status == 200 && !json.is_discarded())
            return json;
        if (!json.is_discarded()) {
            if (auto env = Arcane::Remote::TryParseEnvelope(json); env && !env->errors.empty())
                return fail(std::move(*env));
        }
        return fail(Internal(command, desc, read.reply.status, json.is_discarded() ? "not-json" : "not-an-envelope"));
    }
    return fail(FromReadEnd(read, command, desc, timeoutMs));
}

} // namespace Servitor

#endif
