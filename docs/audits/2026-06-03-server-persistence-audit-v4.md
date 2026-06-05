# Aphelyon Server + Persistence Audit — v4 (Follow-up to v3)
**Date:** 2026-06-03 (same-day successor to v3, after the 35-commit v3 remediation arc)
**Scope:** Same as v3 — Server/Account, Server/Auth, Server/Combat, Server/Common, schema.sql, Client/src/network_tcp.lua + Client/src/services, Server/Account/tests
**Method:** 9 parallel general-purpose audit agents (per-dimension). Each agent read v3 synthesis + v3 per-dimension followup + v2 synthesis, then audited the current code state. v4 added a 9th dimension (networking) that v3 distributed across concurrency/security/cross-cutting.

**Per-dimension findings:**
1. Event sourcing + atomicity → `2026-06-03-v4-followup-event-sourcing.md`
2. Persistence + schema → `2026-06-03-v4-followup-persistence.md`
3. Concurrency + lifecycle → `2026-06-03-v4-followup-concurrency.md`
4. Idempotency + retry → `2026-06-03-v4-followup-idempotency.md`
5. Error handling + rollback → `2026-06-03-v4-followup-error-handling.md`
6. Security → `2026-06-03-v4-followup-security.md`
7. Code quality + complexity → `2026-06-03-v4-followup-code-quality.md`
8. Test coverage → `2026-06-03-v4-followup-test-coverage.md`
9. Cross-cutting / integration → `2026-06-03-v4-followup-cross-cutting.md`
10. Networking + service lifetime (NEW dimension) → `2026-06-03-v4-followup-networking.md`

---

## Executive Summary

The v3 remediation arc (35 commits closing 2 Critical, 14 High, ~25 Medium, ~30 Low) is **largely verified closed in code by independent agents**, but **two of the v3 closures are functionally dead** because the receiving glue was never wired. Both came out of commits I shipped today. Plus one defect of the **same shape as v3-H-V3-1 lives in a fourth handler** the v3 sweep missed.

The headline:

- **C-V4-1** — `OutboxRelay` is never instantiated. H-V3-4 (outbox prune), H-V3-5 (idempotency sweep + partman maintenance), and the 24-month partman retention are **non-functional code**. `outbox`, `idempotency_cache`, and `events` partitions all still grow unboundedly. The class exists in `Server/Account/src/Db/OutboxRelay.hpp` — fully implemented, all three sweep methods present — but there is no `m_outboxRelay` member on `AccountServer`, no `new OutboxRelay`, no instantiation anywhere. ~3 months post-launch behavior matches a world where v3 H-V3-4/5 was never shipped.

- **C-V4-2** — `ServiceClient::Probe` collapses MAC-rejected and envelope-version-mismatched server responses into `Ok`. The server-side rejection returns `{"error": "Authentication failed"}` or `{"error": "unsupported_envelope_version"}` as a valid JSON envelope — which `Call()` correctly classifies as `RpcCallStatus::Success` (the round-trip worked; the error is in the payload). `Probe` only inspects `rpc.ok()` and never reads `rpc.value["error"]`, so it never returns `AuthFailed` or `EnvelopeVersionMismatch`. Those enum members are dead code. The Release-fatal canary catches unreachability but **silently accepts secret mismatch and version skew**. H-V3-14(b)'s headline observability promise — "tells you four things in one call" — currently delivers one.

- **H-V4-1 event-sourcing** — `HandleClaimQuestReward` calls `TickQuests::Apply` (mutating) at `QuestHandlers.hpp:579`, BEFORE `Begin()` at `:605`. Same C7-A scope-creep that H-V3-1 fixed for `HandleSetParty`, `HandleReportQuestProgress`, and `HandleCompleteQuest`. The v3 sweep enumerated three handlers; the claim path is the fourth.

**Tally (raw, before de-dup across dimensions):** 2 Critical, 19 High, ~54 Medium, ~94 Low.

**De-duplicated unique findings:** 2 Critical, ~10 High, ~40 Medium, ~70 Low. The high overlap comes from the Probe defect being flagged by networking (C), security (H), and cross-cutting (H) independently; and the claim-path TickQuests defect flagged by event-sourcing, concurrency, and error-handling.

