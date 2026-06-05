# v5 Medium-Tail Scopes 1-3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the three low-risk pre-launch ops items from the v5 medium-tail scoping design: (Scope 2) bound the ServiceEndpoint loopback accept loop at 64 concurrent connections, (Scope 3) close the AppendIdempotent recheck race by holding the per-account advisory lock, (Scope 1) make the public TCP connection caps configurable via `protocol.json` and throttle the per-IP refusal WARN so a CGNAT-blocked population doesn't log-flood ops.

**Architecture:** All three scopes are independent (disjoint files) and each lands as a single atomic commit matching the existing `chore: v5 medium batch (X) — ...` series (a-i already merged; these are batches j, k, l). Implementation order is ascending LOC/risk: Scope 2 first (~15 LOC, 1 file), Scope 3 second (~5 LOC, 1 file), Scope 1 last (~70 LOC, 6 files). Each scope is TDD-driven where deterministic; Scope 3's race is not deterministically testable and ships with an audit-tag annotation per the spec's documented coverage-by-pattern-equivalence rationale.

**Spec:** `docs/superpowers/specs/2026-06-04-v5-medium-tail-scoping-design.md` (approved through Scope 4 rescope, last commit `bd1f670`). Scope 4 (per-account locks refactor) is deferred to its own batch after 1-3 merge.

**Tech Stack:** C++20, Visual Studio 2026 / premake5, Catch2 v3 (CommonTests / AccountTests), nlohmann/json, libpqxx, Postgres 16. PowerShell 5.1 on Windows for build commands.

**Build prerequisites:** `VCPKG_ROOT` env var set, `Server\scripts\setup-vcpkg-deps.bat` already run once. Docker Postgres must be up (`Server\scripts\db-setup.bat`) for AccountTests integration suite to pass.

---

## Scope 2 — ServiceEndpoint Loopback Caps  [batch j, ~15 LOC, 1 file]

**Audit:** networking M-V5-7. `ServiceEndpoint::Start`'s accept loop spawns one detached thread per accepted loopback connection with no cap. A dev tool that connect-and-drops to `127.0.0.1:<internal_port>` in a tight loop spawns unbounded ~1.1MB-stack threads until OS limits. Production risk is bounded by loopback binding, but the inconsistency with the capped public-facing `TcpServerBase` is the kind of "why is internal less hardened than external?" finding that surfaces in security review.

### Task 1: Bound the loopback internal-RPC accept loop at 64 concurrent connections

**Files:**
- Modify: `Server/Common/src/Net/ServiceEndpoint.hpp` (constant + cap check)
- Create: `Server/Common/tests/ServiceEndpointCapsTest.cpp` (new regression guard)

- [ ] **Step 1: Write the failing test**

Create `Server/Common/tests/ServiceEndpointCapsTest.cpp`:

```cpp
// Audit M-V5-7 networking (2026-06-04): regression guard for the
// ServiceEndpoint loopback connection cap added in v5 medium batch (j).
//
// ServiceEndpoint::Start spawns one detached thread per accepted
// loopback connection. Without a ceiling, a Debug-build dev tool that
// connect-and-drops in a tight loop spawns unbounded ~1.1MB-stack
// threads. This test pins the (cap+1)th refusal path: the OS-level
// TCP handshake completes (ConnectSocket returns a valid FD) but the
// server's accept loop sees m_activeConnections >= cap and CloseSocket's
// the FD with no thread spawned. The client sees a FIN soon after.
//
// Cap is a private static constexpr; we exercise it through the public
// accept-loop behavior (open kMaxInternalConnections connections, then
// verify the next one is FIN'd within a generous CI timeout).

#include <catch2/catch_test_macros.hpp>

#include "Net/ServiceEndpoint.hpp"
#include "Net/TcpSocket.hpp"

#include <chrono>
#include <thread>
#include <vector>

using Aphelyon::ServiceEndpoint;
using Aphelyon::ConnectSocket;
using Aphelyon::CreateListenSocket;
using Aphelyon::InitSockets;
using Aphelyon::SetSocketTimeout;

namespace {

// Bind ephemeral, read the OS-assigned port, close. Mirrors
// TcpServerCapsTest's helper; SO_REUSEADDR on CreateListenSocket lets
// the rebind succeed on Windows.
uint16_t PickFreePortForServiceEndpoint() {
    InitSockets();
    SocketType s = CreateListenSocket(0, "127.0.0.1");
    REQUIRE(s != INVALID_SOCK);
    sockaddr_in a{};
#ifdef APHELYON_PLATFORM_WINDOWS
    int len = sizeof(a);
#else
    socklen_t len = sizeof(a);
#endif
    REQUIRE(getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    const uint16_t port = ntohs(a.sin_port);
    CloseSocket(s);
    return port;
}

} // namespace

TEST_CASE("ServiceEndpoint: loopback cap refuses the (kMaxInternalConnections+1)th connection",
          "[serviceendpoint][caps]") {
    // Mirror TcpServerCapsTest's shape: spin up the endpoint on a free
    // loopback port, open `cap` connections, verify the (cap+1)th sees
    // a graceful FIN within a generous timeout. We never send an RPC
    // frame — HandleConnection's recv blocks until the server's 5s
    // recv timeout fires or m_running flips; the cap refusal happens
    // BEFORE thread spawn so it's independent of recv timing.
    constexpr int kCap = 64;  // matches ServiceEndpoint::kMaxInternalConnections

    const uint16_t port = PickFreePortForServiceEndpoint();
    REQUIRE(port != 0);

    ServiceEndpoint endpoint(port);
    endpoint.Start();

    // Give the accept thread time to bind + enter the loop. Loopback
    // is fast; 200ms is conservative.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Open kCap connections sequentially with a small pause between
    // each so the accept loop has time to fetch_add m_activeConnections
    // before the next one arrives. Without the pause, simultaneous
    // accepts race the counter increment.
    std::vector<SocketType> accepted;
    accepted.reserve(kCap);
    for (int i = 0; i < kCap; ++i) {
        SocketType c = ConnectSocket("127.0.0.1", port);
        REQUIRE(c != INVALID_SOCK);
        accepted.push_back(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Drain: let the server fully process all accepted connections so
    // m_activeConnections == kCap before we test the cap.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // The (kCap+1)th connect: handshake completes, accept-loop CloseSocket's.
    SocketType refused = ConnectSocket("127.0.0.1", port);
    REQUIRE(refused != INVALID_SOCK);

    SetSocketTimeout(refused, 2000);
    char buf[4];
    int n = recv(refused, buf, sizeof(buf), 0);
    REQUIRE(n == 0);  // graceful FIN

    CloseSocket(refused);
    for (auto s : accepted) CloseSocket(s);

    endpoint.Stop();
}
```

