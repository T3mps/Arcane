#pragma once
#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include "RemoteTestSupport.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace RemoteTest {

// A blocking TCP socket connected to 127.0.0.1:port. REQUIREs success.
inline SocketType ConnectLoopback(std::uint16_t port) {
    REQUIRE(Arcane::InitSockets());
    const SocketType sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return sock;
}

// Sends every byte. REQUIREs one full send (requests here are small).
inline void SendAll(SocketType sock, const std::string& bytes) {
    REQUIRE(static_cast<int>(::send(sock, bytes.data(), static_cast<int>(bytes.size()), 0)) ==
            static_cast<int>(bytes.size()));
}

// Reads until `marker` is in the accumulated bytes, the peer closes, or
// `budget` passes. Each recv waits at most 100 ms, so the loop never overruns
// the budget by more than that. Returns everything read.
inline std::string RecvUntil(SocketType sock, const std::string& marker, std::chrono::milliseconds budget) {
    Arcane::SetSocketTimeout(sock, 100);
    std::string raw;
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (raw.find(marker) == std::string::npos && std::chrono::steady_clock::now() < deadline) {
        const int n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (n > 0) {
            raw.append(buf, buf + n);
            continue;
        }
        if (n == 0 || !Arcane::IsSocketTimeoutError())
            break; // closed, or a hard error
    }
    return raw;
}

// One request/response on a fresh connection. `extraHeaders` is zero or more
// complete "Name: value\r\n" lines. Returns the raw HTTP response.
inline std::string HttpExchangeHost(std::uint16_t port, const std::string& token,
                                    const std::string& hostHeader, const std::string& extraHeaders,
                                    const std::string& requestLine, const std::string& body = {}) {
    const SocketType sock = ConnectLoopback(port);
    Arcane::SetSocketTimeout(sock, 5000); // a listener that never answers fails the case, not the suite
    std::string req = requestLine + "\r\nHost: " + hostHeader +
        "\r\nAuthorization: Bearer " + token + "\r\n";
    if (!body.empty()) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += extraHeaders;
    req += "\r\n";
    req += body;
    SendAll(sock, req);
    std::string raw;
    char buf[4096];
    for (;;) {
        const int n = static_cast<int>(::recv(sock, buf, sizeof(buf), 0));
        if (n <= 0) break;
        raw.append(buf, buf + n);
        const auto hdr = raw.find("\r\n\r\n");
        if (hdr == std::string::npos) continue;
        const auto key = raw.find("Content-Length:");
        if (key == std::string::npos || key > hdr) break;
        const int len = std::atoi(raw.c_str() + key + 15);
        if (static_cast<int>(raw.size() - (hdr + 4)) >= len) break;
    }
    CloseSocket(sock);
    return raw;
}

inline std::string HttpExchange(std::uint16_t port, const std::string& token,
                                const std::string& extraHeaders, const std::string& requestLine,
                                const std::string& body = {}) {
    return HttpExchangeHost(port, token, "127.0.0.1:" + std::to_string(port),
                            extraHeaders, requestLine, body);
}

}  // namespace RemoteTest
#endif
