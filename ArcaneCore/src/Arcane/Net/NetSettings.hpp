#pragma once

// net.*, net.keepalive.*, net.rateLimit.* (settings arc S6-11; inventory
// Part 1 "Net (Server)"): the TCP and rate-limit tunables of the header-only
// Net layer (TcpSocket.hpp, RateLimiter.hpp, Protocol.hpp) and of the services
// built on it.
// - Server audience, Project scope. The registration lives in NetSettings.cpp
//   (ArcaneCore.dll), which every consumer of these headers links.
// - The defaults are the pre-sweep literals (the retired ServerConfig::MAX_*
//   constants, EnableTcpKeepAlive's default arguments, listen()'s backlog and
//   RateLimiter's MAX_RECORDS / Config / cleanup cadence); SweepNetTest pins them.
// - ServerConfig::RECV_CHUNK_SIZE stays a marked CONSTANT in TcpSocket.hpp: it
//   is a stack-array bound in the services, not a preference.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    struct NetSettings
    {
        std::uint64_t maxPayloadBytes       = 8192;    // one message payload (B)
        std::uint64_t maxReceiveBufferBytes = 65536;   // per-connection receive buffer, and the largest length-framed body (B)
        std::int32_t  recvTimeoutMs         = 100;     // SO_RCVTIMEO on a connection socket (ms)
        std::uint64_t maxConnectionsTotal   = 2048;    // open public connections, all peers
        // Audit H-V4-10 (2026-06-03): connection caps for the public-facing
        // TcpServerBase accept loop. One thread per connection at ~1.1MB
        // stack means 1000 slow-loris IPs cost ~1.1GB + 1000 OS threads.
        // Defaults sized for our DAU target with headroom; trip an error
        // log when the cap fires so ops can spot the attack signature.
        //
        // Audit M-V5-6 networking (2026-06-04): these are the fallback
        // defaults. The runtime values live in TcpServerBase's
        // m_maxConnPerIp / m_maxConnTotal, populated from
        // protocol.json's settings.max_connections_per_ip /
        // settings.max_connections_total at startup (see each consuming
        // service's main.cpp). A protocol.json without the keys uses these
        // settings -- no behavior change at default values.
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
        std::uint64_t maxConnectionsPerIp   = 16;      // open public connections from one peer address
        std::int32_t  listenBacklog         = 10;      // listen() backlog of a listening socket
        std::string   protocolPath          = "data/protocol.json";   // ProtocolLoader::Load()'s file
    };

    struct NetKeepaliveSettings
    {
        // Audit M-V5-10 networking (2026-06-03): sized for "tolerate flaky
        // mobile/wifi (occasional 30s dropouts are normal) but catch a true
        // dead peer within ~5min."
        std::int32_t idleSeconds     = 120;   // no traffic for this long before the first probe (s)
        std::int32_t intervalSeconds = 30;    // between probes (s)
        std::int32_t probeCount      = 8;     // failed probes before the drop (advisory on Windows)
    };

    struct NetRateLimitSettings
    {
        std::uint64_t maxRecords        = 10000;   // LRU cap on tracked keys
        std::int32_t  maxAttempts       = 5;       // attempts allowed inside one window
        std::int32_t  windowSeconds     = 60;      // the counting window (s)
        std::int32_t  cooldownSeconds   = 30;      // lock-out after the limit trips (s)
        std::int32_t  cleanupEvery      = 100;     // Allow() calls between idle sweeps
        std::int32_t  idleExpiryMinutes = 10;      // an untouched record is dropped after this (min)
    };

    ARC_REFLECT_TYPE(NetSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "net", SettingScope::Project, ApplyMode::Restart, Audience::Server)
        ARC_REFLECT_FIELD(NetSettings, maxPayloadBytes)
            ARC_REFLECT_ATTR(Range, 1024.0, 1048576.0)
            ARC_REFLECT_ATTR(Tooltip, "Largest message payload a service accepts, in bytes.")
        ARC_REFLECT_FIELD(NetSettings, maxReceiveBufferBytes)
            ARC_REFLECT_ATTR(Range, 4096.0, 16777216.0)
            ARC_REFLECT_ATTR(Tooltip, "Per-connection receive buffer, and the largest length-framed body, in bytes. A peer that overruns it is dropped.")
        ARC_REFLECT_FIELD(NetSettings, recvTimeoutMs)
            ARC_REFLECT_ATTR(Range, 1.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Receive timeout on a connection socket, in milliseconds.")
        ARC_REFLECT_FIELD(NetSettings, maxConnectionsTotal)
            ARC_REFLECT_ATTR(Range, 1.0, 1000000.0)
            ARC_REFLECT_ATTR(Tooltip, "Most open public connections a service holds. protocol.json's max_connections_total overrides it.")
        ARC_REFLECT_FIELD(NetSettings, maxConnectionsPerIp)
            ARC_REFLECT_ATTR(Range, 1.0, 10000.0)
            ARC_REFLECT_ATTR(Tooltip, "Most open public connections from one address. Assumes no proxy or CDN in front of the listener; "
                                      "protocol.json's max_connections_per_ip overrides it.")
        ARC_REFLECT_FIELD(NetSettings, listenBacklog)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 65535.0)
            ARC_REFLECT_ATTR(Tooltip, "Pending-connection backlog of a listening socket.")
        ARC_REFLECT_FIELD(NetSettings, protocolPath)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Widget, "path:file")
            ARC_REFLECT_ATTR(Tooltip, "The protocol definition the services load, relative to the working directory.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(NetKeepaliveSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "net.keepalive", SettingScope::Project, ApplyMode::Restart, Audience::Server)
        ARC_REFLECT_FIELD(NetKeepaliveSettings, idleSeconds)
            ARC_REFLECT_ATTR(Range, 1.0, 7200.0)
            ARC_REFLECT_ATTR(Tooltip, "Seconds without traffic before TCP keepalive probes an accepted connection.")
        ARC_REFLECT_FIELD(NetKeepaliveSettings, intervalSeconds)
            ARC_REFLECT_ATTR(Range, 1.0, 600.0)
            ARC_REFLECT_ATTR(Tooltip, "Seconds between TCP keepalive probes.")
        ARC_REFLECT_FIELD(NetKeepaliveSettings, probeCount)
            ARC_REFLECT_ATTR(Range, 1.0, 32.0)
            ARC_REFLECT_ATTR(Tooltip, "Failed keepalive probes before the connection drops (Windows uses its own retransmission count).")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(NetRateLimitSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "net.rateLimit", SettingScope::Project, ApplyMode::Restart, Audience::Server)
        ARC_REFLECT_FIELD(NetRateLimitSettings, maxRecords)
            ARC_REFLECT_ATTR(Range, 100.0, 10000000.0)
            ARC_REFLECT_ATTR(Tooltip, "Most keys a rate limiter tracks; the least recently allowed key is evicted past it.")
        ARC_REFLECT_FIELD(NetRateLimitSettings, maxAttempts)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Range, 1.0, 1000.0)
            ARC_REFLECT_ATTR(Tooltip, "Attempts one key may make inside a window before its cooldown starts.")
        ARC_REFLECT_FIELD(NetRateLimitSettings, windowSeconds)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Range, 1.0, 86400.0)
            ARC_REFLECT_ATTR(Tooltip, "Length of the rate-limit counting window, in seconds.")
        ARC_REFLECT_FIELD(NetRateLimitSettings, cooldownSeconds)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Range, 0.0, 86400.0)
            ARC_REFLECT_ATTR(Tooltip, "Seconds a key stays refused after it trips the limit.")
        ARC_REFLECT_FIELD(NetRateLimitSettings, cleanupEvery)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 1.0, 1000000.0)
            ARC_REFLECT_ATTR(Tooltip, "Rate-limit checks between sweeps for idle records.")
        ARC_REFLECT_FIELD(NetRateLimitSettings, idleExpiryMinutes)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live) ARC_REFLECT_ATTR(Range, 1.0, 1440.0)
            ARC_REFLECT_ATTR(Tooltip, "Minutes after which an untouched rate-limit record is dropped.")
    ARC_END_REFLECT_TYPE()
}