- [ ] **Step 2: Regenerate the Visual Studio solution**

The CommonTests project's premake `files` glob includes `%{prj.location}/tests/**.cpp`, so the new file is picked up on regenerate.

Run: `Server\GenerateProjects.bat`
Expected: completes with `Done (XXXms)` and no premake errors.

- [ ] **Step 3: Build CommonTests and run the new test — verify it fails**

Run:
```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:CommonTests
.\Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe "[serviceendpoint][caps]"
```
Expected: FAIL — the (cap+1)th connection does NOT get a FIN within 2s (recv hangs and returns -1 on timeout, or returns >0 if the connection stays open). Currently every accept spawns a thread; no cap exists.

- [ ] **Step 4: Add the cap constant and accept-loop check**

In `Server/Common/src/Net/ServiceEndpoint.hpp`, add the constant next to `kDrainTimeoutSeconds` (around line 36-40):

```cpp
        // Audit M-V5-7 networking (2026-06-04): bound the loopback
        // accept loop so a misbehaving dev tool that connect-and-drops
        // in a tight loop can't spawn unbounded ~1.1MB-stack threads.
        // 64 is generous for the current 1-Auth ↔ 1-Account ↔ 1-Combat
        // topology where steady-state internal connection count is ≤ 6;
        // the cap only fires on pathological load. No per-IP tracking
        // (every loopback IP is 127.0.0.1) and no log throttle (loopback
        // refusal volume is dev-scoped, not user-scoped — a flood IS the
        // signal a tool is misbehaving).
        static constexpr int kMaxInternalConnections = 64;
```

In the same file, inside the accept loop (currently at line ~152, immediately AFTER `SetSocketTimeout(clientSocket, 5000);` and BEFORE `m_activeConnections.fetch_add(...)`), insert the cap check:

```cpp
                    // Audit M-V5-7 networking (2026-06-04): cap-check
                    // BEFORE fetch_add + thread spawn so a refused
                    // connection costs only the accept syscall + a
                    // close. Check is outside the atomic, so a brief
                    // race between two simultaneous accepts could let
                    // through one extra — acceptable, 64+1 is still
                    // bounded.
                    if (m_activeConnections.load(std::memory_order_relaxed) >= kMaxInternalConnections)
                    {
                        LOG_NET_WARN("ServiceEndpoint: refused loopback connect — cap {} reached (port {})",
                                     kMaxInternalConnections, m_port);
                        CloseSocket(clientSocket);
                        continue;
                    }
```

- [ ] **Step 5: Rebuild and re-run the test — verify it passes**

Run:
```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:CommonTests
.\Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe "[serviceendpoint][caps]"
```
Expected: PASS — `All tests passed (1 assertion in 1 test case)` (or similar).

- [ ] **Step 6: Run the full CommonTests suite to confirm no regression**

Run:
```powershell
.\Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
```
Expected: all tests pass. Specifically verify the existing TcpServerCapsTest cases (`[server][caps]`) still pass.

- [ ] **Step 7: Commit**

Run:
```powershell
git add Server\Common\src\Net\ServiceEndpoint.hpp Server\Common\tests\ServiceEndpointCapsTest.cpp
git commit -m @'
chore: v5 medium batch (j) — ServiceEndpoint loopback cap (M-V5-7 networking)

Bound ServiceEndpoint's accept loop at 64 concurrent loopback
connections. Mirrors TcpServerBase's existing public-facing caps so
internal RPC is no less hardened than external. New CommonTests case
ServiceEndpointCapsTest.cpp pins the (cap+1)th refusal path.

Spec: docs/superpowers/specs/2026-06-04-v5-medium-tail-scoping-design.md
Audit: docs/superpowers/audits/2026-06-03-v5-followup-networking.md
'@
```

---

## Scope 3 — Advisory Lock in AppendIdempotent Recheck  [batch k, ~5 LOC, 1 file]

