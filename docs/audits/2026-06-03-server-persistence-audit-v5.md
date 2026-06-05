# Aphelyon Server + Persistence Audit — v5 (Follow-up to v4)
**Date:** 2026-06-03 (same-day successor to v4, after the 20-commit v4 remediation arc)
**Scope:** Same as v4 — Server/Account, Server/Auth, Server/Combat, Server/Common, schema.sql, Client/src/network_tcp.lua + Client/src/services, Server/Account/tests.
**Method:** 10 parallel general-purpose audit agents (per-dimension), each given full v4 synthesis + v4 per-dimension followup + v3 per-dimension followup + skim of v2/v1, plus the 20 v4-remediation commit list. Each agent verified v4 closures against current code state and surfaced new findings.

**Per-dimension findings:**
1. Event sourcing + atomicity → `2026-06-03-v5-followup-event-sourcing.md`
2. Persistence + schema → `2026-06-03-v5-followup-persistence.md`
3. Concurrency + lifecycle → `2026-06-03-v5-followup-concurrency.md`
4. Idempotency + retry → `2026-06-03-v5-followup-idempotency.md`
5. Error handling + rollback → `2026-06-03-v5-followup-error-handling.md`
6. Security → `2026-06-03-v5-followup-security.md`
7. Code quality + complexity → `2026-06-03-v5-followup-code-quality.md`
8. Test coverage → `2026-06-03-v5-followup-test-coverage.md`
9. Cross-cutting / integration → `2026-06-03-v5-followup-cross-cutting.md`
10. Networking + service lifetime → `2026-06-03-v5-followup-networking.md`

---

## Executive Summary

The v4 remediation arc (20 commits closing 2 Critical, 9 High, 16 Medium, 4 Low) is **largely verified closed in code by independent agents**. The arc shipped clean — no v4 closure regressed, no v3 fix was undone. Tests grew from 70/442 to **117 cases / 779 assertions** (+47 cases, +337 assertions), with 5 of 20 commits being test-only and all of v4's prioritized top-5 now closed.

But v5 surfaces **three systemic patterns** that compound the v4 findings rather than emerging from new design:

- **C-V5-1 SnapshotWriter is the third instance** of the wiring-vs-implementation defect class. v4-C-V4-1 (OutboxRelay never instantiated) was the headline; we wired it. The same migration spec (`docs/superpowers/specs/2026-06-01-account-db-migration-design.md`) authored **SnapshotWriter** alongside OutboxRelay. SnapshotWriter is fully implemented at `Server/Account/src/Db/SnapshotWriter.hpp:32`, has integration tests at `Server/Account/tests/Integration/SnapshotWriterTest.cpp`, and **is never instantiated by AccountServer**. The live `snapshots` table has 0 rows against 1107 outbox writes. The entire snapshot/replay machinery — the whole point of the selective-event-sourcing design — is dead.

- **Four agents independently flagged the same defect** at `Server/Auth/src/AuthServer.hpp:266-297` (HandleLogin) and `:323-343` (HandleRegister). Both sites use `rpc.value.value("result", rpc.value)` which collapses envelope errors (`{"error":"Authentication failed"}`, `{"error":"unsupported_envelope_version"}`) into the result fallback. **This is the C-V4-2 / M-V4-9 pattern at sites the v4 sweep missed.** A Release deployment with mismatched `APHELYON_INTERNAL_SECRET` would silently degrade every login to "Invalid username or password" with only a DEBUG log line — the same ops blindness C-V4-2 was meant to close.

- **The v4 remediation arc itself shipped 5 new code paths with zero direct tests.** Test-coverage's H-V5-1 through H-V5-5 catalog this: TcpServerBase connection-cap refusal branches (H-V4-10), all 3 OutboxRelay sweep methods (C-V4-1), ServiceClient::Probe's 4-outcome classification (C-V4-2), ServiceEndpoint::InvokeForTest backdoor (H-V4-5), and outbox.account_id population (H-V4-3). The v4 arc's fix-vs-test ratio was respectable in aggregate (5/20 commits) but lopsided — all 5 test commits closed *prior* gaps, none covered the *new* code from the v4 arc itself.

Plus a **NEW lifecycle SIGABRT surface** (H-V5-1 networking): when `ProbeStartupPeers` calls `Stop()` from inside `OnStarted()` in Release-fatal mode, `Start()` then spawns `m_cleanupThread`, which immediately exits with `m_running` already false, returns, and the **joinable cleanup thread is never joined**. When `~TcpServerBase` runs, the thread's destructor calls `std::terminate`. Every Release-fatal probe failure SIGABRTs instead of shutting down cleanly. New in v5 because the v4 arc widened H-V4-10's lock surface and didn't audit the resulting interactions.

