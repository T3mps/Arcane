# v6 followup — Cross-cutting + Test Coverage

**Date:** 2026-06-04 (same-day successor to v5; covers commits `88a9346..47d16f0` — the v5-medium-batch arc a-r plus prior H-V5 fixes)
**Scope:** v5 cross-cutting + test-coverage carry-forwards; vocabulary integrity; service-to-service contract surfaces; spec consistency; carry-forward noise floor.
**Method:** Walked the 30+ post-v5 commits, verified each H-V5-8 sub-item by file inspection, ran `--list-tests` and full suites for the four test binaries, cross-walked `Server/data/protocol.json` vs `Client/data/protocol.json`, audited `IdOrThrow` adoption, and inspected the v5-medium-tail spec for post-Scope-4 accuracy.

---

## Status of v5 items in this dimension

| Item | Verdict | Evidence |
|---|---|---|
| **H-V5-8.a** TcpServerBase per-IP cap refusal | **CLOSED (partial)** | `Server/Common/tests/TcpServerCapsTest.cpp` — 3 cases pin per-IP cap (default + small-cap-via-ctor + negative-space). **Total cap refusal still NOT covered** (test file comment explicitly defers it: "the total cap requires a runtime-injectable threshold to test cleanly"). |
| **H-V5-8.b** OutboxRelay 3 sweep methods | **CLOSED** | `Server/Account/tests/Integration/OutboxRelaySweepTest.cpp` — 3 cases, one per method (`PruneDispatchedOutbox`, `SweepExpiredIdempotency`, `RunPartmanMaintenance`). |
| **H-V5-8.c** Probe 4-outcome classification | **CLOSED** | `Server/Common/tests/ProbeClassificationTest.cpp` — 6 cases (Ok, AuthFailed, EnvelopeVersionMismatch, two UnknownError shapes, Unreachable). |
| **H-V5-8.d** InvokeForTest backdoor | **CLOSED (gated)** | `ServiceEndpoint.hpp:103` wraps the method in `#ifdef APHELYON_TEST_BUILD`. Production builds compile without it — structural impossibility, not a runtime check. |
| **H-V5-8.e** outbox.account_id population | **CLOSED** | `Server/Account/tests/Integration/OutboxAccountIdTest.cpp` — 2 cases (NOT NULL post-commit + FK CASCADE on hard-delete). |
| **M-V5-2 cross-cutting** events.schema_version vocab drift | **DEFER (annotated)** | `schema.sql:311-323` has an explicit DEFER block with "revisit when" criteria + spec line ref. Acceptable pre-launch disposition. |
| **M-V5-4 cross-cutting** `ProtocolLoader::Id` silent-miss | **PARTIAL** | `IdOrThrow` added at `Protocol.hpp:244`; **45 handler-ctor adoption sites** (Auth/Account handlers all switched). **TcpServerBase still uses raw `Id("ErrorResponse")` / `Id("SessionExpired")` at lines 348/354** — hot path, called on every error response. |

---

## Test counts at HEAD

Ran each binary on commit `47d16f0`:

| Binary | Cases | Assertions | Δ vs v5 baseline (117/779) |
|---|---|---|---|
| AccountTests | 108 | 807 | +11 / +68 |
| CommonTests | 38 | 289 | +21 / +256 |
| AuthTests | 12 | 34 | +10 / +28 |
| CombatTests | 1 | 1 | 0 / 0 |
| **Total** | **159** | **1131** | **+42 / +352** |

The H-V5-8 test backfill + the M-V5-4 concurrency regression tests landed the +42 cases. Common's growth (+21) is the lion's share — SessionCacheTest + ProbeClassificationTest + ServiceEndpointCapsTest + TcpServerCapsTest extensions + ConnectTimeoutTest.

---

## NEW findings

### H-V6-1. EffectDispatcher EmitToOutbox sites bypass OutboxRelay::Register; future activation accumulates undispatched rows
**Files:** `Server/Account/src/Effects/EffectDispatcher.hpp:41,49,83` (3 destination strings); `Server/Account/src/Db/OutboxRelay.hpp:62-83` (assumption that "no production handler invokes `EmitToOutbox`")
**Status:** NEW (regression of H-V5-2's documented assumption — the assumption was already stale at HEAD).

`EffectDispatcher` calls `txn_.EmitToOutbox(...)` for three destinations:
- `"notifications.toast"` (line 41)
- `"telemetry.event"` (line 49)
- `"inventory.grant_material"` (line 83)

OutboxRelay's class-level comment block (lines 62-83) and the H-V5-2 carry-forward both assert "no production handler invokes `EmitToOutbox`" and use that as the rationale for the deferred `Register` contract. That assertion is **already false in source** — `EffectDispatcher` is the producer. Today, `EffectDispatcher` itself is not instantiated by any production handler (`grep -n EffectDispatcher Server/Account/src` returns the class definition only), so the destinations are dead-by-disuse — but the moment the first handler creates an `EffectDispatcher` (the obvious next step for `HandlePull`'s reducer side-effect wiring), the three destinations activate. Without matching `m_outboxRelay.Register("notifications.toast", handler)` calls, `PumpOnce` will skip every row (it does `if (!handler) continue` at `OutboxRelay.hpp:182`), and rows accumulate with `dispatched_at IS NULL` until `PruneDispatchedOutbox` — which only sweeps **dispatched** rows — leaves them forever.