**Audit:** event-sourcing M-V5-1. `EventStore::AppendIdempotent`'s catch block opens a second `pqxx::work` to disambiguate "idempotency-key hit" from "version race" — but does NOT acquire the per-account advisory lock the way `Append` does. Race window: writer A throws `ConcurrencyConflict`, releases its lock, writer B with same `idempotency_key` slips in, A's recheck finds B's row, swallows as "idempotency hit" — A's caller now believes A's event landed when actually B's did. Pre-launch we have no concurrent traffic; post-launch this is "1-in-N retries silently writes wrong state" with N depending on retry rate.

### Task 2: Hold the per-account advisory lock through the AppendIdempotent recheck

**Files:**
- Modify: `Server/Account/src/Db/EventStore.hpp` (catch block at ~L136-144)
- Modify (comment only): `Server/Account/tests/Integration/IdempotencyMachineryTest.cpp` (audit-coverage NOTE)

Race testing is not deterministic in unit tests. Per the spec's documented coverage-by-pattern-equivalence rationale, the change is structurally identical to `Append`'s already-audited lock+commit pattern; coverage is an `audit:M-V5-1 event-sourcing` comment-tag in the catch block plus a NOTE in `IdempotencyMachineryTest.cpp` flagging the recheck as covered-by-pattern-equivalence. A load-driven concurrent-retry test belongs in a future soak harness.

- [ ] **Step 1: Edit the AppendIdempotent catch block**

In `Server/Account/src/Db/EventStore.hpp`, replace the existing catch block (lines 136-144) with the locked version:

Current code (lines 136-144):
```cpp
        } catch (const ConcurrencyConflict&) {
            // Check whether the conflict was on idempotency_key vs version
            auto lease = pool_.acquire();
            pqxx::work tx(*lease);
            auto r = tx.exec(
                "SELECT 1 FROM events WHERE account_id = $1 AND aggregate_kind = $2 AND idempotency_key = $3 LIMIT 1",
                pqxx::params{ev.account_id, events::AggregateKindToStr(ev.aggregate_kind), ev.idempotency_key});
            if (r.empty()) throw;  // conflict was version-based, re-throw
        }
```

Replace with:
```cpp
        } catch (const ConcurrencyConflict&) {
            // audit:M-V5-1 event-sourcing (2026-06-04): the recheck must
            // hold the per-account advisory lock so a concurrent writer
            // can't land a row with the same idempotency_key between the
            // throw above and the SELECT 1 below. Without the lock, a
            // genuine version-collision retry race could be silently
            // swallowed as an idempotency hit — A's caller would believe
            // A's event landed when actually B's event landed. Structurally
            // equivalent to Append's lock+commit pattern (L115-121). Race
            // testing is not deterministic in unit tests; coverage is
            // pattern-equivalence, see IdempotencyMachineryTest.cpp NOTE.
            auto lease = pool_.acquire();
            pqxx::work tx(*lease);
            AcquireAdvisoryLockInTx(tx, ev.account_id);
            auto r = tx.exec(
                "SELECT 1 FROM events WHERE account_id = $1 AND aggregate_kind = $2 AND idempotency_key = $3 LIMIT 1",
                pqxx::params{ev.account_id, events::AggregateKindToStr(ev.aggregate_kind), ev.idempotency_key});
            if (r.empty()) throw;  // conflict was version-based, re-throw
            // The original code didn't commit because the SELECT was a
            // pure read. With the advisory lock now held, commit promptly
            // so the lock releases on Tx end rather than waiting for the
            // connection lease to return to the pool and pqxx to destruct
            // the work — correct but unnecessarily long.
            tx.commit();
        }
```

- [ ] **Step 2: Add the audit-coverage NOTE to the idempotency test header**

In `Server/Account/tests/Integration/IdempotencyMachineryTest.cpp`, extend the existing top-of-file header comment (currently lines 1-16) by inserting a new bullet about the recheck-pattern coverage. Add immediately after the existing `//   - IdempotencyKey::Scoped's input validation: ...` bullet (around line 15), and before the `#include` block at line 17:

```cpp
//
// NOTE (audit M-V5-1 event-sourcing, 2026-06-04): EventStore::
// AppendIdempotent's recheck path acquires the same per-account
// advisory lock that Append acquires. Race-driven coverage of the
// recheck would require concurrent writers issuing same-key retries
// in a tight loop — not deterministic in a unit test. The change is
// structurally equivalent to Append's already-audited lock+commit
// pattern (EventStore.hpp ~L115-121, AcquireAdvisoryLockInTx → AppendInTx
// → tx.commit()); review-by-pattern-equivalence is the documented
// coverage level for this path. A load-driven concurrent-retry test
// belongs in a future soak harness.
```

- [ ] **Step 3: Build Account and AccountTests**

Run:
```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:Account
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:AccountTests
```
Expected: both targets build clean. No new warnings.

- [ ] **Step 4: Run AccountTests to confirm the existing idempotency suite still passes**

Postgres must be up; if not, run `Server\scripts\db-setup.bat` first.

Run:
```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[idempotency]"
```
Expected: all `[idempotency]`-tagged cases pass — `IdempotencyMachineryTest` round-trip, ON CONFLICT DO UPDATE refresh, sibling-key construction, input validation. Also run the broader integration tag to catch any cross-test regression:

```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe "[integration]"
```
Expected: all `[integration]`-tagged cases pass.

- [ ] **Step 5: Commit**