**Tally (raw, before de-dup across dimensions):** 1 Critical, 18 High, ~47 Medium, ~89 Low.

**De-duplicated unique findings:** 1 Critical, ~11 High, ~38 Medium, ~70 Low. The high overlap comes from the **AuthServer envelope-error pattern** being flagged by Security + Error-handling + Cross-cutting + Networking (4 agents), and **OutboxRelay::Register-never-called** being flagged by Concurrency + Event-sourcing (2 agents).

**Verdicts by dimension:**

| Dimension | Verdict | C/H/M/L |
|---|---|---|
| Event sourcing | clean | 0/0/2/4 |
| Persistence | concerning | 1/2/6/9 |
| Concurrency | concerning | 0/2/5/11 |
| Idempotency | clean | 0/0/4/6 |
| Error handling | clean | 0/0/1/9 |
| Security | clean (one notable scope-miss) | 0/2/1/6 |
| Code quality | clean | 0/0/3/5 |
| Test coverage | improving (but gaps from v4 arc) | 0/8/10/10 |
| Cross-cutting | improving | 0/1/5/8 |
| Networking | concerning | 0/3/10/21 |

**Audit-vs-fix scorecard (v4 → v5):**
- v4 Critical (C-V4-1 OutboxRelay wire, C-V4-2 Probe classify): **both closed in code**.
- v4 High (9 total): **all 9 closed in code**. H-V4-1 (Begin above TickQuests), H-V4-2 (RETURNING created_at — see below for a partial-fix nuance), H-V4-3 (outbox FK + index), H-V4-4 (Debug timeout split), H-V4-5 (M-V2-3 direct tests), H-V4-6 (PityRollback test), H-V4-7 (populated round-trip + AddCurrency e2e), H-V4-8 (idempotency machinery tests), H-V4-9 (M-V2-7 doc expansion), H-V4-10 (TcpServerBase caps). One partial: H-V4-2's `EXTRACT(EPOCH FROM created_at)::bigint` cast truncates microseconds (persistence M-V5-2).
- v4 Medium (16 actioned): all 16 closed.
- v4 Low (4 actioned): all 4 closed.

**Systemic risk surfacing this pass:**
- **Wiring-vs-implementation gap persists.** v4-C-V4-1 closed OutboxRelay's instantiation. v5-C-V5-1 surfaces SnapshotWriter as the same defect class. Plus event-sourcing L-V5-2 + concurrency H-V5-1 both flag that **OutboxRelay is now instantiated but no handler ever calls `Register()`**, so any future `txn.EmitToOutbox(...)` row would accumulate undispatched indefinitely. The migration spec's wiring checklist needs to be cross-referenced against AccountServer.hpp's member-init list literally before next audit.
- **Scope-miss class.** v4's C-V4-2 (Probe classify) and M-V4-9 (SessionCache classify) closed two of the four `m_authClient.Call` consumers. The other two (HandleLogin, HandleRegister) shipped with the same legacy pattern; four agents independently noticed.
- **Test debt from the v4 arc.** All v4 test commits closed v3 gaps; none covered the v4 code being shipped. v5's prioritized top-5 should include a "back-fill v4 code with tests" line item.
- **Comment-debt trajectory.** `AccountServer.hpp` audit-tag density climbed v3 5.5% → v5 7.0% (+9 tags from the v4 arc). v4 L-V4-10 warned about AccountCache; AccountCache held flat, but AccountServer took the lead. If v6 adds another 5+ tags here, extract `AccountServer_DESIGN.md`.

---

## CRITICAL (fix before next deploy)

### C-V5-1. SnapshotWriter is fully implemented but never instantiated
**File:** `Server/Account/src/Db/SnapshotWriter.hpp` (class), `Server/Account/src/AccountServer.hpp` (missing instantiation), `Server/Account/src/main.cpp` (missing instantiation)
**Source:** persistence C-V5-1
**Status:** NEW

The class works. `WriteSnapshot(account)` serializes the four event-sourced aggregates (wallet, pulls, quest_claims, progression) into the `snapshots` table for replay short-circuit; the per-account row is `(account_id, aggregate_kind)` keyed with `ON CONFLICT DO UPDATE`. `SnapshotWriterTest.cpp` exercises the round-trip. The schema reserves `snapshots.reducer_version` for the invalidation policy.

But:

```bash
$ grep -rn "SnapshotWriter\b" Server/Account/src/
Server/Account/src/Db/SnapshotWriter.hpp:32:class SnapshotWriter {
... (only the class file itself; no AccountServer member, no main.cpp instantiation)
```

Live DB state confirms: `snapshots` table is empty (0 rows) against 1107 outbox writes accumulated since the database was created.

