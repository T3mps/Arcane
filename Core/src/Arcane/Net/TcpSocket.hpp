#pragma once

// Shared TCP socket utilities.
// Extracted from Server.hpp socket helpers.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
// Audit M-V5-10 networking (2026-06-03): mstcpip.h provides
// `tcp_keepalive` + `SIO_KEEPALIVE_VALS` consumed by EnableTcpKeepAlive
// below. Pull explicitly -- WS2tcpip.h does NOT include it transitively
// on the v143 Windows SDK.
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CloseSocket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
using SocketType = int;
#define INVALID_SOCK -1
#define CloseSocket close
#endif

#include <cstdint>
#include <string>

namespace Arcane
{
    // ============================================================================
    // Configuration Constants
    // ============================================================================

    namespace ServerConfig
    {
        constexpr size_t MAX_PAYLOAD_SIZE = 8192;
        constexpr size_t MAX_RECEIVE_BUFFER_SIZE = 65536;
        constexpr size_t RECV_CHUNK_SIZE = 4096;
        constexpr int RECV_TIMEOUT_MS = 100;

        // Audit H-V4-10 (2026-06-03): connection caps for the public-facing
        // TcpServerBase accept loop. One thread per connection at ~1.1MB
        // stack means 1000 slow-loris IPs cost ~1.1GB + 1000 OS threads.
        // Defaults sized for our DAU target with headroom; trip an error
        // log when the cap fires so ops can spot the attack signature.
        //
        // Audit M-V5-6 networking (2026-06-04): these are now the fallback
        // defaults. The runtime values live in TcpServerBase's
        // m_maxConnPerIp / m_maxConnTotal, populated from
        // protocol.json's settings.max_connections_per_ip /
        // settings.max_connections_total at startup (see each consuming
        // service's main.cpp). A protocol.json without the keys uses these
        // constants -- no behavior change at default values.
        //
        // Topology assumptions baked into the per-IP cap:
        //   - No reverse proxy or TLS-terminating front (Nginx, Envoy,
        //     CloudFront). All 100% of accept calls would otherwise
        //     return the proxy's IP and the cap would immediately fire
        //     for every user.
        //   - No CDN edge in front of the TCP listener.
        //   - No PROXY-protocol v1/v2 framing on the inbound connection.
        // Upgrade path when a TLS-terminating front lands: parse
        // PROXY-protocol v1 in the accept handshake BEFORE perIpCount
        // is incremented; the `clientIP` variable in TcpServerBase
        // becomes the X-Forwarded-For / PROXY-protocol src address,
        // not the immediate peer. Deferred until the launch topology
        // is chosen (per spec 2026-06-04, Scope 1 out-of-scope notes).
        constexpr size_t MAX_CONNECTIONS_TOTAL  = 2048;
        constexpr size_t MAX_CONNECTIONS_PER_IP = 16;
    }

    // ============================================================================
    // Socket Helpers
    // ============================================================================

    inline void SetSocketTimeout(SocketType socket, int timeoutMs)
    {
#ifdef _WIN32
        DWORD timeout = timeoutMs;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    // Audit M-V5-10 networking (2026-06-03): enable TCP keepalive on an
    // accepted client socket. Without this, half-open connections from
    // crashed clients (TCP RST never delivered -- wifi blackout, laptop
    // suspend, process kill on a network with reverse-path filtering)
    // hold a connection-cap slot for the OS-default keepalive idle (2h
    // on Windows, 7200s on Linux). With H-V4-10's 16/IP cap, 16 stale
    // half-open conns from one CGNAT egress lock out new legit users
    // for hours.
    //
    // Parameters: probe after `idleSeconds` of no traffic, retry every
    // `intervalSeconds`, drop the connection after `probeCount` failures.
    // Defaults sized for "tolerate flaky mobile/wifi (occasional 30s
    // dropouts are normal) but catch a true dead peer within ~5min."
    inline void EnableTcpKeepAlive(SocketType socket,
                                   int idleSeconds     = 120,
                                   int intervalSeconds = 30,
                                   int probeCount      = 8)
    {
#ifdef _WIN32
        BOOL on = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&on, sizeof(on));
        // SIO_KEEPALIVE_VALS lets us pick the idle + interval (in ms).
        // Windows doesn't expose probeCount via this ioctl -- the stack
        // uses TcpMaxDataRetransmissions from the registry (default 5).
        // probeCount is therefore advisory on Windows; the timing curve
        // is dominated by idleMs + N*intervalMs anyway.
        struct tcp_keepalive ka{};
        ka.onoff             = 1;
        ka.keepalivetime     = static_cast<ULONG>(idleSeconds) * 1000;
        ka.keepaliveinterval = static_cast<ULONG>(intervalSeconds) * 1000;
        DWORD bytesReturned = 0;
        WSAIoctl(socket, SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
                 nullptr, 0, &bytesReturned, nullptr, nullptr);
        (void)probeCount;
#else
        int on = 1;
        setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    #ifdef TCP_KEEPIDLE
        setsockopt(socket, IPPROTO_TCP, TCP_KEEPIDLE, &idleSeconds, sizeof(idleSeconds));
    #endif
    #ifdef TCP_KEEPINTVL
        setsockopt(socket, IPPROTO_TCP, TCP_KEEPINTVL, &intervalSeconds, sizeof(intervalSeconds));
    #endif
    #ifdef TCP_KEEPCNT
        setsockopt(socket, IPPROTO_TCP, TCP_KEEPCNT, &probeCount, sizeof(probeCount));
    #endif
#endif
    }