**Verdicts by dimension:**

| Dimension | Verdict | C/H/M/L |
|---|---|---|
| Event sourcing | concerning | 0/1/4/6 |
| Persistence | clean | 0/2/5/10 |
| Concurrency | clean | 0/1/3/8 |
| Idempotency | concerning | 1/3/7/8 |
| Error handling | clean | 0/0/0/9 |
| Security | clean | 0/1/6/10 |
| Code quality | clean | 0/0/4/11 |
| Test coverage | concerning | 0/7/10/9 |
| Cross-cutting | clean | 0/1/5/8 |
| Networking | concerning | 1/3/10/15 |

**Audit-vs-fix scorecard (v3 → v4):**
- v3 Critical (C-V3-1, C-V3-2): both closed in code.
- v3 High (14): 13 closed in code. H-V3-4 + H-V3-5 land as **dead code** because OutboxRelay was never wired into AccountServer's startup. H-V3-14(b)'s Probe classification is partial.
- v3 Medium (~25): all closed.
- v3 Low (~30): all closed except the handful explicitly deferred with rationale.
- **NEW: 2 Critical, ~10 High unique, ~40 Medium unique, ~70 Low unique.**

**Systemic risk surfacing this pass:**
- **Wiring-vs-implementation gap.** Both Critical findings are "class is correct, integration is missing." OutboxRelay's three sweep methods are individually well-written; they just never run. Probe's classification logic is sound for the cases it handles; it just doesn't inspect the response body.
- **Test velocity is improving.** v3→v4 shipped TC-V3-1 (IntegrationDbFixture + 18 test conversions), TC-V3-4 (10 MAC round-trip + golden-file envelope tests), plus regression tests for C-V3-1 and H-V3-11. The v2→v3 ratio was 1-in-22-commits; v3→v4 is meaningfully better. But the **directly-extracted M-V2-3 classes still have zero direct tests** (AccountCache, AccountHydrator, InternalRpcHandlers).
- **Carry-forwards continue to dominate the Low backlog.** ~30 of the ~70 unique Low items are restatements of v2 or v3 items. The audit response pattern leaves them; the noise floor doesn't drop.

---

## CRITICAL (fix before next deploy)

### C-V4-1. OutboxRelay class is fully implemented but never instantiated
**File:** `Server/Account/src/Db/OutboxRelay.hpp` (class), `Server/Account/src/AccountServer.hpp` (missing instantiation), `Server/Account/src/main.cpp` (missing instantiation)
**Source:** idempotency dimension C-V4-1
**Status:** NEW

The class works. `PumpOnce` dispatches; `PruneDispatchedOutbox(24h)` deletes old dispatched rows; `SweepExpiredIdempotency()` deletes expired idempotency_cache rows; `RunPartmanMaintenance()` calls `partman.run_maintenance_proc()`. The `Run()` worker loop interleaves them on a counter cadence (every 120 / 130 / 7200 pumps).

But:

```bash
$ grep -rn "OutboxRelay" Server/Account/src/
Server/Account/src/Db/OutboxRelay.hpp:29:class OutboxRelay {
Server/Account/src/Db/OutboxRelay.hpp:31:    explicit OutboxRelay(...)
Server/Account/src/Db/OutboxRelay.hpp:38:    OutboxRelay(...) = delete;
... (only the class file itself; no callers)
```

No `m_outboxRelay` member on AccountServer. No call to `new OutboxRelay` or `OutboxRelay relay(...)` in `main.cpp` or `AccountServer`'s constructor. The class compiles into the binary and is never used.

Operational consequence:
- `outbox` rows never pruned → unbounded growth (~bytes per RPC × DAU × time)
- `idempotency_cache` rows never swept → ~15MB/day at 1k DAU growth, expires_at filter hides them from reads but they consume disk + index space
- `partman.run_maintenance_proc()` never called → new monthly partitions stop being pre-created once the 12 bootstrap partitions are consumed (~12 months post-launch). After that, INSERTs into `events` fail with "no partition matching key in partitioned table".

