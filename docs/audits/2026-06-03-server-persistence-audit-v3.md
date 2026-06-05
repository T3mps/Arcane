# Aphelyon Server + Persistence Audit — v3 (Follow-up)
**Date:** 2026-06-03 (one day after v2 remediation arc)
**Scope:** Same as v1/v2 — Server/Account, Server/Auth, Server/Combat, Server/Common, Server/Account/schema.sql, Client/src/network_tcp.lua + Client/src/services, Server/Account/tests
**Method:** 9 parallel general-purpose audit agents, each given v1 + v2 + the relevant v2 per-dimension follow-up + the 17-commit diff since v2 baseline as required reading.

**Per-dimension findings** (raw agent reports — read for the file:line evidence):
1. Event sourcing + atomicity → `2026-06-03-v3-followup-event-sourcing.md`
2. Persistence + schema → `2026-06-03-v3-followup-persistence.md`
3. Concurrency + lifecycle → `2026-06-03-v3-followup-concurrency.md`
4. Idempotency + retry → `2026-06-03-v3-followup-idempotency.md`
5. Error handling + rollback → `2026-06-03-v3-followup-error-handling.md`
6. Security → `2026-06-03-v3-followup-security.md`
7. Code quality + complexity → `2026-06-03-v3-followup-code-quality.md`
8. Test coverage → `2026-06-03-v3-followup-test-coverage.md`
9. Cross-cutting / integration → `2026-06-03-v3-followup-cross-cutting.md`

---

## Executive Summary

The v2 remediation arc (22 fixes across 15 commits) is **verified closed in code by four-or-more independent agents**. End-to-end claim and pull flows walk clean. The three M-V2 structural refactors (`AccountServer` 683 → 345 lines via `AccountHydrator` + `AccountCache` + `InternalRpcHandlers`; `HandleClaimQuestReward` 310 → 120 via `ApplyClaimRewards` + `BuildClaimEvents`; `Account::Snapshot` X-macro) sharpen exactly the seams v2 flagged — code quality posture is materially better than at v2 baseline.

**Two new Criticals emerged**, both completeness gaps in v2 fixes that this synthesis pass surfaced:
- **C-V3-1** — H-V2-2 missed the reducer. `ProgressionReducer::ProgressionState::difficulty_tier = 1` still disagrees with the now-aligned schema (0) + struct (0) + Account (0). Source-of-truth divergence on the canonical event reducer; live Account reports tier=0 while reducer-rebuilt projection reports 1.
- **C-V3-2** — C7-A is bypassed on pull paths. `GetPity()` / `GetGuarantee()` at the top of HandlePull / HandleMultiPull are *lazy-mutating* accessors that insert into `m_pityBySlot` / `m_guaranteeBySlot` and flip `m_dirty.pity_slots` BEFORE `Begin()` fires. The Memento snapshot captures post-insert state — the exact defect C7-A shipped to close, masked by C1's stale-flag fallback (same shape as the original C7 bug).

**14 High items** — themes:
- **C7-A scope was too narrow.** Three non-event-sourced mutating handlers (`HandleSetParty`, `HandleReportQuestProgress`, `HandleCompleteQuest`) and the two pull paths (per C-V3-2) all mutate before `Begin()`. v2 explicitly scoped to event-sourced handlers; the v3 fleet flags both classes.
- **Unbounded growth surfaces.** `outbox` is never DELETEd, `idempotency_cache` has no sweeper, `events` partitions have no `pg_partman` cron and no retention. Three independent operational debts; all bite within months of launch.
- **Wallet integrity missing at the DB layer.** Five wallet columns lack `CHECK (>= 0)`. v2 M1; not actioned.
- **`m_dummyPasswordHash` ordering fragile.** H-V2-5's timing mitigation depends on the dummy hash being populated before any RPC fires; an init-order refactor would silently regress.
- **LruCache `Get` touches on rejected probes.** M-V2-10's spray defense is inverted in the attacker's favor — sustained spray evicts the legit user's slot.
- **Auth + Combat have zero direct test coverage** (structural — no test projects exist in premake).