Run:
```powershell
git add Server\Account\src\Db\EventStore.hpp Server\Account\tests\Integration\IdempotencyMachineryTest.cpp
git commit -m @'
chore: v5 medium batch (k) — AppendIdempotent recheck holds advisory lock (M-V5-1 event-sourcing)

Acquire the per-account advisory lock during AppendIdempotent's
recheck transaction so a concurrent writer with the same
idempotency_key cannot land a row between the ConcurrencyConflict
throw and the dedup SELECT. Structurally equivalent to Append's
existing lock+commit pattern; race-driven coverage deferred to
future soak harness, NOTE pinned in IdempotencyMachineryTest.cpp.

Spec: docs/superpowers/specs/2026-06-04-v5-medium-tail-scoping-design.md
Audit: docs/superpowers/audits/2026-06-03-v5-followup-event-sourcing.md
'@
```

---

## Scope 1 — NAT Cap Configurability + Log Throttle  [batch l, ~70 LOC, 6 files]

**Audits:** networking M-V5-6, security M-V5-1. Three forward-compat hazards on the public-facing TCP caps:
1. **CGNAT/shared-NAT collision** — 1000-player school/dorm/carrier egress hits per-IP cap 16, the 17th legit user refused indistinguishably from a slow-loris attacker.
2. **Reverse-proxy collapse** — if Aphelyon ever sits behind Nginx/Envoy/CloudFront, 100% of accept calls return the proxy's IP, cap fires for everyone.
3. **Log storm on refusal** — `LOG_NET_WARN` fires once per refused SYN, sustained at network retry cadence under a CGNAT-blocked population.

Fix is three pieces: configurable caps via `protocol.json` `settings` block, per-IP refusal log throttle (1 line per IP per second, drop the rest), doc comment naming the proxy upgrade path so the topology decision stays open.

### Task 3: Make TCP connection caps configurable from `protocol.json` and add per-IP refusal log throttle

**Files:**
- Modify: `Server/Common/src/Net/Protocol.hpp` (Settings struct + loader)
- Modify: `Server/Common/src/Net/TcpServerBase.hpp` (ctor args, member fields, accept-loop cap reads, log throttle, disconnect cleanup)
- Modify: `Server/Common/src/Net/TcpSocket.hpp` (doc comment block near constants)
- Modify: `Server/data/protocol.json` (explicit defaults for the two keys)
- Modify: `Server/Auth/src/AuthServer.hpp` (ctor args passthrough)
- Modify: `Server/Account/src/AccountServer.hpp` (ctor args passthrough)
- Modify: `Server/Combat/src/CombatServer.hpp` (ctor args passthrough)
- Modify: `Server/Auth/src/main.cpp` (read settings → service ctor)
- Modify: `Server/Account/src/main.cpp` (read settings → service ctor)
- Modify: `Server/Combat/src/main.cpp` (read settings → service ctor)
- Modify: `Server/Common/tests/TcpServerCapsTest.cpp` (new test case for ctor-supplied cap)

- [ ] **Step 1: Write the failing test (drives ctor signature out across the codebase)**

In `Server/Common/tests/TcpServerCapsTest.cpp`, extend `TestServer` to accept and forward optional cap args, then add a new TEST_CASE after the existing two. The test demonstrates the configurable path with a small per-IP cap of 3.

Replace the existing TestServer class (lines 39-59) with:

```cpp
class TestServer final : public TcpServerBase {
public:
    TestServer(uint16_t port, uint16_t internalPort,
               std::size_t maxConnPerIp = ServerConfig::MAX_CONNECTIONS_PER_IP,
               std::size_t maxConnTotal = ServerConfig::MAX_CONNECTIONS_TOTAL)
        : TcpServerBase(port, internalPort, maxConnPerIp, maxConnTotal) {}

protected:
    AuthResult ValidateSession(const std::string&,
                               const std::string&,
                               MsgId) override {
        AuthResult r;
        r.valid = true;
        r.playerId = "test";
        return r;
    }

    std::string OnProcessMessage(const Message&,
                                 const std::string&,
                                 const std::string&) override {
        return std::string{};
    }
};
```

Append a new TEST_CASE at the end of the file:

```cpp
TEST_CASE("TcpServerBase: ctor-supplied small per-IP cap refuses the (small+1)th connection",
          "[server][caps]") {
    // Audit M-V5-6 networking (2026-06-04): pins the configurable-cap
    // path added in v5 medium batch (l). protocol.json's
    // settings.max_connections_per_ip flows main.cpp → service ctor →
    // TcpServerBase ctor → m_maxConnPerIp. A cap of 3 is well below the
    // compile-time default (16) so this test fails fast if the ctor
    // arg is silently ignored or shadowed by the old ServerConfig::*
    // constant reference in the accept loop.
    constexpr std::size_t kSmallCap = 3;

    const uint16_t port         = PickFreePort();
    const uint16_t internalPort = PickFreePort();
    REQUIRE(port != 0);
    REQUIRE(internalPort != 0);
    REQUIRE(port != internalPort);

    TestServer server(port, internalPort, /*maxConnPerIp=*/kSmallCap,
                      /*maxConnTotal=*/2048);
    std::thread srvThread([&]() { server.Start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<SocketType> accepted;
    accepted.reserve(kSmallCap);
    for (std::size_t i = 0; i < kSmallCap; ++i) {
        SocketType c = ConnectSocket("127.0.0.1", port);
        REQUIRE(c != INVALID_SOCK);
        accepted.push_back(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    SocketType refused = ConnectSocket("127.0.0.1", port);
    REQUIRE(refused != INVALID_SOCK);

    SetSocketTimeout(refused, 2000);
    char buf[4];
    int n = recv(refused, buf, sizeof(buf), 0);
    REQUIRE(n == 0);

    CloseSocket(refused);
    for (auto s : accepted) CloseSocket(s);

    server.Stop();
    if (srvThread.joinable()) srvThread.join();
}
```