**Fix:** instantiate as an `AccountServer` member after `m_pool`:
```cpp
db::OutboxRelay m_outboxRelay{m_pool};
```
Member-init places it after the pool's construction. H-V3-7's explicit `Stop()` in `~AccountServer` ensures the relay's destructor runs while the pool is still alive. Verify with: after a Debug run, check `pg_partman.run_maintenance_proc` was called by inspecting `partman.part_config.last_run` or by checking that a new partition was created at month boundary.

### C-V4-2. ServiceClient::Probe never returns AuthFailed or EnvelopeVersionMismatch
**File:** `Server/Common/src/Net/ServiceClient.hpp` (Probe method, ~lines 217-265)
**Source:** networking C-V4-1, security H-V4-1, cross-cutting H-V4-1 (three independent flags)
**Status:** NEW

H-V3-14(b)'s commit message claimed the Probe surfaces five outcomes: `Ok`, `Unreachable`, `AuthFailed`, `EnvelopeVersionMismatch`, `UnknownError`. Mechanically, only three are reachable:
- `Ok` — when the server's response is parseable JSON, regardless of whether it's a success or an error
- `Unreachable` — when `rpc.unreachable()` (connect refused / backoff)
- `UnknownError` — when `rpc.wireError()` (send fail / recv timeout / parse error)

ServiceEndpoint's response to a version-skewed or MAC-rejected envelope is:
```cpp
response = {{"error", "unsupported_envelope_version"}, {"expected_v", ...}};
// or
response = {{"error", "Authentication failed"}};
```

Both are well-formed JSON. `ServiceClient::Call` round-trips them as `RpcCallResult{Success, json}`. `Probe` checks `rpc.ok()` and returns `Ok`. The server's distinctive error string is in `rpc.value["error"]` but Probe never reads it.

Operational consequence: a Release deployment with mismatched `APHELYON_INTERNAL_SECRET` between Auth and Account would **probe-succeed** at startup, then every real RPC fails with MAC rejection. The canary's "tells you four things in one call" claim delivers one: reachability.

**Fix:** after `rpc.ok()`, inspect `rpc.value`. If it contains `"error" == "unsupported_envelope_version"` return `EnvelopeVersionMismatch`. If `"error" == "Authentication failed"` return `AuthFailed`. Otherwise, if it lacks a `"result"` field, return `UnknownError`. Else `Ok`.

```cpp
if (rpc.ok()) {
    const auto err = rpc.value.value("error", std::string{});
    if (err == "unsupported_envelope_version") return ProbeOutcome::EnvelopeVersionMismatch;
    if (err == "Authentication failed")        return ProbeOutcome::AuthFailed;
    if (!err.empty())                          return ProbeOutcome::UnknownError;
    if (!rpc.value.contains("result"))         return ProbeOutcome::UnknownError;
    return ProbeOutcome::Ok;
}
```

