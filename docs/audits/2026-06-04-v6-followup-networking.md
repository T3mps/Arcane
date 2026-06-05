# v6 followup — Network + RPC + Envelope

**Date:** 2026-06-04 (v6, focused dimensional audit)
**Dimension:** Network + RPC + envelope
**Scope:** `Server/Common/src/Net/*` (TcpServerBase, TcpSocket, ServiceClient, ServiceEndpoint, InternalRpcAuth, RpcFraming, SessionCache, Protocol) plus three TcpServerBase subclasses (AuthServer / AccountServer / CombatServer) and their main.cpp wiring; verification of the Scope 1 + Scope 2 batches (commits `27d7917`, `c36947e`, `b37390b`) and the H-V5-2 / H-V5-3 / H-V4-5 closures.
**Method:** Re-read v5 networking followup + v5 medium-tail scoping spec, then walked each v5 item against current HEAD code state. Cross-checked port-topology drift, envelope-error classification across all 4 `Call` consumers, drain budget split, length-frame parsing, and connect timeout mutex-hold ceiling.

---

## Verdict

**Clean.** The v5 networking arc shipped its top-3 lifecycle defects (H-V5-1 cleanup-thread-after-Stop, H-V5-2 drain budget, H-V5-3 AuthServer envelope-error) plus the Scope-1/Scope-2 NAT/loopback work. All three Highs are verifiably closed. The two annotated-deferrals (M-V5-1 server-side timeout, M-V5-3 Probe classification completeness) carry forward as low-noise observation items — pinned at the call sites with `DEFER (audit ref: ...)` comments and unit-test coverage that locks in the current behavior so future expansion breaks the test, not production. The single notable new finding is observational (Combat UDP port logged but no socket opened — visible flag, not a runtime issue).

---

## Status of v5 items in this dimension

| Item | Verdict | Evidence |
|---|---|---|
| **H-V5-1** AuthServer HandleLogin/HandleRegister envelope-error miss | **CLOSED** (commit unknown — Scope-pre-l) | `AuthServer.hpp:295-300` (HandleRegister), `:366-371` (HandleLogin) both inspect `rpc.value.contains("error")` before the `result.value("result", rpc.value)` fallback. WARN log + "Account service unavailable" response. Mirrors the M-V4-9 SessionCache pattern. |
| **H-V5-3** SIGABRT on Release-fatal probe failure (`m_cleanupThread` spawn-after-Stop) | **CLOSED** | `TcpServerBase.hpp:109` spawns `m_cleanupThread` BEFORE `OnStarted()`. Inline comment at L98-108 documents the fix rationale. The L119 `m_running.load()` re-check after OnStarted surfaces the early-shutdown to the caller (M-V5-5 companion). |
| **H-V5-4** ServiceEndpoint 10s detached-thread drain widens UAF window | **CLOSED** | `ServiceEndpoint.hpp:36-40` splits `kDrainTimeoutSeconds` into Debug 30s / Release 10s. Documents the (longest handler runtime + server recv timeout) sizing at L26-35. WARN log on overrun at L226-231 surfaces drain failures to ops. |
| **M-V5-6** NAT cap configurability + log throttle | **CLOSED** (commit `27d7917`) | `Protocol.hpp:108-109,174-179` (settings struct + loader fallback), `TcpServerBase.hpp:55-57` (ctor args with ServerConfig defaults), `:172-180` (per-IP refusal log throttle with `m_lastRefusalLog`). protocol.json carries `max_connections_per_ip: 16` and `max_connections_total: 2048` defaults explicitly. main.cpp wires through in all three services. |
| **M-V5-7** ServiceEndpoint loopback caps | **CLOSED** (commit `c36947e` + followup `b37390b`) | `ServiceEndpoint.hpp:51` `kMaxInternalConnections = 64`, check at `:171-177` before `fetch_add`. WARN log on refusal. Test pinned in `ServiceEndpointCapsTest.cpp`. |
| **H-V5-4 (test)** `ServiceEndpoint::InvokeForTest` gate | **CLOSED** | `ServiceEndpoint.hpp:103-116` gated behind `#ifdef APHELYON_TEST_BUILD`. Comment at L94-102 documents the gating contract. |
| **M-V5-1 networking** server-side `SetSocketTimeout(clientSocket, 5000)` hardcoded | **STILL OPEN (annotated defer)** | `ServiceEndpoint.hpp:150-162` carries the explicit `DEFER (audit M-V5-1 networking, ...)` block. Rationale pinned in-file: loopback-only RPC means the server-side polling timeout doesn't suffer the PBKDF2 cost the client side has to tolerate. Consolidate to a named constant on next touch. |
| **M-V5-3 networking** Probe classification covers 2 of 4 error strings | **STILL OPEN (annotated defer)** | `ServiceClient.hpp:323-333` carries the `DEFER (audit M-V5-3 networking, ...)` block. Today's Probe only targets the auto-registered Ping handler, so routes 3 ("Unknown method") and 4 ("Internal error") are not reachable in practice; classifier expansion fires when a non-Ping Probe is added. `ProbeClassificationTest.cpp:153-163` pins the current behavior for "Unknown method" so the deferral resolution must update the test. SessionCache's M-V5-2 networking fix (`SessionCache.hpp:148-184`) already splits these correctly on its end — when Probe expands, mirror that split. |