**Audit-vs-fix scorecard (v2 → v3):**
- v2 Critical (3): all closed in code. C7-A has a scope-creep gap (C-V3-2 + H-V3-1) but the explicit-scope contract is met.
- v2 High (9): all closed.
- v2 Medium (13): all closed.
- v2 Low: most closed; some carried forward (the bigger refactors hit M-class status in v3).
- **NEW: 2 Critical, 14 High, ~25 Medium, ~30 Low / observations.**

**Systemic risk surfacing this pass:** test velocity is regressing. 22 fix commits, 1 shipped tests (`LruCacheTest.cpp` — 16 cases / 280 assertions). Net new coverage outside LruCache across the whole sweep ≈ 0 cases / +2 assertions. The fix-vs-test ratio is the largest single health gap, not any individual finding.

---

## CRITICAL (fix before next deploy)

### C-V3-1. `ProgressionReducer` default still says tier 1
**File:** `Server/Account/src/reducers/ProgressionReducer.hpp:10`
**Flagged by:** Persistence
**Status:** NEW — H-V2-2 completeness miss.

`ProgressionReducer::ProgressionState::difficulty_tier = 1`. The H-V2-2 fix in commit `18b6627` aligned three of four sites to 0 (schema default, `AccountData::difficultyTier`, `Account::m_difficultyTier`) but missed the reducer. A fresh state-fold reports tier=1 while the live Account row reports 0 — silent source-of-truth divergence on the system's canonical event-sourcing projection. The reducer test (`ProgressionReducerTest.cpp:25`) seeds `s.difficulty_tier = 1` so the wrong default passes its own test.

**Fix:** change `difficulty_tier = 1` → `difficulty_tier = 0` and update the test fixture.

### C-V3-2. C7-A is bypassed by lazy-mutating accessors on pull paths
**Files:** `Server/Account/src/GachaHandlers.hpp:117-118` (HandlePull), `:364-365` (HandleMultiPull); `Server/Account/src/Account.hpp:175-208` (GetPity / GetGuarantee)
**Flagged by:** Concurrency
**Status:** NEW — C7-A scope miss.

```cpp
PityTracker& pity            = account.GetPity(slotId, *pityConfigPtr);   // L117 — lazy-INSERTS into m_pityBySlot, flips m_dirty.pity_slots
GuaranteeTracker& guarantee  = account.GetGuarantee(slotId);              // L118 — same
// ...
auto txn = m_ctx.repository->Begin(account);   // L140 — snapshot captures POST-insert state
```

On a player's first pull for a given slot, the accessor inserts a default-constructed tracker AND marks the slot dirty BEFORE the snapshot fires. Rollback's `RestoreFrom` then restores to the post-insert point — exact replay of the C7 bug C7-A was supposed to close. C1's stale-flag eviction masks it (next handler reload re-derives pity from events); the Memento path is dead code for the first-pull-on-slot case.