The MAC round-trip itself verifies (the server signed the response and ServiceClient's Verify accepted it), so a successful-shape error response is still authenticated — `AuthFailed` only fires on the server-side method-call MAC check rejecting the inbound request, not on response tampering.

---

## HIGH

### H-V4-1. HandleClaimQuestReward runs TickQuests::Apply BEFORE Begin() — same as H-V3-1 missed one handler
**Files:** `Server/Account/src/Handlers/QuestHandlers.hpp:579` (TickQuests::Apply) vs `:605` (Begin())
**Source:** event-sourcing H-V4-1, concurrency H-V4-1, error-handling L-V4-1 (three independent flags)
**Status:** NEW (scope-miss of H-V3-1)

The v3 H-V3-1 fix moved `Begin()` above `TickQuests::Apply + questStates.Upsert + mutations` in `HandleSetParty`, `HandleReportQuestProgress`, and `HandleCompleteQuest`. The v3 audit's M-V3-3 cross-cutting noted that v3's enumeration "found three sibling handlers" — but `HandleClaimQuestReward` is the fourth handler that uses `TickQuests::Apply`, and the v3 sweep missed it.

Lines 579 and 605:
```cpp
579:  TickQuests::Apply(account, m_ctx.questLoader, std::time(nullptr), playerId);
580:
581:  if (!account.GetQuestStates().Has(questId)) ...
605:  auto txn = m_ctx.repository->Begin(account);
```

`TickQuests::Apply` mutates `questStates` (auto-completing rollover transitions, unlocking quests, etc.) before the Memento snapshot. The snapshot captures post-tick state. Rollback restores to post-tick — the tick mutations stay live. If a future change makes the tick's mutations un-idempotent on retry, this becomes a bug; today the tick is idempotent enough that it doesn't visibly bite.

**Fix:** same mechanical move H-V3-1 applied — `Begin()` above the `TickQuests::Apply` call, after the idempotency cache check (lines 558-577 must stay above because they short-circuit before any mutation). The IsClaimed / IsCompleted checks (lines 585-586) are reads on `q` which is mutable; they can move below Begin() too.

### H-V4-2. AccountRepository::Create overwrites RETURNING created_at — silent drift between in-memory and DB
**File:** `Server/Account/src/Cache/AccountRepository.hpp` (Create method)
**Source:** persistence H-V4-1
**Status:** REPEAT (from v2 followup-persistence M1, v3 followup-persistence M-V3-1, third audit round unactioned)

`INSERT INTO accounts ... RETURNING account_id, public_uid, created_at` returns the DB-assigned `created_at` (schema DEFAULT now()). `Create` then overwrites the returned struct's `createdAt` field with `std::time(nullptr)` instead of preserving the DB value. Drift between in-memory and DB is small (sub-second) but non-zero, and the C++ side now sees a value the DB doesn't have.

**Fix:** preserve the RETURNING value; drop the `time(nullptr)` write.

### H-V4-3. outbox table has no account_id FK or index
**File:** `Server/Account/schema.sql` (outbox table, lines 336-344)
**Source:** persistence H-V4-2
**Status:** NEW

`outbox` rows have `destination TEXT` and `payload JSONB` — the account ID is buried inside the payload. There is no `account_id` column, no FK to `accounts`. When the TC-V3-1 integration fixture (and a future GDPR hard-delete pathway) delete an account, outbox rows referencing it survive. The current OutboxRelay doesn't filter by account anyway (it walks all undispatched rows), so no immediate functional bug, but a real GDPR-style "delete all data for user X" capability would need a JSONB-key scan instead of a CASCADE.

**Fix:** add `account_id BIGINT REFERENCES accounts(account_id) ON DELETE CASCADE` to `outbox`. Index it.

### H-V4-4. ServiceClient 5s recv timeout < Debug PBKDF2 ~18s
**File:** `Server/Common/src/Net/ServiceClient.hpp:86` (SetSocketTimeout 5000), interacts with `Crypto::DEFAULT_ITERATIONS = 200000`
**Source:** networking H-V4-1
**Status:** NEW

Documented in commit `0a92bd5`. Debug builds of `Crypto::VerifyPassword` take ~18s per call (200k PBKDF2 iterations × Debug-build per-iteration cost). ServiceClient's 5s recv timeout fires first. Every Auth→Account `VerifyCredentials` call in Debug fails with `wire_error`, then Auth returns "Account service unavailable" to the client.

Release behavior: 200k iterations × Release per-iteration cost ≈ 2s, well under 5s budget. Not a Release bug; an iteration-loop problem for dev.

**Fix options:** (a) bump ServiceClient timeout in Debug only (`#ifndef NDEBUG`), (b) reduce PBKDF2 iterations in Debug, (c) async ServiceClient (out of scope).

### H-V4-5. AccountCache / AccountHydrator / InternalRpcHandlers still have zero direct tests
**Files:** Tests directory (`Server/Account/tests/Integration/`) does not contain dedicated test files for these three classes
**Source:** test-coverage H-V4-2
**Status:** REPEAT (v3 TC-V3-2; H-V3-13 premake unblock landed but no tests followed)

M-V2-3 extracted these three classes specifically to be testable. The IntegrationDbFixture (TC-V3-1) now unblocks them. No tests have been written. The C1 stale-erase-and-reload contract (the load-bearing AccountCache invariant) remains exercised only by full-stack handler tests.

**Fix:** at minimum one test per class:
- `AccountCacheTest.cpp` — stale-flag round-trip (mark stale → next GetLockedAccount evicts and reloads from DB)
- `AccountHydratorTest.cpp` — load → set state → save → reload assertion
- `InternalRpcHandlersTest.cpp` — Verify path round-trip via mock ServiceEndpoint

### H-V4-6. C-V3-2 fix shipped without regression test
**Files:** `Server/Account/tests/` (no test asserts the Begin()→GetPity invariant)
**Source:** test-coverage H-V4-1
**Status:** NEW

C-V3-2's fix moved `Begin()` above `GetPity/GetGuarantee` in HandlePull/HandleMultiPull. No test asserts this position. A future refactor that re-introduces the lazy-mutate-before-Begin pattern would land silently.

**Fix:** an integration test that exercises a first-pull-on-slot under Commit failure (e.g., force a ConcurrencyConflict on the AppendInTx), then asserts that the rolled-back Account has `m_pityBySlot` empty for that slot.

### H-V4-7. v2's prioritized top-5 #3 and #5 unaddressed despite IntegrationDbFixture unblocking them
**Source:** test-coverage H-V4-3 + H-V4-4
**Status:** REPEAT (v2 #3 and #5, v3 noted both as missing)

Specifically:
- AccountRepository populated round-trip (load an account with characters/weapons/gear/quests, assert all fields round-trip)
- End-to-end HandleAddCurrency through DB (exercise the AccountTransaction commit chain end-to-end)

Both are now trivially writable on top of `TEST_CASE_METHOD(IntegrationDbFixture, ...)`. The fixture's `repo` member is the entry point.

### H-V4-8. No tests for idempotency machinery
**Source:** idempotency H-V4-3
**Status:** REPEAT (v3 TC-V3-3 subset)

The idempotency_cache layer has zero direct tests. `StoreIdempotency` → `FindIdempotency` round-trip, `ON CONFLICT DO UPDATE` refresh semantics, sibling-key construction across all 7 RPCs that depend on them, cache-hit short-circuit paths — none tested. Idempotency bugs (V3-L3 daily-quest reset, M-V3-4 sibling-key direction) would land silently.

### H-V4-9. M-V2-7 point-in-time documentation still only names wallet balances
**Source:** idempotency H-V4-2
**Status:** REPEAT (v3 M-V3-2 idempotency)

M-V2-7 introduced the doc-comment about idempotent retries returning point-in-time data ("the retry must replay the original response"). The comment names wallet balances. Actual cached fields span 14-16+ values including streak counters, story state, difficulty tier, pity, guarantee, and (for AddCurrency) the full BuildStatePayload snapshot. Client trust assumes "this is current" — the cached retry returns "this was current at T1." Documentation gap, not a correctness bug, but exposes the same trap the original comment closed.

### H-V4-10. No connection cap on TcpServerBase or ServiceEndpoint
**Files:** `Server/Common/src/Net/TcpServerBase.hpp`, `Server/Common/src/Net/ServiceEndpoint.hpp`
**Source:** networking H-V4-3
**Status:** NEW (pre-launch concern)

The accept loops spawn one thread per connection with no per-IP cap and no total-connection cap. Each thread holds ~1.1MB of stack. A slow-loris attacker sending one byte per second per connection from 1000 IPs costs the attacker nothing and the server 1.1GB + 1000 threads. The ServiceEndpoint is 127.0.0.1-bound so loopback-only; the TcpServerBase is the public-facing concern.

**Fix:** add `MAX_CONNECTIONS_TOTAL` and `MAX_CONNECTIONS_PER_IP` caps; refuse on overflow.

---

## MEDIUM (selected; full lists in per-dimension reports)

### Event sourcing
- **M-V4-1.** EffectDispatcher H10 latent dual-emitter cursor collision unchanged from v2/v3 (dead-coded today).
- **M-V4-2.** `EventStore::AppendIdempotent` recheck opens a second pqxx::work without re-acquiring the advisory lock; live commits don't route through it, but inconsistent with H-V3-2's hoist.

### Persistence
- **M-V4-1.** `accounts.reducer_version` is a dead column (only `snapshots.reducer_version` is read). Same defect class as the `last_login_claim_at` columns M-V3-3 dropped.
- (4 more in per-dimension report)

### Concurrency
- **M-V4-1.** `ServiceEndpoint::Stop()` 10s detached-thread drain hard-coded; H-V3-7 materially narrows the UAF window but doesn't close it.
- **M-V4-2.** Stripe lock held through full multi-pull/claim handler body — 1/64 false-contention surface.

### Idempotency
- **M-V4-1.** Documented client `idempotencyKey` parameter present in `Client/src/network_tcp.lua` but no UI caller passes a stable value (per `project_inventory_party_rework`).
- **M-V4-2.** AddCurrency's cached response captures BuildStatePayload (14+ fields). Future field additions silently expand point-in-time scope.

### Error handling
(Per the agent: all 9 are pure Low / Observation. Highest item is HandleClaimQuestReward TickQuests-before-Begin which is H-V4-1 above.)

### Security
- **M-V4-1.** Downstream of C-V4-2: startup log "MAC + envelope-v OK" is false on secret mismatch.
- **M-V4-2.** `InvalidateByPlayerIdInternal` and `CleanupExpiredInternal` drop sessions without emitting `LogSessionEvent`.
- (4 more in per-dimension report)

### Code quality
- **M-V4-1.** M-Q3-2 only partially landed — 5 stdlib includes (`<algorithm>`, `<chrono>`, `<mutex>`, `<unordered_set>`, `<vector>`) still in `AccountServer.hpp:42-50` despite the commit claim.
- **M-V4-2.** `SaveAccountToRepository` (AccountServer.hpp:324-327) — dead method flagged in v2 L-Q2, v3 L-Q3-4, still present.
- **M-V4-3.** HandlePull/HandleMultiPull duplication (~250 + 333 LOC, ~80% overlap) carried forward unchanged.

### Test coverage
(See HIGH section — most test-coverage items are H/M severity.)

### Cross-cutting
- **M-V4-1.** Snapshot X-macro coverage convention-only — no static_assert, no `[c7-fields]` round-trip test.
- **M-V4-4.** Probe budget 5×2s=10s < cold-Docker Account boot ~25s; Release-fatal Stop() turns startup race into service crash.

### Networking
- **M-V4-1+.** 10 items spanning slowloris timeouts, partial-frame handling, recv-loop tight-spin on EAGAIN, send blocking, RecvChunk size assumptions, malformed-length handling. See per-dimension report.

---

## LOW / OBSERVATION

~70 unique items across dimensions after de-dup. Carry-forwards dominate. See per-dimension reports for full enumeration.

---

## Verified Closed (from v3, by independent multi-agent consensus)

| Item | Source | Status |
|---|---|---|
| C-V3-1 ProgressionReducer default 1→0 | v3 Critical | Closed; regression test added |
| C-V3-2 Begin() above GetPity/GetGuarantee | v3 Critical | Closed in code; regression test missing (H-V4-6) |
| H-V3-1 C7-A scope to SetParty/Quest handlers | v3 High | Closed for the three named; **missed HandleClaimQuestReward** (H-V4-1) |
| H-V3-2 + M-V3-1 advisory lock once-per-commit | v3 High | Closed |
| H-V3-3 created_at predicate | v3 High | Deferred with documented rationale; H-V3-5 closes the urgency |
| H-V3-4 outbox prune | v3 High | **Class written, never instantiated (C-V4-1)** |
| H-V3-5 idempotency sweeper + partman maintenance + 24mo retention | v3 High | **Schema applied to live DB but sweeper/maintenance non-functional (C-V4-1)** |
| H-V3-6 wallet CHECK constraints | v3 High | Closed; applied to live DB |
| H-V3-7 server destructor ordering | v3 High | Closed for all 3 TcpServerBase subclasses; 10s drain timeout still hard-coded (M-V4-1 concurrency) |
| H-V3-8 [[nodiscard]] on LockFor | v3 High | Closed |
| H-V3-9 try_emplace in InsertIfAbsent | v3 High | Closed |
| H-V3-10 dummy hash → function-local static | v3 High | Closed |
| H-V3-11 LruCache Peek/Touch + RateLimiter touch-iff-allow | v3 High | Closed; 4 regression cases added |
| H-V3-12 partition-local dedup documented | v3 High | Closed |
| H-V3-13 CommonTests/AuthTests/CombatTests premake | v3 High | Closed; content still tiny (H-V4-5) |
| H-V3-14(a) envelope version + golden file | v3 High | Closed; MAC over v verified |
| H-V3-14(b) startup canary Probe | v3 High | **Probe classification partial (C-V4-2)** |
| TC-V3-1 integration teardown fixture | v3 Test | Closed; 18 tests converted, 389 garbage rows removed |
| TC-V3-4 MAC round-trip + golden envelope | v3 Test | Closed |
| All v3 Medium items (~25) | v3 Medium | Closed; partial M-Q3-2 dead-include sweep (M-V4-1 code-quality) |
| All v3 Low items (~30) | v3 Low | Closed except explicitly deferred |

---

## Test Coverage Status

- v3 baseline: 70 cases / 442 assertions
- v4 (current HEAD): 95 cases / 599 assertions across 4 binaries
- **Net new:** 25 cases / 157 assertions in the v3 remediation arc
- v3→v4 fix-vs-test ratio: ~25 commits added tests of the 35-commit arc. Up from v2→v3's 1-in-22.

v3's prioritized top-5 status (revised):
1. **AccountCache stale-flag erase + reload** — still open (H-V4-5)
2. **InternalRpcAuth MAC round-trip** — closed (TC-V3-4)
3. **AccountRepository populated round-trip** — still open (H-V4-7)
4. **End-to-end HandleAddCurrency through DB** — still open (H-V4-7)
5. **Integration test isolation fixture** — closed (TC-V3-1)

v4 recommended top-5:
1. **Wire OutboxRelay into AccountServer** — closes C-V4-1; add integration test that asserts the relay's three sweep operations actually fire (probe `partman.last_run` advances after the first hour).
2. **Fix Probe response classification** — closes C-V4-2; add a test that constructs a mismatched-secret ServiceClient and asserts `Probe()` returns `AuthFailed`.
3. **AccountCache stale-flag round-trip test** — H-V4-5 (M-V2-3 extraction unblock).
4. **C-V3-2 regression guard** — H-V4-6 (first-pull-on-slot under Commit-failure).
5. **End-to-end HandleAddCurrency through DB** — v2 top-5 carry-forward, now trivially writable on IntegrationDbFixture.

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **C-V4-1** — instantiate `OutboxRelay` on AccountServer. ~3 LOC.
2. **C-V4-2** — fix `Probe()` to inspect `rpc.value["error"]`. ~10 LOC.
3. **H-V4-1** — move `Begin()` above `TickQuests::Apply` in HandleClaimQuestReward. ~3-line move.

**This week:**
4. **H-V4-2** — preserve RETURNING `created_at` in `AccountRepository::Create`.
5. **H-V4-3** — add `account_id` FK column + index to `outbox`.
6. **H-V4-4** — `#ifndef NDEBUG` longer ServiceClient timeout (or reduce Debug PBKDF2 iterations).
7. **H-V4-10** — connection caps on TcpServerBase.

**Test backlog (use IntegrationDbFixture):**
8. **H-V4-5** — AccountCache/Hydrator/InternalRpcHandlers direct tests.
9. **H-V4-6** — C-V3-2 first-pull-on-slot regression test.
10. **H-V4-7** — AccountRepository populated round-trip + HandleAddCurrency through-DB tests.
11. **H-V4-8** — Idempotency machinery tests.

**Documentation:**
12. **H-V4-9** — extend M-V2-7 point-in-time doc to enumerate all cached fields per handler.

**Pre-launch quality bar:**
13. Medium-tier sweeps (~40 items); Low backlog (~70 items).

---

## Agent reports

Per-dimension findings (raw):
1. `2026-06-03-v4-followup-event-sourcing.md`
2. `2026-06-03-v4-followup-persistence.md`
3. `2026-06-03-v4-followup-concurrency.md`
4. `2026-06-03-v4-followup-idempotency.md`
5. `2026-06-03-v4-followup-error-handling.md`
6. `2026-06-03-v4-followup-security.md`
7. `2026-06-03-v4-followup-code-quality.md`
8. `2026-06-03-v4-followup-test-coverage.md`
9. `2026-06-03-v4-followup-cross-cutting.md`
10. `2026-06-03-v4-followup-networking.md` (new dimension)

Synthesis written by the orchestrator session that ran the fleet.