---

## Verified holding from prior cycles

| Item | Status | Evidence |
|---|---|---|
| C-V4-2 Probe response-body classification | CLOSED, regression test added | `ServiceClient.hpp:294-339`. `ProbeClassificationTest.cpp` covers 6 reachable outcomes (Ok / AuthFailed / EnvelopeVersionMismatch / 2× UnknownError / Unreachable). |
| M-V4-5 stoull-strict frame parse | CLOSED | `TcpSocket.hpp:449-452` requires `parsedChars == colonPos`. Both `MessageParser` (L387) and `RpcParser` (L34) delegate to `ExtractLengthFramed`; the single-point fix covers both. |
| M-V4-6 ServiceClient parser-clear on Send fail | CLOSED | `ServiceClient.hpp:212-225`. Post-failure invariant ("socket invalid → no buffered bytes") preserved. |
| M-V4-9 SessionCache envelope-error classify | CLOSED + expanded | `SessionCache.hpp:148-184` splits the channel-integrity vs deployment-skew buckets per M-V5-2. SessionCacheTest.cpp pins both "Unknown method" and "Internal error" cases. |
| H-V4-10 TcpServerBase connection caps + cleanup | CLOSED | `TcpServerBase.hpp:151-201` accept-loop check, M-V5-3 concurrency eager decrement at L563-589, cleanup-thread decrement removed (L654-666 comment explains). Two-phase Stop() join (L243-274) eliminates the LOCK2 deadlock that H-V5-2 concurrency flagged. |
| M-V5-9 connect timeout | CLOSED | `ServiceClient.hpp:84` `kConnectTimeoutMs = 3000`; `TcpSocket.hpp:289-409` `ConnectSocketWithTimeout` (non-blocking connect + select). Mutex hold ceiling = 3s connect + 5-30s recv. Test pinned in `ConnectTimeoutTest.cpp`. |
| M-V5-10 TCP keepalive on accepted sockets | CLOSED | `TcpSocket.hpp:110-144` `EnableTcpKeepAlive` + per-platform constants. Called at `TcpServerBase.hpp:196` for every accepted client. |
| Envelope version handling | CORRECT | `InternalRpcAuth.hpp:64` `APHELYON_ENVELOPE_VERSION = 1`. Server-side hard-reject at `ServiceEndpoint.hpp:283-289` returns `{"error":"unsupported_envelope_version","expected_v":1}`. Version is also part of the MAC input (`InternalRpcAuth.hpp:117-126`) so a skewed envelope fails MAC even if the explicit `v` check were bypassed (defense-in-depth). `EnvelopeShapeTest.cpp` is the golden-bytes anchor. |
| Port topology | CORRECT | Auth 7770 / Account 7773 / Combat 7774 internal; Auth 7777 / Account 7771 / Combat 7772 client TCP; Combat 7778 UDP (advertised). Auth's `main.cpp:146-151` passes `accountInternalPort=7773`, ctor at `AuthServer.hpp:35-49` correctly aligns. Account `main.cpp:238-241` passes `authInternalPort=7770, internalPort=7773`. Combat `main.cpp:110-112` passes `authInternalPort=7770, internalPort=7774`. No ctor-arg-order drift after Scope 1 batch l. |
| Token preflight | CORRECT | `TcpServerBase.hpp:607-614` rejects oversized tokens (>128 bytes) before session validation; null-byte payloads rejected at L600-601. The per-frame max payload (`m_maxPayloadSize` from protocol.json) is enforced at the parser layer at L524. |

---

## NEW findings

### [L-V6-1] Combat UDP port (7778) is advertised in `services.combat_udp` but no socket exists
**File:** `Server/Combat/src/main.cpp:110-117`, `Server/Combat/src/CombatServer.hpp:99-100,110`
**Status:** NEW (observational, pre-existing condition surfaced)
**Finding:** `main.cpp` logs `"Gameplay UDP port: {}"` and stores `udpPort` into `CombatServer`'s `m_udpPort`, and `AuthServer::HandleLogin` advertises `services.combat_udp.port = 7778` to every client at login (AuthServer.hpp:411). But `CombatServer` never opens a UDP socket — there is no `SOCK_DGRAM` call anywhere in Server/Combat/src. A client that takes the advertised endpoint at face value and `sendto(7778)` gets ICMP port-unreachable.
**Impact:** Pre-launch operational hazard only. Combat is documented as a stub (CombatServer.hpp:16-19, "transport TBD"; L100 logs `"Combat is a stub — match/arena systems not yet implemented"`). The client side similarly hasn't wired UDP yet. Risk surfaces if a client team starts integrating gameplay UDP before Combat opens the listener — they'll silently observe "no response" and may misattribute to firewall / NAT.
**Fix sketch:** Either (a) elide `services.combat_udp` from the login response until Combat opens the socket, or (b) open a UDP socket that responds to a minimal probe so the advertised endpoint is reachable. Probably (a) is right — match the broadcast surface to actual capability.