**Impact:** The same operational hazard C-V4-1 closed (unbounded `outbox` growth) re-opens silently the first time `EffectDispatcher` is wired into a live handler. Because the dispatcher and the destinations are already implemented, this is a true "code is ready, wiring not" gap — the OutboxRelay.hpp comment treats `Register` as future-work, but the producer side has caught up.

**Fix sketch:** either (a) wire the three destinations now with no-op handlers + a "TODO when notifications/telemetry/inventory services land" log line, so the dispatch loop empties rows instead of accumulating; or (b) implement the startup self-check the v5 audit recommended (refuse to start when zero destinations registered AND outbox row count > 0); or (c) update the OutboxRelay header comment to acknowledge `EffectDispatcher` as the live producer and convert the H-V5-2 deferral into "wire before next `EffectDispatcher` consumer site lands."

### H-V6-2. Client/data/protocol.json diverged from Server/data/protocol.json — CompleteQuest is server-only
**Files:** `Server/data/protocol.json` (defines `CompleteQuest` + `CompleteQuestResponse`); `Client/data/protocol.json` (missing both); `Client/src/network_tcp.lua:1838-1878` (calls `conn:send(MessageTypes.CompleteQuest, ...)`)
**Status:** NEW

The Editor (`Tools/Editor/src/DataStore.hpp:37`) registers `protocol.json` only against the Server root — there is no Client-side write path. The two files must be kept in sync manually, and `CompleteQuest` proves they drifted:

- `python -c "json.load(open(srv))['messages']" - json.load(open(cli))['messages']` ⇒ `['CompleteQuest', 'CompleteQuestResponse']` are server-only.
- `MessageTypes` is built from `Client/data/protocol.json` at `Client/src/network_tcp.lua:110-113`, so `MessageTypes.CompleteQuest` is **nil** on the client.
- `conn:send(MessageTypes.CompleteQuest, ...)` at `network_tcp.lua:1850` would send with a nil type — silently dropped at the receive-side type-router or routed as id 0 depending on `conn:send`'s coercion. Either way, the client's `completeQuest` callback never fires.