**Fix:** either move `Begin()` above the tracker fetches (cheapest), make `GetPity` / `GetGuarantee` const-read with explicit `EnsurePity()` mutation called after `Begin()`, or extend the X-macro snapshot to capture pity state directly (it's already in `m_pityBySlot`, which IS in the snapshot — so the issue is the lazy-insert side-effect, not the data coverage).

---

## HIGH

### H-V3-1. C7-A scope missed three non-event-sourced mutating handlers
**Files:** `AccountHandlers.hpp` (HandleSetParty), `QuestHandlers.hpp:166-292` (HandleReportQuestProgress + HandleCompleteQuest)
**Flagged by:** Event sourcing
Same defect class as C-V3-2 but for non-event-sourced flows. These handlers mutate Account state (`SetParty`, quest objective progress, q->state) BEFORE `Begin()`. They don't emit events so Rollback's snapshot path is currently dead; failures recover via C1's stale-flag eviction. v2 deferred this as a deliberate scope decision; v3 surfaces it as a real gap.

### H-V3-2. Event-less commits bypass M-V2-5 advisory lock
**File:** `Server/Account/src/AccountTransaction.hpp` (EnsureOpen path)
**Flagged by:** Event sourcing
`pg_advisory_xact_lock` lives in `AppendInTx`. `EnsureOpen` only fires on the first buffered op (AppendEvent / RecordAudit / StoreIdempotency / EmitToOutbox). Any commit that buffers nothing skips the advisory lock entirely. Stripe lock saves us today; the moment horizontal scale lands (or stripe locks are bypassed) the empty-Commit class loses its DB serialization backstop.

### H-V3-3. `events` queries lack `created_at` predicates everywhere
**Files:** `Server/Account/src/AccountRepository.hpp` (LoadEventVersions, LoadByAccountId), `Server/Account/src/db/EventStore.hpp` (AppendInTx, AppendIdempotent, LoadStream)
**Flagged by:** Persistence
Postgres scans every partition for every event query. Cost scales linearly with partition count; with no retention configured (H-V3-5), every commit gets slower forever. Pre-launch a non-issue; weeks post-launch it dominates handler latency.

### H-V3-4. `OutboxRelay::PumpOnce` never DELETEs
**File:** `Server/Account/src/db/OutboxRelay.hpp:114`
**Flagged by:** Persistence
Successful dispatch UPDATEs `dispatched_at`; the row sits forever. Outbox grows monotonically. The integration tests hit this today (had to db-reset mid-session because 73 undispatched rows pushed test-seeded rows past LIMIT 64).

### H-V3-5. `idempotency_cache` no sweeper + `pg_partman` no retention/cron
**Files:** schema.sql idempotency_cache, schema.sql events + partman config
**Flagged by:** Persistence + Idempotency
Schema comment promises a sweeper that doesn't exist. partman creates 3 pre-built partitions; inserts will fail when the third is consumed (~3 months post-launch). idempotency_cache grows ~15MB/day at 1k DAU with the 24h TTL never enforced.

### H-V3-6. Wallet columns lack `CHECK (>= 0)`
**File:** `Server/Account/schema.sql:89-93`
**Flagged by:** Persistence
`credits`, `universal_credits`, `tickets`, `limited_tickets`, `scrap` — all signed BIGINT with no constraint. v2 flagged as M1; not actioned. A reducer bug or buggy migration could write a negative balance with no DB-level catch.

### H-V3-7. Detached `ServiceEndpoint` threads can outlive `AccountCache`
**Files:** `Server/Common/src/ServiceEndpoint.hpp` (thread-per-connection drain), `Server/Account/src/AccountServer.hpp` (member declaration order)
**Flagged by:** Concurrency
`m_internalEndpoint` lives in `TcpServerBase` (base class); `m_cache` lives in `AccountServer` (derived). Reverse declaration order destruction means `m_cache` is gone first, but ServiceEndpoint's detached connection threads have a 10s drain timeout — a mid-RPC handler still holds a stripe lock referencing the destroyed cache. UAF window during slow-DB or slow-PBKDF2 shutdowns.

### H-V3-8. `AccountCache` escape-hatch contracts are comment-only
**File:** `Server/Account/src/AccountCache.hpp` (LockFor / InsertIfAbsent / UpdateCachedPasswordHash)
**Flagged by:** Concurrency + Code quality
The three methods document "caller must hold LockFor for playerId" but have no runtime or compile-time enforcement. `LockFor` lacks `[[nodiscard]]` so a future misuse `m_cache.LockFor(playerId);` (no name binding) compiles and silently releases immediately.

### H-V3-9. `InsertIfAbsent` uses `operator[]`
**File:** `Server/Account/src/AccountCache.hpp` InsertIfAbsent
**Flagged by:** Concurrency
`m_accounts[id] = std::move(account)` default-constructs an empty `unique_ptr` then move-assigns. If a concurrent reader catches the empty-default-then-assign window it sees nullptr. Same defect class v2 flagged as M1 elsewhere. Should be `try_emplace`.

### H-V3-10. `m_dummyPasswordHash` startup ordering is fragile
**Files:** `Server/Account/src/AccountServer.hpp:73-107`, `InternalRpcHandlers.hpp` (ref-bind)
**Flagged by:** Security
`InternalRpcHandlers` binds `const std::string&` to `m_dummyPasswordHash` at construction; the actual hash is computed later in the constructor body before `Register()`. Works today (RPCs can't fire until `Register()` wires them up) but any future init-order refactor silently regresses H-V2-5 — `Crypto::VerifyPassword` short-circuits on an empty/malformed stored hash, returning in microseconds instead of 150-400ms. Timing oracle reopens.

### H-V3-11. LruCache `Get` touches on rate-limit-rejected probes
**File:** `Server/Common/src/RateLimiter.hpp` Allow path; `Server/Common/src/LruCache.hpp` Get
**Flagged by:** Security
`RateLimiter::Allow` calls `m_records.Get(key)` which touches LRU even when the entry is in cooldown and the call returns false. Sustained attacker spray keeps attacker entries at MRU and the legit user's once-touched slot drifts to LRU and gets evicted — the *opposite* of M-V2-10's stated goal. Fix: split into `Get` (touches) / `Peek` (no-touch), use Peek for rate-limit-reject paths; only touch on `Allow == true`.

### H-V3-12. Claim deterministic-key dedup is partition-local
**File:** `Server/Account/src/QuestHandlers.hpp` BuildClaimEvents
**Flagged by:** Idempotency
`events.UNIQUE (account_id, idempotency_key, created_at)` includes the partition key. The new H-V2-9 deterministic key `claim:<questId>:<accountId>` provides real dedup *within a partition*; two claims that straddle a monthly partition boundary could both INSERT. Stripe lock saves us today; real cross-time dedup lives in `idempotency_cache.PRIMARY KEY (account_id, scoped_key)` which is un-partitioned.

### H-V3-13. No Auth or Combat test projects (structural)
**Files:** `Server/premake5.lua` (no AuthTests / CombatTests / CommonTests projects)
**Flagged by:** Test coverage
`AccountTests` is the only test binary. SessionManager (GetValidSession from H-V2-8), AuthServer (HandleResumeSession from C7-B), RateLimiter (M-V2-10 LRU eviction), SessionCache (NoteAuthLost/Recovered from M-V2-9), ServiceEndpoint / ServiceClient (C7-C wire format), InternalRpcAuth (M-V2-11 cached secret) — none directly testable. Promake gap, not a "just write tests" gap.

### H-V3-14. C7-C wire-shape is a coordinated breaking change with no canary
**Files:** `Server/Common/src/ServiceClient.hpp` + `ServiceEndpoint.hpp`
**Flagged by:** Cross-cutting
No version negotiation in the envelope; both ends must redeploy in lockstep. No startup canary test that pings each peer with a known params_json. No round-trip test for the C7-C wire format in the suite. `ServiceClient::Call` returns empty `Json()` on TCP fail / parse fail / MAC fail / connect-refused — `SessionCache` can't distinguish integrity failures from unreachability and enters the H6 extension path even when an attacker is replaying a stale envelope.

---

## MEDIUM (selected; full lists in per-dimension reports)

### Event sourcing
- **M-V3-1.** Advisory lock re-acquired per event in multi-event commits (claim emits up to 7 events → 7 lock RTTs).
- **M-V3-2.** `EffectDispatcher` H10 latent collision unchanged from v2 (still dead-coded; will resurface if dispatcher is wired in).
- **M-V3-3.** Cursor reads in handlers (`dirty.cached_wallet_version + 1`) happen after Begin but read the live (non-snapshot) struct; correctness rests on the unenforced "no handler self-increments cursors" invariant.
- **M-V3-4.** H-V2-9 sibling keys include currency name but not delta direction — implicit "one-direction-per-currency-per-claim" invariant has no test.

### Persistence
- **M-V3-1.** `AccountRepository::Create` still omits most columns + no `RETURNING *` (same defect class as H-V2-2 — see C-V3-1).
- **M-V3-2.** `audit_log.metadata` + `events.metadata` rely on schema DEFAULT '{}'; no NOT NULL.
- **M-V3-3.** `last_login_claim_at` / `last_login_claim_day_idx` are dead schema columns (no C++ binding).
- **M-V3-4.** `events.idempotency_key` UNIQUE includes the partition key (carry-forward L-V2-5).
- **M-V3-5.** `seed.sql` is empty post-reset (lost dev fixtures during the session DB reset).

### Concurrency
- **M-V3-1.** `UpdateCachedPasswordHash` missing `!IsStale()` guard.
- **M-V3-2.** `LockedAccountRef` stripe held through full multi-pull handler body (200+ lines including RNG iteration and event serialization).
- **M-V3-3.** `NoteAuthRecovered` lock-free-input contract is comment-only.

### Idempotency
- **M-V3-1.** Client `idempotencyKey` parameter still dead in tree (H-V2-6 plumbing; inventory rework will need to wire it).
- **M-V3-2.** M-V2-7's point-in-time doc only mentions wallet balances; actually applies to streak/story/tier/pity counters/guarantee state too.
- **M-V3-3.** `ON CONFLICT DO UPDATE` on idempotency_cache corrupts on horizontal scale (safe today via stripe lock; future-scale concern).

### Error handling
- **M-V3-1.** `HandleBeginMinigame` (QuestHandlers.hpp:112) still uses ParseJsonSafe — mints HMAC challenge token; H-V2-7 sweep should have caught it.
- **M-V3-2.** `HandleGetInventory` + `HandleGetCharacterStats` use ParseJsonSafe (read-only, but invariant-drift surface).
- **M-V3-3.** `AuthServer::HandleRegister` + `HandleLogin` use ParseJsonSafe (mutating; explicit empty-checks downstream make it boundary-equivalent).

### Security
- **M-V3-1.** Per-RPC `LogSessionEvent("validated")` token-prefix leak (v2 M7 deferred).
- **M-V3-2.** DB password literal still in `AccountServer.hpp:58` default arg + 6 test fixtures.
- **M-V3-3.** `VerifyPassword` should burn one PBKDF2 even on parse-failure (defense in depth for H-V3-10).
- **M-V3-4.** Legacy `SessionManager::ValidateSession` is dead but still public.

### Code quality
- **M-Q3-1.** `AccountCache` escape-hatch trio contract unenforceable (dup of H-V3-8).
- **M-Q3-2.** `AccountServer.hpp` carries ~8 dead includes after M-V2-3 extraction.
- **M-Q3-3.** `HandlePull` / `HandleMultiPull` (~250 / 335 lines) duplicate ~80% of event-build / collection-mutation / idempotency logic. Next M-V2-4-style helper-extraction target.

### Test coverage (per-finding gaps)
- **TC-V3-1.** Integration teardown still absent (hit this session via db-reset).
- **TC-V3-2.** AccountCache / AccountHydrator / InternalRpcHandlers have zero direct tests.
- **TC-V3-3.** v2's prioritized top-5 = 0 of 5 done.
- **TC-V3-4.** C7-C MAC-over-wire-bytes has no round-trip test.

### Cross-cutting
- **M-V3-1.** `events.event_type` is unconstrained TEXT; `BuildClaimEvents` emits `story_xp_gained` but reducer doesn't dispatch on event_type. Silent vocabulary drift.
- **M-V3-2.** Snapshot X-macro coverage enforced by convention only.
- **M-V3-3.** Cross-service env-var matrix correct, but no runtime canary that asserts.

---

## LOW / OBSERVATION

- L-V3-1. `RestoreFrom` catch is still silent (v2 L-V2-1 carry-forward).
- L-V3-2. ServiceEndpoint outer-parse-failure surfaces `method=""` in log; better diagnostic value with the raw bytes hex-prefix.
- L-V3-3. LruCache `LruKey()` is unused outside tests.
- L-V3-4. `claimed_at_streak_day` naming nit (v2 L-V2-13 carry-forward).
- L-V3-5. WalletPropertyTest:122 uses `return;` instead of `RC_DISCARD()`.
- L-V3-6. `BuildClaimEvents::rewardKindFor` parallel to `DispatchCurrencyReward` — could be deduped.
- L-V3-7. `<algorithm>`, `<chrono>` dead includes in several files post-extraction.
- L-V3-8. Snapshot doc-comment placement no longer matches X-macro layout.
- L-V3-9. `BCryptGenRandom` failure falls back to `std::random_device` silently.
- L-V3-10. ResumeSession + Login sharing limiter is a documented design tradeoff worth UX-confirming.
- (~20 more in per-dimension reports.)

---

## Verified Closed (from v2, by independent 4+ agent consensus)

- **Critical:** C7-A (handler scope as specified — see C-V3-2 / H-V3-1 for scope-creep gaps), C7-B (ResumeSession through ValidateSession with clientIP), C7-C (MAC over on-wire bytes, params_json string).
- **High:** H-V2-1 (deferred per audit), H-V2-2 (schema/struct/Account — see C-V3-1 for reducer miss), H-V2-3, H-V2-4, H-V2-5 (see H-V3-10 for fragility), H-V2-6, H-V2-7 (see error-handling M-V3-1 for missed sites), H-V2-8, H-V2-9 (see H-V3-12 for partition scope).
- **Medium:** M-V2-1 through M-V2-13 all closed; M-V2-3 (AccountServer split) deserves special call-out as a clean architectural win.

---

## Test Coverage Status

- v2: 51 cases / 162 assertions
- v3: 70 cases / 442 assertions
- **Net new outside LruCache: ~0 cases / +2 assertions across 22 fix commits.**

v2's prioritized top-5 — all still missing. v3 top-5 (updated):
1. **AccountCache stale-flag erase + reload** — now testable as a unit after M-V2-3 extraction.
2. **InternalRpcAuth MAC round-trip** with int / float / nested / duplicate-key payloads (C7-C regression guard).
3. **AccountRepository populated round-trip** (unchanged from v2 — load an account with characters/weapons/gear/quests).
4. **End-to-end HandleAddCurrency through DB** (unchanged from v2 — exercise the AccountTransaction commit chain).
5. **Integration test isolation fixture** — promoted from v2's "ticking clock" Low to High after the session db-reset incident.

Meta-finding: 21 of 22 v2 fix commits chose not to add tests. Without a hook gate (CI / pre-commit) that requires test coverage for new audit findings, this pattern will persist into v3 fixes.

---

## Suggested triage order

**Today / immediate (before next deploy):**
1. **C-V3-1** — `ProgressionReducer::ProgressionState::difficulty_tier` 1 → 0 + test fixture. 2-line schema-alignment fix.
2. **C-V3-2** — Move `Begin()` above the GetPity/GetGuarantee fetches in HandlePull + HandleMultiPull. Same 5-line move as the C7-A pass.
3. **H-V3-11** — LruCache split into Get (touches) + Peek (no-touch); RateLimiter uses Peek on rejection. ~30 LOC; closes the M-V2-10 inversion.
4. **H-V3-10** — Make `m_dummyPasswordHash` a static-local in `HandleVerifyCredentials` (or add startup `assert(IterationsOfStoredHash == DEFAULT_ITERATIONS)`).

**This week:**
5. **H-V3-1** — Expand C7-A to SetParty / ReportQuestProgress / CompleteQuest. Same Begin()-to-top mechanical pattern.
6. **H-V3-2** — Move `pg_advisory_xact_lock` from `AppendInTx` to `EnsureOpen` so empty-Commits still serialize.
7. **H-V3-3** — Add `created_at >= partition-lower-bound` predicates to event SELECT/UNIQUE-check sites (or accept partition-scan cost pending H-V3-5 retention work).
8. **H-V3-6** — `CHECK (>= 0)` on the 5 wallet columns. 5-line schema change.
9. **H-V3-8 + H-V3-9** — `[[nodiscard]]` on `LockFor`; `try_emplace` in `InsertIfAbsent`.

**Before launch (operational):**
10. **H-V3-4 + H-V3-5** — Outbox DELETE-on-dispatched daemon. idempotency_cache TTL sweeper. partman retention cron + extra partition pre-warm.
11. **H-V3-7** — Member-init reorder so `m_cache` outlives `m_internalEndpoint`'s detached threads (or add explicit Stop+drain before m_cache destructs).
12. **H-V3-12** — Document the partition-local dedup scope, OR add a global event-key index alongside the partitioned UNIQUE.
13. **H-V3-13** — Add `AuthTests` + `CombatTests` + `CommonTests` premake projects.
14. **H-V3-14** — Startup MAC canary RPC; distinct `ServiceClient::Call` failure-mode returns (replace empty `Json()` with `Result<Json, FailureReason>`).

**Pre-launch quality bar:**
15. Medium-tier sweeps (~25 items); test-coverage backlog (v3 top-5).

---

## Agent reports

Per-dimension findings (raw):
1. `2026-06-03-v3-followup-event-sourcing.md`
2. `2026-06-03-v3-followup-persistence.md`
3. `2026-06-03-v3-followup-concurrency.md`
4. `2026-06-03-v3-followup-idempotency.md`
5. `2026-06-03-v3-followup-error-handling.md`
6. `2026-06-03-v3-followup-security.md`
7. `2026-06-03-v3-followup-code-quality.md`
8. `2026-06-03-v3-followup-test-coverage.md`
9. `2026-06-03-v3-followup-cross-cutting.md`

Synthesis written by the orchestrator session that ran the fleet.