- [ ] **Step 2: Verify the test does not compile yet**

Run:
```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug /t:CommonTests
```
Expected: build FAIL with errors about `TcpServerBase` constructor not accepting 4 args. This confirms the test drives the ctor signature change.

- [ ] **Step 3: Extend the Settings struct and loader**

In `Server/Common/src/Net/Protocol.hpp`, extend the `Settings` struct (around lines 91-100):

```cpp
        struct Settings
        {
            int defaultPort = 0;
            int maxMessageSize = 0;
            std::string messageFormat;
            int tokenLength = 0;
            int sessionLifetimeSeconds = 0;
            int idleTimeoutSeconds = 0;
            int heartbeatIntervalSeconds = 0;
            // Audit M-V5-6 networking (2026-06-04): public-facing TCP
            // connection caps, formerly compile-time constants in
            // TcpSocket.hpp::ServerConfig. Configurable so a launch-day
            // topology change (CGNAT presence, reverse-proxy, etc.)
            // doesn't require a rebuild. Missing keys in protocol.json
            // fall back to the ServerConfig defaults at load time, so
            // pre-existing protocol.json files keep working unchanged.
            int maxConnectionsPerIp = 0;
            int maxConnectionsTotal = 0;
        };
```

In the same file, inside `Load()` (around lines 151-157), append the two parses AFTER the existing `m_settings.heartbeatIntervalSeconds = ...;` line and BEFORE the `if (j.contains("messages"))` block (around line 159):

```cpp
                m_settings.heartbeatIntervalSeconds = settings["heartbeat_interval_seconds"].get<int>();

                // Audit M-V5-6 networking (2026-06-04): optional keys.
                // Default to the compile-time ServerConfig values when
                // absent so existing protocol.json files keep working
                // without modification. Casting through int is safe —
                // the ServerConfig constants are small (16, 2048).
                m_settings.maxConnectionsPerIp = settings.value(
                    "max_connections_per_ip",
                    static_cast<int>(ServerConfig::MAX_CONNECTIONS_PER_IP));
                m_settings.maxConnectionsTotal = settings.value(
                    "max_connections_total",
                    static_cast<int>(ServerConfig::MAX_CONNECTIONS_TOTAL));
```

Note: `ServerConfig` lives in `TcpSocket.hpp`; `Protocol.hpp` does NOT include `TcpSocket.hpp` today. Add the include at the top of `Protocol.hpp` (around line 9, alongside the other `#include` lines):

```cpp
#include "Net/TcpSocket.hpp"
```

- [ ] **Step 4: Add the cap args + member fields + log throttle to TcpServerBase**

In `Server/Common/src/Net/TcpServerBase.hpp`, replace the ctor (lines 51-56) with:

```cpp
        // Audit M-V5-6 networking (2026-06-04): cap args default to the
        // ServerConfig compile-time constants so existing ctors that
        // pass only (port, internalPort) compile unchanged. main.cpp's
        // settings-aware path supplies explicit values from protocol.json.
        explicit TcpServerBase(uint16_t port, uint16_t internalPort,
                               std::size_t maxConnPerIp = ServerConfig::MAX_CONNECTIONS_PER_IP,
                               std::size_t maxConnTotal = ServerConfig::MAX_CONNECTIONS_TOTAL)
            : m_port(port)
            , m_running(false)
            , m_nextClientId(1)
            , m_maxConnPerIp(maxConnPerIp)
            , m_maxConnTotal(maxConnTotal)
            , m_internalEndpoint(internalPort)
        {}
```

In the same file, add member fields after `m_clientsByIp` (around line 666). Place the throttle map next to `m_clientsByIp` since both are guarded by `m_clientsMutex`:

```cpp
        // Audit M-V5-6 networking (2026-06-04): runtime cap values.
        // Source: protocol.json settings.max_connections_per_ip /
        // settings.max_connections_total via ProtocolLoader, with
        // ServerConfig defaults if the keys are absent. Stored as
        // members (not re-read on every accept) so a runtime config
        // reload would require a restart — acceptable, the caps don't
        // need hot-reload semantics.
        std::size_t m_maxConnPerIp;
        std::size_t m_maxConnTotal;
        // Audit M-V5-1 security (2026-06-04): per-IP last-WARN timestamp
        // for the per-IP-cap refusal log throttle. Without throttling, a
        // CGNAT-blocked population logs one WARN per refused SYN at the
        // network's natural retry cadence (browsers, mobile clients,
        // Steam-style relaunchers). One WARN per IP per second is the
        // signal that the IP is hitting cap; suppressed lines are
        // dropped because counting them adds noise. Guarded by
        // m_clientsMutex; cleaned up alongside m_clientsByIp in
        // HandleClient's disconnect block.
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastRefusalLog;
```

Replace the accept-loop cap block (currently lines 143-163, the `{ std::lock_guard<std::mutex> lock(m_clientsMutex); ... }` block) with the throttled, member-field version:

```cpp
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    if (m_clients.size() >= m_maxConnTotal)
                    {
                        LOG_NET_WARN("Accept refused: total connection cap {} reached (rejecting {})",
                            m_maxConnTotal, clientIP);
                        CloseSocket(clientSocket);
                        continue;
                    }
                    auto& perIpCount = m_clientsByIp[clientIP];
                    if (perIpCount >= m_maxConnPerIp)
                    {
                        // Audit M-V5-1 security (2026-06-04): emit the
                        // per-IP refusal WARN at most once per second per
                        // IP. The rate-limit IS the signal that the IP
                        // is hitting cap; counting suppressed lines adds
                        // noise. CGNAT or reverse-proxy collapse can
                        // sustain refusal at the network's natural retry
                        // cadence; an unthrottled log dwarfs everything
                        // else in the sink. m_lastRefusalLog is guarded
                        // by m_clientsMutex (same lock as m_clientsByIp).
                        const auto now = std::chrono::steady_clock::now();
                        auto& last = m_lastRefusalLog[clientIP];
                        if (last == std::chrono::steady_clock::time_point{} ||
                            now - last >= std::chrono::seconds(1))
                        {
                            LOG_NET_WARN("Accept refused: per-IP cap {} reached for {}",
                                m_maxConnPerIp, clientIP);
                            last = now;
                        }
                        CloseSocket(clientSocket);
                        continue;
                    }
                    ++perIpCount;

                    Logger::LogConnectionEvent("connected", clientIP);
                    SetSocketTimeout(clientSocket, ServerConfig::RECV_TIMEOUT_MS);
                    // Audit M-V5-10 networking (2026-06-03): TCP keepalive
                    // on every accepted client socket so half-open
                    // connections from crashed clients release their
                    // connection-cap slot within ~5min instead of the OS
                    // default 2h. Especially important with the per-IP
                    // cap (16): 16 stale half-open conns from one CGNAT
                    // egress would otherwise shut out new legit users
                    // for hours.
                    EnableTcpKeepAlive(clientSocket);

                    const uint64_t clientId = m_nextClientId++;
                    m_clients[clientId] = ClientConnection(clientSocket, clientIP);
                    m_clients[clientId].thread = std::thread(&TcpServerBase::HandleClient, this, clientId);
                }
```

In the same file, update the disconnect cleanup block in `HandleClient` (currently lines 553-558) to also erase from `m_lastRefusalLog` when the per-IP counter drops to zero. Replace this fragment:

```cpp
                    if (auto ipIt = m_clientsByIp.find(it->second.ipAddress);
                        ipIt != m_clientsByIp.end())
                    {
                        if (ipIt->second > 0) --ipIt->second;
                        if (ipIt->second == 0) m_clientsByIp.erase(ipIt);
                    }
```

with:

```cpp
                    if (auto ipIt = m_clientsByIp.find(it->second.ipAddress);
                        ipIt != m_clientsByIp.end())
                    {
                        if (ipIt->second > 0) --ipIt->second;
                        if (ipIt->second == 0)
                        {
                            // Audit M-V5-1 security (2026-06-04): drop
                            // the throttle entry alongside the count so
                            // the map only carries active IPs — matches
                            // the m_clientsByIp eviction invariant
                            // (audit M-V5-3 concurrency).
                            m_lastRefusalLog.erase(it->second.ipAddress);
                            m_clientsByIp.erase(ipIt);
                        }
                    }
```

In the same file, update the `Stop()` cleanup block (currently around lines 248-250 inside the two-phase collect-then-join) to also clear `m_lastRefusalLog`:

```cpp
                m_clients.clear();
                m_clientsByIp.clear();
                m_lastRefusalLog.clear();
```

- [ ] **Step 5: Add the proxy-upgrade-path doc comment to TcpSocket.hpp**

In `Server/Common/src/Net/TcpSocket.hpp`, append a comment block to the existing `MAX_CONNECTIONS_*` block (currently lines 49-55). Replace the existing block with:

```cpp
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
        // settings.max_connections_total at startup (see Auth/Account/
        // Combat main.cpp). A protocol.json without the keys uses these
        // constants — no behavior change at default values.
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
```

- [ ] **Step 6: Add the two keys to `data/protocol.json` explicitly**

In `Server/data/protocol.json`, add the two new settings to the `settings` block (currently around lines 1192-1200). The block is alphabetically ordered today; insert at the right position:

```json
  "settings": {
    "default_port": 7777,
    "heartbeat_interval_seconds": 60,
    "idle_timeout_seconds": 1800,
    "max_connections_per_ip": 16,
    "max_connections_total": 2048,
    "max_message_size": 65536,
    "message_format": "LENGTH:TYPE|TOKEN|PAYLOAD\\n",
    "session_lifetime_seconds": 86400,
    "token_length": 64
  },
```

Note: keys are explicit at the existing constexpr default values, so behavior is bit-for-bit identical to today. The point of pinning them in JSON is to make the configurable surface discoverable to ops, not to change behavior.

- [ ] **Step 7: Thread cap args through AuthServer ctor**

In `Server/Auth/src/AuthServer.hpp`, replace the existing ctor (lines 35-47) with:

```cpp
        explicit AuthServer(uint16_t port = 7777, uint16_t accountInternalPort = 7773,
                            uint16_t internalPort = 7770,
                            uint16_t accountPort = 7771,
                            uint16_t combatTcpPort = 7772, uint16_t combatUdpPort = 7778,
                            std::size_t maxConnPerIp = ServerConfig::MAX_CONNECTIONS_PER_IP,
                            std::size_t maxConnTotal = ServerConfig::MAX_CONNECTIONS_TOTAL)
            : TcpServerBase(port, internalPort, maxConnPerIp, maxConnTotal)
            , m_sessionManager(CreateSessionConfig())
            , m_accountClient("127.0.0.1", accountInternalPort)
            , m_accountPort(accountPort)
            , m_combatTcpPort(combatTcpPort)
            , m_combatUdpPort(combatUdpPort)
        {
            SetupInternalEndpoint();
        }
```

- [ ] **Step 8: Thread cap args through AccountServer ctor**

In `Server/Account/src/AccountServer.hpp`, replace the existing ctor signature (lines 65-69) with:

```cpp
        explicit AccountServer(uint16_t port, uint16_t authInternalPort,
                               uint16_t internalPort,
                               std::string dbConn,
                               std::size_t dbPoolSize = 16,
                               std::size_t maxConnPerIp = ServerConfig::MAX_CONNECTIONS_PER_IP,
                               std::size_t maxConnTotal = ServerConfig::MAX_CONNECTIONS_TOTAL)
            : TcpServerBase(port, internalPort, maxConnPerIp, maxConnTotal)
```

Leave the rest of the member-init list unchanged.

- [ ] **Step 9: Thread cap args through CombatServer ctor**

In `Server/Combat/src/CombatServer.hpp`, replace the existing ctor (lines 23-31) with:

```cpp
        explicit CombatServer(uint16_t lobbyPort       = 7772,
                              uint16_t authInternalPort = 7770,
                              uint16_t internalPort     = 7774,
                              uint16_t udpPort          = 7778,
                              std::size_t maxConnPerIp = ServerConfig::MAX_CONNECTIONS_PER_IP,
                              std::size_t maxConnTotal = ServerConfig::MAX_CONNECTIONS_TOTAL)
            : TcpServerBase(lobbyPort, internalPort, maxConnPerIp, maxConnTotal)
            , m_udpPort(udpPort)
            , m_authClient("127.0.0.1", authInternalPort)
            , m_sessionCache(m_authClient, 60)
        {}
```

- [ ] **Step 10: Wire `Server/Auth/src/main.cpp` to read settings → ctor**

In `Server/Auth/src/main.cpp`, replace the AuthServer construction (currently line 142, `Aphelyon::AuthServer server(port);`) with the settings-aware version. The settings load already happens at line 124; just consume them. Place this immediately after the `port` override at line 134:

```cpp
    const auto& netSettings = protocol.GetSettings();
    const auto maxConnPerIp = static_cast<std::size_t>(netSettings.maxConnectionsPerIp);
    const auto maxConnTotal = static_cast<std::size_t>(netSettings.maxConnectionsTotal);
```

And at line 142, replace:
```cpp
    Aphelyon::AuthServer server(port);
```
with:
```cpp
    Aphelyon::AuthServer server(port, /*accountInternalPort=*/7773,
                                /*internalPort=*/7770,
                                /*accountPort=*/7771,
                                /*combatTcpPort=*/7772,
                                /*combatUdpPort=*/7778,
                                maxConnPerIp, maxConnTotal);
```

- [ ] **Step 11: Wire `Server/Account/src/main.cpp` to read settings → ctor**

In `Server/Account/src/main.cpp`, place the settings read right after the protocol load (after line 193, before the `gachaConfig` block):

```cpp
    const auto& netSettings = protocol.GetSettings();
    const auto maxConnPerIp = static_cast<std::size_t>(netSettings.maxConnectionsPerIp);
    const auto maxConnTotal = static_cast<std::size_t>(netSettings.maxConnectionsTotal);
```

And at line 234, replace:
```cpp
    Aphelyon::AccountServer server(port, /*authInternalPort=*/7770,
                                   /*internalPort=*/7773, dbConn);
```
with:
```cpp
    Aphelyon::AccountServer server(port, /*authInternalPort=*/7770,
                                   /*internalPort=*/7773, dbConn,
                                   /*dbPoolSize=*/16,
                                   maxConnPerIp, maxConnTotal);
```

- [ ] **Step 12: Wire `Server/Combat/src/main.cpp` to read settings → ctor**

In `Server/Combat/src/main.cpp`, the protocol load already runs. Place the settings read immediately after the protocol load completes (around line 99, before the signal handler at line 101):

```cpp
    const auto& netSettings = Aphelyon::ProtocolLoader::Instance().GetSettings();
    const auto maxConnPerIp = static_cast<std::size_t>(netSettings.maxConnectionsPerIp);
    const auto maxConnTotal = static_cast<std::size_t>(netSettings.maxConnectionsTotal);
```

And at line 106, replace:
```cpp
    Aphelyon::CombatServer server(lobbyPort, udpPort);
```
with:
```cpp
    Aphelyon::CombatServer server(lobbyPort, /*authInternalPort=*/7770,
                                  /*internalPort=*/7774, udpPort,
                                  maxConnPerIp, maxConnTotal);
```

Note: Combat's ctor's 4th positional arg is `udpPort`, not the cap. The full positional form keeps the wiring explicit and matches the new ctor signature added in Step 9.

- [ ] **Step 13: Build everything**