The 42 shared messages all have matching IDs (verified by walking each name's `.id` field), so the drift is **pure missing-message**, not ID collision. Still, the silent-drift surface is exactly what M-V5-4 cross-cutting flagged — message vocabulary divergence with no startup-time validation.

**Impact:** The "CompleteQuest" path on the client is silently broken (or has been silently broken since the message was added). Given the v5 batch-r commit history, this could be a stale message name (the protocol may have settled on `ClaimQuestReward` as the canonical client-facing claim, with `CompleteQuest` being an internal-only Account RPC). Either way, the source-of-truth gap is structural.

**Fix sketch:** either (a) `cp Server/data/protocol.json Client/data/protocol.json` if the server is canonical (one-line copy in the Editor's flush path, mirroring how strings.json is client-side); or (b) add a startup CI check that fails the build when `diff -q` reports the two files differ; or (c) collapse to a single file with a per-message `client:bool` flag and dual-emit. (a) is the cheapest closure that matches the Editor's current single-source pattern.

### M-V6-1. TcpServerBase::CreateErrorResponse / CreateSessionExpiredResponse still use raw `Id()` (M-V5-4 carry-forward, hot path)
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:348,354`
**Status:** REPEAT (M-V5-4 cross-cutting carry-forward; partial closure)

`IdOrThrow` adoption is broad — 45 cached-ID sites in `AuthServer.hpp` / `AccountHandlers.hpp` / `GachaHandlers.hpp` / `QuestHandlers.hpp` / `ProgressionHandlers.hpp` switched. But the two hot-path lookups in `TcpServerBase` still use raw `Id(...)`:

```cpp
return CreateResponse(ProtocolLoader::Instance().Id("ErrorResponse"), response.dump());      // L348
return CreateResponse(ProtocolLoader::Instance().Id("SessionExpired"), response.dump());     // L354
```

Two issues:
1. **Silent failure**: A protocol.json edit renaming `ErrorResponse` would send id 0 on every server error. No throw, no startup log, no per-call log. Same shape as M-V5-4 at every other site that did adopt `IdOrThrow`.
2. **Hash-map lookup per call**: every `CreateErrorResponse` and `CreateSessionExpiredResponse` invocation hits the `ProtocolLoader` map. Static one-time cache at first use closes both.

**Fix sketch:**
```cpp
static const MsgId kErrorResponseId = ProtocolLoader::Instance().IdOrThrow("ErrorResponse");
static const MsgId kSessionExpiredId = ProtocolLoader::Instance().IdOrThrow("SessionExpired");
```
~6 LOC. Throws at first error-response time (still pre-traffic) if the protocol.json drifts.

### L-V6-1. TcpServerBase total-cap (MAX_CONNECTIONS_TOTAL) refusal branch still untested
**Files:** `Server/Common/src/Net/TcpServerBase.hpp` accept-loop total-cap branch; `Server/Common/tests/TcpServerCapsTest.cpp:11-12` comment explicitly defers the total-cap test.
**Status:** PARTIAL (per-IP closed; total still open)

The test file comment says: "the total cap requires a runtime-injectable threshold to test cleanly and is deferred per the audit's recommendation." But the per-IP small-cap test (line 186) already takes a `maxConnTotal` ctor arg — the same mechanism could test a small total cap if the test opened connections from multiple ephemeral source ports (loopback supports many). Doable in ~40 LOC by varying the source port via `bind` before `connect`. Pre-launch the gap is observational; the per-IP path is the realistic attack surface.

---

## Observations / Lows

- **AccountServer.hpp audit-tag density dropped to 22/399 = 5.5%** (down from v5's 28/398 = 7.0%). The batch-k followup (`1644d92`) normalized casing, which collapsed some duplicates. The "extract to `AccountServer_DESIGN.md` if it climbs 5+ more tags" trigger has not fired; do not re-flag.
- **Spec-vs-code consistency is clean.** `docs/superpowers/specs/2026-06-04-v5-medium-tail-scoping-design.md` was rescoped in commit `bd1f670` from "snapshot pattern" to "per-account locks refactor" with the rationale (`GachaHandlers.hpp:270-273` idempotency invariant) pinned in the design's prose. Scope 4 was implemented across batches m-r and the spec accurately predicts the implementation.
- **Service-to-service RPC vocabulary is clean.** Three named methods registered (`CreateAccount`, `VerifyCredentials` on Account; `AuthorizeToken` on Auth) plus auto-`Ping`. Three named methods called (matching producers). One method (`GetPartyData`) is registered on Account but only referenced in tests + a comment — no production caller (low-impact dead surface; not worth flagging as a separate finding).
- **Carry-forward noise floor** — counted v5 Mediums explicitly deferred-with-rationale (`bc05aee` + `d8c1af4` annotation passes): persistence M-V5-4, M-V5-5; networking M-V5-1, M-V5-3; concurrency M-V5-1; idempotency M-V5-2. The DEFER markers carry the "revisit when" criteria inline, so a re-audit can verify the trigger condition rather than re-litigate. The noise floor is **structurally falling** — about 6 Mediums are now DEFER-annotated rather than re-stating them as open Lows. The "asymptote" v5 observed is being chipped at by annotation.
- **Configurable-constants candidates beyond M-V5-6:** v5 batch-l made `max_connections_per_ip` / `max_connections_total` configurable via `protocol.json.settings`. Two more constants with similar pre-launch-ops-flexibility shape: (a) `ServiceEndpoint::kMaxInternalConnections` (currently `constexpr 64`, hardcoded), (b) `ServiceEndpoint`'s server-side 5000 ms recv timeout (M-V5-1 networking deferral). Both have DEFER annotations already; no new finding, just noting the candidate set per the audit prompt.
- **events.event_type vocabulary surface unchanged** — still emitted via free-form strings at 7 handler sites (`AccountHandlers.hpp:418`, `GachaHandlers.hpp:229/261/576/602`, `ProgressionHandlers.hpp:528`, `QuestHandlers.hpp:456`, plus `EffectDispatcher.hpp:131`). No reducer dispatches on the string value. The M-V4-3 silent-drift surface persists; deferring per the M-V5-2 cross-cutting DEFER comment in schema.sql is the documented disposition.

---

## Suggested triage

**Before next deploy:**
1. **H-V6-1** — wire OutboxRelay handlers (or no-op stubs) for the three EffectDispatcher destinations before the first EffectDispatcher consumer ships. The C-V4-1 hazard re-opens silently otherwise.
2. **H-V6-2** — sync `Client/data/protocol.json` with `Server/data/protocol.json` and either pin them with a CI diff check or add a Client-side write path in the Editor.

**This week:**
3. **M-V6-1** — cache `ErrorResponse` / `SessionExpired` MsgIds via `IdOrThrow` in TcpServerBase. ~6 LOC; closes the M-V5-4 carry-forward fully.

**Backlog:**
4. **L-V6-1** — total-cap test via multi-source-port loopback. Pre-launch optional.