Operational consequence:
- Cold-start replay walks the full `events` partition history for every aggregate every time. At 12 months post-launch with 1000 events/account, every Auth→Account VerifyCredentials path that triggers a cache reload chains through 4 partition scans + 4 reducer fold loops. The whole point of the snapshot/replay design was to bound this to the last-snapshot watermark.
- A reducer version bump (the invariant `snapshots.reducer_version` exists to track) has no mechanism to invalidate; there's no row to compare against.
- Same wiring-vs-implementation pattern as v4-C-V4-1 OutboxRelay.

**Fix:** instantiate as an `AccountServer` member after `m_outboxRelay`:
```cpp
db::SnapshotWriter m_snapshotWriter{m_pool};
```
Wire `m_snapshotWriter.MaybeWriteSnapshot(account)` at the post-commit bookkeeping point in `AccountTransaction::Commit` (after `dirty.cached_*_version` advances), gated on a stride policy (every 100 events per aggregate is the doc'd target — verify against the spec).

Verify with: after one successful HandlePull commit on a fresh account, `SELECT count(*) FROM snapshots WHERE account_id = $1` returns > 0.

**Cross-reference:** docs/superpowers/specs/2026-06-01-account-db-migration-design.md authored both SnapshotWriter and OutboxRelay. The spec's wiring checklist is the canonical source of truth; AccountServer.hpp's member-init list should be reviewed against it before the v6 audit.

---

## HIGH

### H-V5-1. AuthServer HandleLogin / HandleRegister miss the C-V4-2 / M-V4-9 envelope-error pattern
**Files:** `Server/Auth/src/AuthServer.hpp:266-297` (HandleRegister), `:323-343` (HandleLogin)
**Source:** security H-V5-1, error-handling M-V5-1, cross-cutting H-V5-1, networking H-V5-3 (**four independent flags**)
**Status:** NEW (scope-miss of C-V4-2 + M-V4-9)

Both handlers call `m_accountClient.Call("CreateAccount" | "VerifyCredentials", ...)` and read:
```cpp
auto result = rpc.value.value("result", rpc.value);
if (result.value("valid", false)) { /* success */ }
return CreateErrorResponse("Invalid username or password");
```

The `value("result", rpc.value)` fallback collapses envelope errors (`{"error":"Authentication failed"}`, `{"error":"unsupported_envelope_version"}`) into the result lookup. Both error shapes lack a `"valid":true`, so the handler returns the legitimate user-facing rejection — by coincidence the failure mode is safe, but the cause is misdiagnosed. HandleLogin logs only at DEBUG. A Release deployment with mismatched `APHELYON_INTERNAL_SECRET` between Auth and Account would silently degrade every login attempt to "Invalid username or password" with no WARN signal to ops.

Same shape as C-V4-2 (Probe) and M-V4-9 (SessionCache::Validate); the v4 sweep enumerated 2 of the 4 `m_authClient.Call` / `m_accountClient.Call` consumers. The other 2 stayed legacy.

**Fix:** mirror the M-V4-9 SessionCache pattern at both sites:
```cpp
if (rpc.value.contains("error")) {
    LOG_AUTH_WARN("Auth RPC envelope error: {}", rpc.value.value("error", std::string{}));
    return CreateErrorResponse("Internal error");
}
auto result = rpc.value.value("result", rpc.value);
```

Convergent flagging by 4 agents is strong evidence to prioritize.

### H-V5-2. OutboxRelay::Register never called — dispatch path is incomplete
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:53-56` (Register method), `Server/Account/src/AccountServer.hpp` (no Register calls anywhere)
**Source:** concurrency H-V5-1, event-sourcing L-V5-2 (**two independent flags**)
**Status:** NEW

C-V4-1 wired the relay's constructor. The three sweep methods (`PruneDispatchedOutbox`, `SweepExpiredIdempotency`, `RunPartmanMaintenance`) run correctly on the worker cadence. But `PumpOnce` skips every row whose `destination` has no registered handler (`OutboxRelay.hpp:182 — if (!handler) continue`). Without `Register("destination_name", handler_fn)` calls anywhere in the codebase, every queued outbox row would accumulate with `dispatched_at IS NULL` indefinitely.

Today this is **zero-impact** because no handler invokes `txn.EmitToOutbox(...)` — the outbox dispatch path is dead-code-by-omission. The C-V4-1 fix closed the sweep half of the relay; the dispatch half re-opens the moment the first handler emits.

The audit's classification: this is a **completion gap on C-V4-1**, same family as H-V5-1 above (C-V4-2's completion gap on AuthServer sites).

**Fix:** at minimum, document the contract in `AccountServer.hpp`'s RegisterHandlers method block:
```cpp
// The OutboxRelay's dispatch path requires per-destination Register calls.
// When you add a txn.EmitToOutbox call in a handler, register the matching
// destination here. Today no handler emits to outbox.
// m_outboxRelay.Register("destination_x", [this](const Json& p) { ... });
```

Or, more rigorously: refactor so the relay refuses to start (or logs WARN) when zero destinations are registered and `outbox` row count > 0.

### H-V5-3. TcpServerBase ~SIGABRT~ on Release-fatal probe failure (lifecycle defect)
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:80-91`, the three derived `ProbeStartupPeers` sites in AccountServer / AuthServer / CombatServer
**Source:** networking H-V5-1
**Status:** NEW

`Start()`'s sequence:
```cpp
m_running.store(true);
m_internalEndpoint.Start();
OnStarted();                                    // <-- ProbeStartupPeers in Release-fatal calls Stop() here
m_cleanupThread = std::thread(&TcpServerBase::CleanupThread, this);  // <-- spawns AFTER Stop()
while (m_running.load()) { /* accept loop */ }
```

If `OnStarted()` calls `Stop()` (Release-fatal probe failure), `m_running` is now false. `m_cleanupThread` is spawned anyway at the next line. The cleanup thread's loop checks `m_running.load()` immediately, exits, and the thread becomes joinable-but-not-joined. The accept-loop while-condition is false, so `Start()` returns true. Subsequent `Stop()` calls short-circuit on the atomic. When `~TcpServerBase` (or the derived `~AccountServer`) runs, `m_cleanupThread`'s std::thread destructor calls `std::terminate` per the C++ standard for joinable-but-not-joined threads.

**Operational consequence:** every Release deployment hitting a probe failure (Auth-Account secret mismatch, envelope skew, peer not up) crashes the process via SIGABRT instead of the clean Stop() exit the LOG_SERVER_CRITICAL log implies.

**Fix:** move the `m_cleanupThread = std::thread(...)` line to **before** `OnStarted()`. Or guard with `if (m_running.load())` before spawning. Or use `std::jthread` (C++20) which auto-joins on destruction.

This is a new H rather than a Critical because it only fires on probe failure, which is already a fatal-out path. But the SIGABRT prevents any clean-shutdown signal from reaching the orchestrator (no graceful drain, no log flush, no ops-friendly exit code).

### H-V5-4. ServiceEndpoint 10s detached-thread drain widens UAF window
**File:** `Server/Common/src/Net/ServiceEndpoint.hpp:143-148`
**Source:** networking H-V5-2, concurrency M-V5-2 (carry-forward)
**Status:** REPEAT (v4 H-V4-2 / M-V4-1 concurrency; explicitly deferred in v4 triage)

`Stop()` waits up to 10 seconds for in-flight connection threads to drain, then proceeds with detached threads still running. v3-H-V3-7 narrowed the UAF window for AccountServer.cpp's destructor by adding the explicit `Stop()` call; the 10s window itself is unchanged. H-V4-4's 30s Debug client timeout means handler runtime can exceed the drain budget — Debug shutdown now leaves up to 20s of in-flight handler work running against a destroyed ServiceEndpoint.

**Fix:** either (a) raise the drain timeout to match the longest handler runtime (30s Debug, 10s Release), or (b) restructure to wait unconditionally with a kill-switch after the longer interval.

### H-V5-5. partman premake schema-vs-live drift (live DB stuck at 3)
**File:** `Server/Account/schema.sql:342` claims `p_premake := 12`; live DB `partman.part_config.premake = 3`
**Source:** persistence H-V5-1
**Status:** NEW

v3-H-V3-5 bumped `p_premake` from 3 to 12 to give a 12-month pre-warmed partition runway. The schema.sql update landed, but `partman.create_parent(..., p_premake := 12)` only fires at initial bootstrap. The live DB was bootstrapped before the change; subsequent schema.sql edits don't re-bootstrap. The live `partman.part_config` row carries the old premake value.

Operational consequence: 3 months of pre-warmed partitions instead of 12. Inside 3 months of launch (and any time the OutboxRelay's `RunPartmanMaintenance` cadence — 1h — misses for >3 months for any reason), INSERTs into `events` would fail with "no partition matching key in partitioned table."

**Fix:**
```sql
UPDATE partman.part_config SET premake = 12 WHERE parent_table = 'public.events';
SELECT partman.run_maintenance_proc();   -- force a sweep to pre-create the new partitions
```

Verify schema.sql against `partman.part_config` directly. The H-V3-5 closure claim in v4 was schema-true but live-false.

### H-V5-6. AuthServer / TcpServerBase Stop() deadlock surface widened by H-V4-10
**File:** `Server/Common/src/Net/TcpServerBase.hpp:189-198`
**Source:** concurrency H-V5-2
**Status:** REPEAT (pre-existing) — widened by H-V4-10

`Stop()`'s LOCK2 block holds `m_clientsMutex` through `thread.join()` while live `HandleClient` threads need that same mutex for their disconnect bookkeeping at `:448-462`. The deadlock is bounded by the recv timeout (100ms) plus handler runtime, so it's been latent for the project's lifetime. H-V4-10 added work-under-m_clientsMutex in the accept loop (per-IP counter management), which widens the critical section, increases lock contention, and makes the deadlock easier to reach under realistic shutdown timing.

H-V4-4's 30s Debug recv timeout means any PBKDF2-verify handler in Debug (~18s) holds the disconnect-bookkeeping path under m_clientsMutex while Stop() tries to join.

**Fix:** restructure `Stop()` to drop m_clientsMutex before `thread.join()`. Two-phase: collect joinable threads under the lock, release, then join outside. This is structurally similar to the AccountCache::SaveAllAndClear two-phase pattern (audit M1 + M-V2-3).

### H-V5-7. accounts.public_uid UNIQUE is dead surface
**File:** `Server/Account/schema.sql:71` (public_uid UNIQUE constraint)
**Source:** persistence H-V5-2
**Status:** NEW

`accounts.public_uid` has a UNIQUE constraint creating an index. Live DB stats:
```
idx_scan = 0 (across 2495 PK scans of accounts.account_id)
```

No code path queries by public_uid. The column exists for display (the 9-digit Trailblaze-Level analogue) but reads are always by `account_id` or `username`. The UNIQUE constraint is real — `public_uid_seq` enforces it on insert — but the **index that enforces it has never served a SELECT**.

This is observational pre-launch (zero query load), but the index pays the INSERT-time cost on every account create with no read benefit.

**Fix (deferred):** drop the UNIQUE constraint if the spec confirms public_uid is display-only. If a future "view player by public_uid" flow lands, re-add. Tracking as H because the spec is the source of truth — if public_uid IS supposed to be queryable, then a missing read path is the bug; if it's display-only, the UNIQUE index is the bug.

### H-V5-8. 5 v4-introduced code paths still untested
**Files:**
- `Server/Common/src/Net/TcpServerBase.hpp:119-139` — H-V4-10 connection-cap refusal branches
- `Server/Account/src/Db/OutboxRelay.hpp:115-152` — three sweep methods (PruneDispatchedOutbox, SweepExpiredIdempotency, RunPartmanMaintenance)
- `Server/Common/src/Net/ServiceClient.hpp:289-325` — Probe's 4-outcome classification
- `Server/Common/src/Net/ServiceEndpoint.hpp:64-83` — InvokeForTest backdoor
- `Server/Account/src/Cache/AccountTransaction.hpp:183-194` — outbox.account_id population
**Source:** test-coverage H-V5-1 through H-V5-5
**Status:** NEW

All five code paths shipped in the v4 arc. None have direct test coverage. The Probe classification gap is the most impactful — a future change that breaks "unsupported_envelope_version" detection (e.g., refactor that renames the server-side error string) would land silently.

**Fix:** test backfill. The fixture infrastructure is in place; each test is ~30-60 LOC on the IntegrationDbFixture pattern (for the DB-touching items) or pure unit (for Probe classification).

---

## MEDIUM (selected; full lists in per-dimension reports)

### Persistence
- **M-V5-1.** 60 pre-fix outbox rows with `account_id = NULL` (pre-H-V4-3 era). Un-cleanable by CASCADE on a future account hard-delete because payload has no account info either.
- **M-V5-2.** H-V4-2's `EXTRACT(EPOCH FROM created_at)::bigint` cast truncates microseconds. C++ side `data.createdAt` doesn't equal the row's actual TIMESTAMPTZ. Partial fix.
- **M-V5-3.** AccountRepository::Save's brute-force dirty re-mark (M-V4-5 carry-forward, unactioned).

### Concurrency
- **M-V5-1.** OutboxRelay's worker thread starts in member-init list (line 79 of AccountServer.hpp), before ctor body runs Initialize*/RegisterHandlers. Race surface for future Register calls in ctor body.
- **M-V5-3.** H-V4-10 per-IP counter decrement only fires in CleanupThread (lags disconnect by up to 5s). Slow-tier leak.

### Idempotency
- **M-V5-4.** Cache-hit short-circuit emits no audit_log row (M-V4-7 carry-forward, unactioned). Forensically blind to retries.
- **M-V5-2.** AddCurrency cached payload (~24 fields, full BuildStatePayload) — H-V4-9 documented the surface; behavioral change (minimal ack + force GetState refresh) deferred.

### Security
- **M-V5-1.** TcpServerBase `m_clientsByIp` keyed by raw peer IP. NAT-collapsed populations (school/dorm/CDN) all share the 16-conn cap. Pre-launch operational hazard.

### Event sourcing
- **M-V5-2.** ProgressionReducer doesn't validate `evt.from_level` against current `story_level`. Wallet/Pulls/QuestClaims all throw `*InvariantViolation` on stale baselines; Progression silently overwrites. Replay-determinism gap, surfaced by my own golden-fixture commit (6357419) for story_xp_gained.

### Cross-cutting
- **M-V5-2.** `events.schema_version` is the same silent vocabulary-drift surface M-V4-3 flagged for `event_type`. Default-1 emit, no reducer dispatches on it, load-bearing-with-zero-scaffold on first migration.
- **M-V5-4.** `ProtocolLoader::Id()` returns `kInvalidMsgId=0` on unknown name with no startup validation; cached-ID sites never check the result. Typo in protocol.json silently sends as id 0.

### Code quality
- **M-V5-1.** `portOverride` is a dead variable in `Server/Account/src/main.cpp:99,111` (Auth's main.cpp consumes it at L132; Account drops it on the floor).
- **M-V5-2.** Tombstone-of-a-tombstone in AccountServer.hpp:326-346 — M-V4-2's deletion comment sits immediately above the older L3 ConvertAccountToData tombstone, under a "Persistence" section header with zero live code between. v4-L-V4-1 misleading-adjacency got worse.
- **M-V5-3.** OutboxRelay.hpp:208 uses `std::condition_variable` with no `<condition_variable>` include — transitive via `<thread>`/`<mutex>`. V4-cycle-introduced IWYU regression.

### Networking
- **M-V5-1.** Server-side `SetSocketTimeout` at `ServiceEndpoint.hpp:111` is still hardcoded 5000 — H-V4-4 only fixed the client side. Half-fixes the "magic-number drift surface" the H-V4-4 commit claimed to eliminate.
- **M-V5-3.** Probe's classification covers 2 of 4 error strings (Authentication failed, unsupported_envelope_version). "Unknown method: ..." and "Internal error: ..." both fall through to UnknownError — same observability shape but should at minimum log distinctively.
- **M-V5-4.** ServiceEndpoint::InvokeForTest has no compile-time gate; only a doc comment keeps production callers away.

### Test coverage
- **M-V5-1+.** 10 items spanning property tests for QuestClaimsReducer / ProgressionReducer, cross-aggregate replay-determinism test, integration test for the H-V4-10 caps refusal path, etc. See per-dimension report.

---

## LOW / OBSERVATION

~70 unique items across dimensions after de-dup. Carry-forwards dominate (v3-deferred + v2-deferred carry forward unchanged). See per-dimension reports for full enumeration.

Notable convergent observations:
- **AccountServer.hpp audit-tag density** climbed to 7.0% (28 tags / 398 lines, +9 from v4 arc). v6 should extract `AccountServer_DESIGN.md` if 5+ more tags land.
- **Carry-forward dominance.** Roughly 40 of ~70 unique Lows are restatements of v2 / v3 / v4 items. The noise floor doesn't drop because the audit response pattern leaves them. v5 actioned 4 Lows (L-V4-2, L-V4-7, L-V4-11, L-V4-1) in the v4 arc; v5 itself surfaced ~70 new + carry-forward unique Lows. The asymptote is real.

---

## Verified Closed (from v4, by independent multi-agent consensus)

| Item | Source | Status |
|---|---|---|
| C-V4-1 OutboxRelay instantiation | v4 Critical | Closed; sweep methods active. **Dispatch path incomplete (H-V5-2)** because no Register() calls exist; same defect class re-opens at first handler emit. |
| C-V4-2 Probe response-body classification | v4 Critical | Closed for the 2 named error strings. **Same pattern missed at HandleLogin / HandleRegister (H-V5-1).** "Unknown method"/"Internal error" fall through (M-V5-3 networking). |
| H-V4-1 Begin above TickQuests::Apply in HandleClaimQuestReward | v4 High | Closed verbatim. PityRollback regression test (commit 5fcb70d) materially exercises the dtor-Rollback path. |
| H-V4-2 RETURNING created_at | v4 High | **Partial** — epoch cast truncates microseconds (M-V5-2 persistence). |
| H-V4-3 outbox.account_id FK + index | v4 High | Closed. AccountTransaction populates account_id correctly. 60 pre-fix rows orphan in M-V5-1 persistence. |
| H-V4-4 Debug-only ServiceClient timeout | v4 High | Closed for client (Connect + Call). **Server-side `ServiceEndpoint::Start` at L111 still hardcoded 5000** (M-V5-1 networking). |
| H-V4-5 Direct tests for M-V2-3 extractions | v4 High | Closed; 10 cases / ~67 assertions. Value-based stale-flag assertion is sound. |
| H-V4-6 C-V3-2 first-pull rollback regression test | v4 High | Closed; PityRollbackRegressionTest's structure correctly triggers the regression signal. |
| H-V4-7 Populated round-trip + AddCurrency e2e | v4 High | Closed; 3 cases / ~44 assertions. |
| H-V4-8 Idempotency machinery tests | v4 High | Closed; 5 cases / ~45 assertions. |
| H-V4-9 M-V2-7 point-in-time doc | v4 High | Closed; per-handler enumeration landed. |
| H-V4-10 TcpServerBase connection caps | v4 High | Closed mechanically. **Widens H-V5-6 Stop() deadlock surface** and introduces M-V5-3 per-IP counter leak. **Raw-IP keying (M-V5-1 security) is a NAT operational hazard.** |
| M-V4-1 persistence (drop reducer_version) | v4 Medium | Closed. |
| M-V4-2 persistence (outbox_dispatched_idx) | v4 Medium | Closed. |
| M-V4-3 persistence (last_streak_day CHECK) | v4 Medium | Closed. |
| M-V4-2 security (LogSessionEvent reason field) | v4 Medium | Closed. JSON escape safe (reasons are compile-time literals). |
| M-V4-6 security (Crypto::ParseStoredHash odd-length guard) | v4 Medium | Closed; dummy-hash burn still triggers. |
| M-V4-1 idempotency (cadence doc 65s) | v4 Medium | Closed in OutboxRelay.hpp; **schema.sql:434-436 still claims "every minute"** (L-V5-1 idempotency, one-line doc drift). |
| M-V4-5 idempotency (ExtractClientKey helper) | v4 Medium | Closed; **all 8 handler sites adopted correctly**, sibling-key disjointness pinned by IdempotencyMachineryTest. |
| M-V4-5 networking (stoull-strict frame parse) | v4 Medium | Closed; single-point fix covers all symmetric paths. |
| M-V4-6 networking (parser-clear on Send failure) | v4 Medium | Closed; defense-in-depth. |
| M-V4-9 networking (SessionCache envelope-error classify) | v4 Medium | Closed for SessionCache. **Same pattern missed at AuthServer (H-V5-1).** |
| M-V4-4 cross-cutting (probe budget widen) | v4 Medium | Closed; all 3 sites use retries=15 delayMs=2000. |
| M-V4-2 cross-cutting (golden fixtures credits_added + story_xp_gained) | v4 Medium | Closed; all 6 emitted event_types now covered. story_xp_gained fixture surfaced M-V5-2 event-sourcing (Progression validation gap). |
| M-V4-1/2/4 code-quality (dead includes, dead method, vector include) | v4 Medium | All closed. |
| L-V4-1 (orphan Idle Account Eviction divider) | v4 Low | Closed in same e30ea9e cleanup. |
| L-V4-2 (claimed_at_streak_day doc) | v4 Low | Closed. |
| L-V4-7 (universal_credits in Create INSERT) | v4 Low | Closed. |
| L-V4-11 (TODO false positive) | v4 Low | Closed. |

---

## Test Coverage Status

- v4 baseline: 70 cases / 442 assertions
- v5 (current HEAD): **117 cases / 779 assertions** across 4 binaries
- **Net new:** 47 cases / 337 assertions in the v4 remediation arc
- v4→v5 fix-vs-test ratio: 5 of 20 commits (25%) test-only — closed **all 5 v4 prioritized top-5**.

v4's prioritized top-5 status (revised):
1. **Wire OutboxRelay into AccountServer** — closed in C-V4-1. **C-V5-1 surfaces SnapshotWriter as the same defect.**
2. **Fix Probe response classification** — closed in C-V4-2. **Same pattern missed at HandleLogin / HandleRegister (H-V5-1).**
3. **AccountCache stale-flag round-trip test** — closed in H-V4-5.
4. **C-V3-2 regression guard** — closed in H-V4-6.
5. **End-to-end HandleAddCurrency through DB** — closed in H-V4-7.

v5 recommended top-5:
1. **Wire SnapshotWriter into AccountServer** — closes C-V5-1; mirrors the C-V4-1 fix pattern. Cross-reference the migration spec to find any other unwired class.
2. **Apply C-V4-2 / M-V4-9 envelope-error classify to AuthServer HandleLogin + HandleRegister** — closes H-V5-1. 4 agents flagged independently.
3. **Fix `m_cleanupThread` spawn-after-Stop SIGABRT** — closes H-V5-3 networking. Pure lifecycle ordering fix.
4. **Restructure `TcpServerBase::Stop()` to drop m_clientsMutex before thread.join()** — closes H-V5-6 concurrency deadlock surface widened by H-V4-10.
5. **Test back-fill for the 5 v4-introduced untested code paths** — closes H-V5-8 (5 sub-items in test-coverage report).

Plus persistence-cluster: **H-V5-5 partman premake live-DB ALTER + run_maintenance_proc()** (urgent at 3-month watermark from DB bootstrap date), **C-V5-1 SnapshotWriter wire**.

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **C-V5-1** — instantiate `SnapshotWriter` on AccountServer + wire post-commit hook. Cross-reference migration spec for other unwired classes.
2. **H-V5-1** — apply C-V4-2 / M-V4-9 envelope-error classify to AuthServer HandleLogin + HandleRegister. ~15 LOC.
3. **H-V5-3** — move `m_cleanupThread` spawn before `OnStarted()` in `TcpServerBase::Start()`. ~2-line move.
4. **H-V5-5** — `UPDATE partman.part_config SET premake = 12 WHERE parent_table = 'public.events'; CALL partman.run_maintenance_proc();`. Verify pre-built partitions extend 12 months.

**This week:**
5. **H-V5-2** — document OutboxRelay::Register contract OR add startup self-check (refuse to start when zero destinations registered and outbox row count > 0).
6. **H-V5-6** — restructure `TcpServerBase::Stop()` LOCK2 to two-phase collect-then-join.
7. **H-V5-4** — raise ServiceEndpoint drain timeout to match longest handler runtime (Debug 30s, Release 10s).
8. **H-V5-7** — decide policy on `accounts.public_uid` UNIQUE constraint: drop or add read path.

**Test backlog (H-V5-8 cluster):**
9. Refusal-path test for `TcpServerBase` connection caps.
10. Direct tests for `OutboxRelay::{PruneDispatchedOutbox,SweepExpiredIdempotency,RunPartmanMaintenance}`.
11. Probe-classification unit test driving 4 outcomes through mock responses.
12. Negative test for `ServiceEndpoint::InvokeForTest` exposure (compile-time gate via #ifdef would obviate).
13. Test for `outbox.account_id` population on AccountTransaction commit.

**Documentation:**
14. Update `schema.sql:434-436` "every minute" → "every ~65s" (L-V5-1 idempotency).
15. Cross-reference migration spec wiring checklist against AccountServer member-init.

**Pre-launch quality bar:**
16. Medium-tier sweeps (~38 items); Low backlog (~70 items).

---

## Agent reports

Per-dimension findings (raw):
1. `2026-06-03-v5-followup-event-sourcing.md`
2. `2026-06-03-v5-followup-persistence.md`
3. `2026-06-03-v5-followup-concurrency.md`
4. `2026-06-03-v5-followup-idempotency.md`
5. `2026-06-03-v5-followup-error-handling.md`
6. `2026-06-03-v5-followup-security.md`
7. `2026-06-03-v5-followup-code-quality.md`
8. `2026-06-03-v5-followup-test-coverage.md`
9. `2026-06-03-v5-followup-cross-cutting.md`
10. `2026-06-03-v5-followup-networking.md`

Synthesis written by the orchestrator session that ran the fleet (20 commits of v4 remediation, then 10 parallel agents for v5).

---

## Methodology Note

This sweep took **10 parallel general-purpose agents** in a single fan-out (all 10 spawned in two batches: 9 in the first message, 1 corrective spawn for the missing Networking dimension after the orchestrator noticed the gap). Each agent:

1. Read the v4 synthesis + v4 per-dimension followup + v3 per-dimension followup + skim of v2/v1.
2. Inspected the 20 v4-remediation commits via `git show` to verify intent-vs-landed.
3. Audited current code state for both **v4-closure verification** AND **new findings**.
4. Wrote findings to disk (`docs/superpowers/audits/2026-06-03-v5-followup-<dim>.md`).
5. Returned a ≤500-word compact summary to the orchestrator.

The orchestrator (this session) maintained ≤6KB of agent-result context (10 × ~500-word summaries) plus this synthesis, instead of inlining the 10 full reports (~80KB combined). Total wall-clock for the 10-agent fan-out: ~17 minutes from spawn to last completion.

The convergent-flagging pattern (H-V5-1 flagged by 4 agents independently, H-V5-2 by 2 agents) provided high-confidence prioritization with zero coordination overhead — each agent ran with no awareness of the others' findings.