### [L-V6-2] `Message::ParseBody` uses partial `std::stoi` on the type-id field
**File:** `Server/Common/src/Net/Protocol.hpp:339`
**Status:** NEW (subtle parity gap with M-V4-5)
**Finding:** M-V4-5 networking closed `std::stoull` partial-parse on the outer LENGTH frame. The same defect class exists at the inner TYPE field — `body.substr(0, firstPipe)` is fed to `std::stoi` without checking the parsed-char count. `"123abc|tok|pld"` would parse as `type=123`, dropping the trailing `abc`. The frame already passed length validation; this only affects the per-message TYPE id classification.
**Impact:** Tiny. A type-id of `123abc` would silently become type 123. The dispatcher's `RequiresAuthentication` / `OnProcessMessage` routes go through `ProtocolLoader::Instance().GetMessageName(123)` and dispatch to whatever handler 123 maps to. A peer sending garbage in the type-id field gets a deterministic-but-wrong mapping, not an error. No exploit surface today because type-id space is dense (0-100) and clients ship one protocol.json. Hardening would be one line: capture `parsedChars` from `std::stoi`'s out-param and treat anything other than full-prefix consumption as `kInvalidMsgId`.
**Fix sketch:** Mirror the `TcpSocket.hpp:449-452` pattern at `Protocol.hpp:339`. Strict-parse the type field so a malformed prefix is a protocol error, not a silent mapping. Defer-acceptable.