    inline bool IsSocketTimeoutError()
    {
#ifdef _WIN32
        int err = WSAGetLastError();
        return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    }

    inline bool IsDisconnectError()
    {
#ifdef _WIN32
        int err = WSAGetLastError();
        return err == WSAECONNRESET || err == WSAECONNABORTED ||
            err == WSAENETRESET || err == WSAESHUTDOWN ||
            err == ERROR_IO_PENDING;
#else
        return errno == ECONNRESET || errno == ECONNABORTED ||
            errno == EPIPE || errno == ENOTCONN;
#endif
    }

    // Send all bytes, looping until complete
    inline bool SendAll(SocketType socket, const char* data, int len)
    {
        int totalSent = 0;
        while (totalSent < len)
        {
            int sent = send(socket, data + totalSent, len - totalSent, 0);
            if (sent <= 0)
                return false;
            totalSent += sent;
        }
        return true;
    }

    // Initialize Winsock (call once per process)
    inline bool InitSockets()
    {
#ifdef _WIN32
        WSADATA wsaData;
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
        return true;
#endif
    }

    inline void CleanupSockets()
    {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    // Create a TCP listening socket on the given port and optional bind address
    inline SocketType CreateListenSocket(uint16_t port, const char* bindAddr = nullptr)
    {
        SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCK)
            return INVALID_SOCK;

        int opt = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        if (bindAddr)
            inet_pton(AF_INET, bindAddr, &addr.sin_addr);
        else
            addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }

        if (listen(sock, 10) < 0)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }

        return sock;
    }

    // Connect to a remote TCP endpoint. BLOCKS until the OS-level connect
    // timeout fires (~21s on Windows for an unresponsive remote host)
    // -- acceptable for loopback / fast-network paths, NOT acceptable for
    // ServiceClient which holds m_mutex across the call. Production
    // ServiceClient code uses ConnectSocketWithTimeout below.
    inline SocketType ConnectSocket(const std::string& host, uint16_t port)
    {
        SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCK)
            return INVALID_SOCK;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }

        return sock;
    }

    // Audit M-V5-9 networking (2026-06-03): connect with a bounded
    // timeout via non-blocking connect + select. The blocking
    // ConnectSocket above holds for the OS-level SYN timeout -- ~21s
    // on Windows, ~127s with default tcp_syn_retries=6 on Linux --
    // when the remote silently drops SYNs (firewall blackhole,
    // saturated accept queue, dead but-not-disconnected peer).
    // ServiceClient holds m_mutex across this call, so the unbounded
    // wait collapses the RPC channel to a single-caller-at-a-time
    // contract for up to a minute. With H-V4-4's 30s Debug recv
    // timeout stacked on top, worst-case Debug Call() blocks 51s
    // serially. Capping connect at ~3s keeps the mutex-hold ceiling
    // at handler-runtime + 3s.
    //
    // Returns INVALID_SOCK on:
    //   - socket()/inet_pton failure
    //   - connect() returning an error that's not WSAEWOULDBLOCK /
    //     EINPROGRESS (immediate refusal -- connection refused is
    //     fast on loopback so this branch fires routinely there)
    //   - select() timing out before the socket becomes writable
    //   - SO_ERROR != 0 after the socket becomes writable (asynchronous
    //     connect failure -- e.g., connection refused arriving after
    //     the SYN-pending state)
    // On success, returns a connected socket in BLOCKING mode (we
    // restore the pre-call flags before returning so the caller's
    // subsequent send/recv semantics are unchanged).
    inline SocketType ConnectSocketWithTimeout(const std::string& host,
                                               uint16_t port,
                                               int timeoutMs)
    {
        SocketType sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCK)
            return INVALID_SOCK;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }

        // Flip to non-blocking. We capture the pre-call flags on POSIX
        // so we can restore them; on Windows there's just on/off.
#ifdef _WIN32
        u_long nonblock = 1;
        if (ioctlsocket(sock, FIONBIO, &nonblock) != 0)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }
#else
        const int origFlags = fcntl(sock, F_GETFL, 0);
        if (origFlags == -1 || fcntl(sock, F_SETFL, origFlags | O_NONBLOCK) == -1)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }
#endif

        // Kick off the connect. EWOULDBLOCK / EINPROGRESS means "pending";
        // any other error is terminal. Note: a successful connect on
        // loopback may return 0 here (the kernel completes synchronously),
        // in which case we skip the select and go straight to blocking-
        // mode restoration.
        bool connectPending = false;
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
#ifdef _WIN32
            const int err = WSAGetLastError();
            connectPending = (err == WSAEWOULDBLOCK);
#else
            connectPending = (errno == EINPROGRESS);
#endif
            if (!connectPending)
            {
                CloseSocket(sock);
                return INVALID_SOCK;
            }
        }

        if (connectPending)
        {
            fd_set writeSet, errSet;
            FD_ZERO(&writeSet); FD_SET(sock, &writeSet);
            FD_ZERO(&errSet);   FD_SET(sock, &errSet);

            struct timeval tv;
            tv.tv_sec  = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;

            // select's nfds is ignored on Windows; on POSIX it must be
            // > max(fd). Pass sock+1 unconditionally.
            const int selRc = select(static_cast<int>(sock) + 1,
                                     nullptr, &writeSet, &errSet, &tv);
            if (selRc <= 0)
            {
                // 0 = timeout, -1 = error
                CloseSocket(sock);
                return INVALID_SOCK;
            }

            // Connect can fail asynchronously -- the socket becomes
            // writable but SO_ERROR carries the error code. Always
            // check; a writable result alone does NOT imply success.
            int soError = 0;
#ifdef _WIN32
            int optLen = sizeof(soError);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&soError), &optLen) != 0
                || soError != 0)
            {
                CloseSocket(sock);
                return INVALID_SOCK;
            }
#else
            socklen_t optLen = sizeof(soError);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soError, &optLen) != 0
                || soError != 0)
            {
                CloseSocket(sock);
                return INVALID_SOCK;
            }
#endif
        }

        // Restore blocking mode. Caller expects the usual blocking
        // send/recv semantics; the SetSocketTimeout call sites still
        // own the recv side via SO_RCVTIMEO.
#ifdef _WIN32
        u_long block = 0;
        if (ioctlsocket(sock, FIONBIO, &block) != 0)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }
#else
        if (fcntl(sock, F_SETFL, origFlags) == -1)
        {
            CloseSocket(sock);
            return INVALID_SOCK;
        }
#endif

        return sock;
    }

    // ============================================================================
    // Length-prefixed frame parser (shared by MessageParser and RpcParser)
    //
    // Wire format: LENGTH:BODY\n
    // ============================================================================

    struct LengthFrameResult
    {
        bool        needMoreData = false;
        bool        error        = false;
        std::string body;      // populated on success; excludes the trailing '\n'
        size_t      consumed = 0; // bytes to erase from the caller's buffer on success
    };

    // Parse one LENGTH:BODY\n frame from buffer without modifying it.
    // On success, body contains the raw payload and consumed is the total bytes to discard.
    inline LengthFrameResult ExtractLengthFramed(const std::string& buffer,
                                                  size_t maxBodySize = ServerConfig::MAX_RECEIVE_BUFFER_SIZE)
    {
        LengthFrameResult r;
        if (buffer.empty()) { r.needMoreData = true; return r; }

        size_t colonPos = buffer.find(':');
        if (colonPos == std::string::npos)
        {
            r.needMoreData = (buffer.size() <= 10);
            r.error        = !r.needMoreData;
            return r;
        }

        // Audit M-V4-5 networking (2026-06-03): std::stoull is a partial
        // parse -- it would happily return 12 from "12abc" and leave "abc"
        // unconsumed. With the colon search above, that means a prefix
        // like "12abc:body" would be treated as length 12 with no error.
        // Capture the parsed-char count via the `pos` out-param and
        // require it to consume the entire pre-colon substring so the
        // length prefix must be all-numeric.
        size_t expectedLen = 0;
        std::size_t parsedChars = 0;
        try { expectedLen = std::stoull(buffer.substr(0, colonPos), &parsedChars); }
        catch (...) { r.error = true; return r; }
        if (parsedChars != colonPos) { r.error = true; return r; }

        if (expectedLen > maxBodySize) { r.error = true; return r; }

        size_t bodyStart = colonPos + 1;
        size_t totalLen  = bodyStart + expectedLen + 1; // +1 for '\n'
        if (buffer.size() < totalLen) { r.needMoreData = true; return r; }

        // Audit E01-3d networking: the length prefix promises exactly
        // expectedLen body bytes followed by a '\n' terminator. Verify the
        // terminator is actually present at that offset before consuming.
        // buffer.size() >= totalLen guarantees the index is valid. A mismatch
        // means the declared length disagrees with the on-wire framing (stream
        // desync, or a crafted length that would slice the frame on the wrong
        // boundary) -- flag it as an error instead of silently accepting a
        // mis-terminated frame and resyncing mid-message.
        if (buffer[bodyStart + expectedLen] != '\n') { r.error = true; return r; }

        r.body     = buffer.substr(bodyStart, expectedLen);
        r.consumed = totalLen;
        return r;
    }

} // namespace Arcane