Run:
```powershell
msbuild Server\Aphelyon.slnx /p:Configuration=Debug
```
Expected: clean build across Common, Auth, Account, Combat, AccountTests, CommonTests, AuthTests, CombatTests. No new warnings.

- [ ] **Step 14: Run the new test and verify it passes**

Run:
```powershell
.\Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe "[server][caps]"
```
Expected: all three TcpServerCapsTest cases pass — the existing two (default-cap refuse, default-cap accept-below) plus the new ctor-supplied-small-cap case.

- [ ] **Step 15: Run the full CommonTests suite to confirm no regression**

Run:
```powershell
.\Server\bin\Debug-windows-x86_64\CommonTests\CommonTests.exe
```
Expected: all CommonTests pass — `[server][caps]`, `[serviceendpoint][caps]` (from Scope 2), and every other tag.

- [ ] **Step 16: Run AccountTests integration suite to confirm no cross-service regression**

Postgres must be up; if not, run `Server\scripts\db-setup.bat`.

Run:
```powershell
.\Server\bin\Debug-windows-x86_64\AccountTests\AccountTests.exe
```
Expected: all AccountTests pass, including integration suite. AccountServer ctor change must not have broken any test that constructs AccountServer directly.

- [ ] **Step 17: Smoke-test the three services end-to-end**

Run:
```powershell
Server\scripts\start-all.bat
```
Expected: all three services start cleanly; each logs `Loaded v4 with N messages` (protocol.json load) and `Starting <X> service on port ...`. No new WARN about cap config; no startup crash.

After ~10s of clean run, Ctrl-C and verify clean shutdown:
```powershell
Server\scripts\stop-all.bat
```
Expected: all services stop cleanly with `Stopping service on port ...` log.

- [ ] **Step 18: Commit**

Run:
```powershell
git add Server\Common\src\Net\Protocol.hpp Server\Common\src\Net\TcpServerBase.hpp Server\Common\src\Net\TcpSocket.hpp Server\data\protocol.json Server\Auth\src\AuthServer.hpp Server\Account\src\AccountServer.hpp Server\Combat\src\CombatServer.hpp Server\Auth\src\main.cpp Server\Account\src\main.cpp Server\Combat\src\main.cpp Server\Common\tests\TcpServerCapsTest.cpp
git commit -m @'
chore: v5 medium batch (l) — configurable NAT caps + per-IP refusal log throttle (M-V5-6 net / M-V5-1 sec)

Make TcpServerBase's per-IP and total connection caps configurable
via protocol.json settings.max_connections_per_ip /
settings.max_connections_total; existing constants in TcpSocket.hpp
remain as fallback defaults so a protocol.json without the keys is
bit-for-bit identical to today. Wire through Auth/Account/Combat
service ctors and main.cpp; doc the topology assumptions + reverse-
proxy upgrade path adjacent to the constants.

Throttle the per-IP refusal WARN to at most one line per IP per
second (drop suppressed messages — the rate IS the signal). Without
the throttle, a CGNAT-blocked population logs once per refused SYN
at network retry cadence — the WARN would dwarf everything else in
the sink. Map cleaned up alongside m_clientsByIp in the disconnect
block (audit M-V5-3 concurrency invariant).

New regression test (ctor-supplied small per-IP cap of 3, verify the
4th refused) pins the configurable path; existing default-cap tests
remain green.

Spec: docs/superpowers/specs/2026-06-04-v5-medium-tail-scoping-design.md
Audits: docs/superpowers/audits/2026-06-03-v5-followup-networking.md,
        docs/superpowers/audits/2026-06-03-v5-followup-security.md
'@
```

---

## Post-merge verification

After all three commits land:

- [ ] **Confirm the audit-tag trail is intact**

Run:
```powershell
git log --oneline -3
```
Expected: three new commits matching the `chore: v5 medium batch (j|k|l)` pattern, on top of `bd1f670 docs: rescope Scope 4...`.

- [ ] **Confirm Scope 4 (per-account locks refactor) decision remains deferred**

The spec defers Scope 4 to its own batch after 1-3 merge. Do NOT proceed to Scope 4 in this plan. The Scope 4 implementation decision (snapshot pattern vs per-account locks) is rescoped per `bd1f670` and is in its own future plan.

## Out-of-scope items (deferred per spec)

The following spec sections are explicitly NOT implemented by this plan; they retain the deferral rationale documented in the spec's "Out of scope" section:

- Persistence M-V5-4 (events.idempotency_key partition-scoped UNIQUE) — by design; cross-partition dedup is `idempotency_cache`, not events UNIQUE.
- Persistence M-V5-5 (public_uid_seq overflow monitoring) — post-launch metric.
- Networking proxy plumbing (PROXY-protocol v1, X-Forwarded-For) — topology not chosen; Scope 1's config knob + doc-comment upgrade path leave the option open.
- Networking M-V5-1 / M-V5-3 (server-side timeout / probe classification completeness) — already deferred in `bc05aee` annotation pass.
- Concurrency M-V5-1 (OutboxRelay worker thread member-init order) — tied to deferred H-V5-1 OutboxRelay::Register contract.
- Idempotency M-V5-2 (AddCurrency BuildStatePayload caching) — behavioral change, deferred to IAP integration spec.
- **Scope 4 (per-account locks refactor)** — high-risk, 5-6 sub-commits, separate batch after 1-3 merge.