### [L-V6-3] `RpcParser::TryExtract` uses default 65536 maxBodySize, not the protocol-loaded value
**File:** `Server/Common/src/Net/RpcFraming.hpp:38`
**Status:** NEW (observational)
**Finding:** `MessageParser::TryExtractMessage` (TcpServerBase.hpp:387-413) takes a `maxPayloadSize` parameter and is invoked at L524 with `m_maxPayloadSize` (protocol.json's `max_message_size`, 65536). `RpcParser::TryExtract` (RpcFraming.hpp:34-46) doesn't accept a cap and falls through to `ExtractLengthFramed`'s default of `MAX_RECEIVE_BUFFER_SIZE = 65536`. For internal RPC this is fine — loopback only, internal protocol is small JSON envelopes — but it's worth noting the asymmetry: public path is configurable; internal path is hardcoded.
**Impact:** None operationally. The internal RPC payload upper bound is 65536 (the receive buffer), which is generous for the current envelope shapes. Worth a comment if/when an internal RPC payload approaches that limit; consolidate caps into one named constant on next touch.
**Fix sketch:** Cosmetic. Either thread a `maxInternalPayload` cap through RpcParser, or add a comment at RpcFraming.hpp documenting that internal RPC inherits MAX_RECEIVE_BUFFER_SIZE as the de-facto cap.

### [L-V6-4] `ServiceEndpoint::Stop()` 10s/30s drain still detaches threads on overrun
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:212-232`
**Status:** REPEAT (carry-forward from H-V4-2; H-V5-2 closed the timing-budget side, not the overrun-then-detach side)
**Finding:** When the drain budget expires with active threads, `Stop()` logs a WARN and returns. The threads are detached (spawned at L182-187 with `connThread.detach()`), so they continue running against the destroying ServiceEndpoint instance. The H-V5-2 budget bump (Debug 30s) makes this overrun much less likely under normal handler runtime (Debug PBKDF2 ~18s + 5s recv timeout = 23s, well under 30s), but the overrun-detach UAF window remains structurally open.
**Impact:** Today the audit's own analysis showed the budget is now sized to (longest handler runtime + server recv timeout), so the overrun path is unreachable on normal flows. The risk surface is "a future handler exceeds the budget" or "a synthetic load test trips it" — neither is an immediate concern. The WARN log at L229-230 is the ops signal.
**Fix sketch:** Two structural options (v4 H-V4-2 enumerated these): (a) `join` instead of `detach`, holding `Stop()` until threads complete; (b) on overrun, force-close the connection sockets so the recv loops unblock and the threads exit. Defer-acceptable; revisit if overrun WARNs ever appear in production logs.

---

## Observations / Lows

- **Connect timeout is in place but mutex hold-time is still 3s + (5-30s recv)** — `ServiceClient::Call` holds `m_mutex` across `ConnectSocketWithTimeout` (3s) + send + recv (5s Release / 30s Debug). The probe sleeps OUTSIDE Call so retries don't hold the mutex through sleep. Single-thread-at-a-time RPC contract for up to 33s in Debug is the design tradeoff. L-V5-3 (v5) carry-forward.
- **`InternalRpcAuth::MAX_AGE_SECONDS = 30` symmetric replay window** — pinned at `InternalRpcAuth.hpp:69` with the same `DEFER (audit L-V5-3 security, ...)` comment block from v5. Loopback-only threat model, no exploit pre-launch. Revisit at first internal-RPC-over-network deploy.
- **Probe classification test (`ProbeClassificationTest.cpp`) pins UnknownError for "Unknown method"** — this is correct as the deferred behavior. SessionCacheTest.cpp covers the corresponding "Unknown method" + "Internal error" buckets at the consumer site. When M-V5-3 deferral resolves, both tests need updating in lockstep.
- **Length frame cap asymmetry** — `MAX_RECEIVE_BUFFER_SIZE = 65536`, `MAX_PAYLOAD_SIZE = 8192` (compile-time fallback), `m_maxPayloadSize` from protocol.json defaults to 65536 (matches the buffer). The static 8192 default fallback is only used if protocol.json fails to load, which would prevent service start anyway. Effectively the cap is 65536 on public + internal paths.
- **`Combat` ProbeStartupPeers identical-shape to Auth/Account** — `CombatServer.hpp:75-93` carries the same Release-fatal `Stop()` shape; H-V5-3's `m_cleanupThread` ordering fix protects all three sites.

---

## Suggested triage order

**Today / immediate:** None — networking dimension is in a clean state at HEAD.

**Pre-launch operational hygiene:**
1. **L-V6-1** — decide whether to elide `services.combat_udp` from login response (or open a stub UDP listener) so the advertised endpoint matches actual capability.
2. **L-V6-2** — strict-parse the type-id field in `Message::ParseBody` (mirror M-V4-5 pattern). One-line hardening on the public path.

**Track-but-don't-act (annotated deferrals):**
3. **M-V5-1 networking** — consolidate `SetSocketTimeout(clientSocket, 5000)` at ServiceEndpoint.hpp:162 into a named constant on next touch of the file. Pinned with `DEFER` comment.
4. **M-V5-3 networking** — when a non-Ping Probe target is added, expand classifier to surface `MethodNotFound` + `InternalServerError` outcomes (mirror SessionCache's M-V5-2 split). Pinned with `DEFER` comment + test fixture in place.
5. **L-V6-4** — revisit `ServiceEndpoint::Stop()` overrun-detach if ops ever observe drain WARN lines in production.

---

## Test coverage status (networking-specific)

Tests verified present:
- `Server/Common/tests/ProbeClassificationTest.cpp` — 6 cases, covers all 4 currently-reachable Probe outcomes + 1 deferred + 1 unreachable scenario.
- `Server/Common/tests/SessionCacheTest.cpp` — covers M-V5-2 channel-integrity vs deployment-skew split for all 4 error strings.
- `Server/Common/tests/TcpServerCapsTest.cpp` — H-V4-10 connection-cap refusal paths.
- `Server/Common/tests/ServiceEndpointCapsTest.cpp` — M-V5-7 loopback-cap refusal path.
- `Server/Common/tests/ConnectTimeoutTest.cpp` — M-V5-9 connect-timeout boundary.
- `Server/Common/tests/EnvelopeShapeTest.cpp` — envelope-version golden-bytes anchor.
- `Server/Common/tests/InternalRpcAuthRoundTripTest.cpp` — MAC verification + replay window.

The v5 networking test-coverage gap (H-V5-8 sub-items for connection caps + Probe classification + InvokeForTest exposure) is now closed at the file level. The Stop-Start lifecycle regression that would mechanically catch the H-V5-3 SIGABRT remains unwritten (low priority — the spawn-ordering fix is structurally simple and the WARN log path at TcpServerBase.hpp:121 surfaces the early-shutdown to main()).

---

## System-level Verdict

**A−.** The Scope-1/Scope-2 batches plus the v5-top-3 lifecycle fixes shipped clean. Two carry-forward deferrals are explicit (annotated at call sites with `DEFER (audit ref: ...)` blocks and test fixtures pinning current behavior). New findings are observational: a Combat UDP-port advertise gap and a parity-pattern partial-parse on the public `type` field. No active networking regression in code.

The v6 networking dimension's recommended action is **none urgent** — the deferred items are correctly deferred, the new lows are pre-launch hygiene, and the post-Scope-1/2 baseline is the cleanest the dimension has been across the v3→v6 audit arc.
