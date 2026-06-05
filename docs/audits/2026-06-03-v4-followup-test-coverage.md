# v4 Follow-up Audit: Test Coverage

**Date:** 2026-06-03 (same-day follow-up to the v3 remediation arc)
**Scope:** `Server/Account/tests/`, `Server/Common/tests/`, `Server/Auth/tests/`, `Server/Combat/tests/`, premake test-project wiring
**Auditor:** v4 follow-up (read-only)
**Previous audits:**
- `2026-06-02-server-persistence-audit.md` (v1)
- `2026-06-02-server-persistence-audit-v2.md` (v2)
- `2026-06-02-followup-test-coverage.md` (v2 dimension)
- `2026-06-03-server-persistence-audit-v3.md` (v3 synthesis)
- `2026-06-03-v3-followup-test-coverage.md` (v3 dimension)

---

## Verdict

**Posture: concerning — but materially better than at the v3 mark.** The v3 remediation arc finally lit up structural gaps the v1/v2/v3 audits could only describe in the abstract: `CommonTests`, `AuthTests`, and `CombatTests` premake projects now exist (`Server/premake5.lua:439-441`), `IntegrationDbFixture` solves the integration-teardown DB-bloat that bit the user mid-session (TC-V3-1 closed), the C7-C MAC-over-wire-bytes contract picked up an 11-case round-trip suite plus a golden-file envelope-shape pin (TC-V3-4 closed), and the v3 Critical `ProgressionReducer` default-tier misalignment ships with a fixture-default-asserting test (C-V3-1 closed). That said, the **substance of the test surface outside Account is still tiny** — `AuthTests` is 2 trivial cases on `SessionManager` constructor defaults, `CombatTests` is one `REQUIRE(true)` stub, and `CommonTests` is 17 cases but 11 of those are concentrated in one file. v2's prioritized top-5 still scores **2 of 5 done** (#2 InternalRpcAuth round-trip and the new v3 #5 integration fixture are closed; #1 `AccountCache` stale-erase + reload, #3 `AccountRepository` populated round-trip, and #5 end-to-end `HandleAddCurrency` remain open). TC-V3-2 (`AccountCache`/`AccountHydrator`/`InternalRpcHandlers` unit tests) is the single largest carry-forward — three classes extracted explicitly to be unit-testable, still with zero direct tests one day after extraction. The audit-vs-fix ratio improved drastically (LruCache 16 + InternalRpcAuth 11 + EnvelopeShape 2 + ProgressionReducer-default-assert + IntegrationDbFixture across 18 integration converts), but the velocity is still uneven: the v3 fixes that landed code-without-test (H-V3-7 destructor reorder, H-V3-8/9 AccountCache hardening, C-V3-2 `Begin()` move on the pull paths) match the v2 pattern. No new Criticals.

---

## CRITICAL

*(None. Test-coverage Criticals are rare; the closest v4 cousin is TC-V3-5 below, but per v3 H-V3-13 the premake gap that made it Critical is now closed — the substance is now a High.)*

---

## HIGH

### H-V4-1. C-V3-2 (lazy-mutating accessors on pull paths) has no regression test
**Files:** `Server/Account/src/Handlers/GachaHandlers.hpp:124-127` (HandlePull), `:369-372` (HandleMultiPull); `Server/Account/tests/Integration/` (no test references `GetPity` / `HandlePull` / `HandleMultiPull`).

The C-V3-2 fix moved `m_ctx.repository->Begin(account)` above `account.GetPity(...)` / `account.GetGuarantee(...)` so the Memento snapshot captures the pre-lazy-insert state. The fix is verified in code (clear comments at L117-124 and L364-369). But the exact bug shape — first-pull-on-slot lazy-INSERTs into `m_pityBySlot` and flips `m_dirty.pity_slots` BEFORE `Begin()` — has no test. The next handler refactor (e.g., M-Q3-3's HandlePull/HandleMultiPull helper extraction noted in v3) can silently regress the call ordering with no failing test. Identical structural risk to the original C7 bug, which shipped with a "test" that only passed because of post-mutation Memento timing.

**Recommended:** unit-style test that constructs an `Account` with no `m_pityBySlot` entries, opens an `AccountTransaction`, calls `GetPity(slotId, cfg)` after `Begin()`, forces `Commit()` to throw (bad event payload), and asserts `m_pityBySlot` is empty on rollback. Equally important: a NEGATIVE test that swaps the order and asserts the rollback fails to restore (locks in the contract).

### H-V4-2. `AccountCache` / `AccountHydrator` / `InternalRpcHandlers` still have zero direct tests
**Files:** `Server/Account/src/Cache/AccountCache.hpp`, `Server/Account/src/Cache/AccountHydrator.hpp`, `Server/Account/src/Handlers/InternalRpcHandlers.hpp`; `Server/Account/tests/` (grep for any of the three class names: zero hits).

Carries forward TC-V3-2 unchanged. The M-V2-3 three-step extraction explicitly created these classes to be unit-testable; one day after extraction none has a dedicated test. The C1 stale-flag erase+reload contract — v2's #1 prioritized test — now lives in `AccountCache::GetLockedAccount` and remains untested by any code path other than the production handlers themselves. `AccountCache::InsertIfAbsent`'s H-V3-9 `try_emplace` fix and `LockFor`'s H-V3-8 `[[nodiscard]]` are both code-only fixes; the contracts they enforce (no concurrent default-construct window, no silently-dropped lock) have no unit-test that would notice their reversal.

**Recommended:** `tests/Unit/AccountCacheTest.cpp` covering — IsStale triggers erase+reload, `LockFor` returns a locked unique_lock (verify via `try_lock` from another thread), `InsertIfAbsent` second-call returns the first-inserted Account, `UpdateCachedPasswordHash` happy path, `CleanupIdleAccounts` removes idle / preserves recent. `AccountHydrator` should be tested as a pure `AccountData → Account` transformer (no DB needed; mock the repo or pass literals).

### H-V4-3. `AccountRepository` populated round-trip still missing
**Files:** `Server/Account/src/Cache/AccountRepository.hpp` (9+ `Load*` helpers); `Server/Account/tests/Integration/AccountRepositoryTest.cpp` (5 cases — Create / duplicate / scalar-save / LoadByUsername / Delete).

Carry-forward from v2 #3 and v3 #3. Still no test that inserts an account with characters / weapons / gear / gear_substats / char_traces / loadouts / party_slots / quest_states / quest_objectives / world_flags / material_inventory / pity_state, calls `LoadByUsername` (or a populated `LoadByAccountId`), and round-trips field-by-field. The `IntegrationDbFixture` (TC-V3-1 closed) is the prerequisite that made this writable — now it's just a "no excuse" item. M-V3-1 in v3's persistence dimension explicitly flagged `AccountRepository::Create` as omitting most columns; a populated round-trip catches that and the dead `last_login_claim_at` / `last_login_claim_day_idx` columns (v3 persistence M-V3-3) in one test.

### H-V4-4. End-to-end `HandleAddCurrency` through DB still missing
**Files:** `Server/Account/src/Handlers/*.hpp` (any `HandleAddCurrency`); `Server/Account/tests/` (no reference).

Carry-forward from v2 #5. The v1 audit's C4 dismissal explicitly framed `HandleAddCurrency` as "the canonical wallet-grant path used by purchases, quests, achievements" (echoed in `MEMORY.md`'s `project_addcurrency_not_debug_only.md`). The canonical wallet-grant path has zero direct end-to-end coverage. Combined with H-V3-6 (wallet columns lack `CHECK (>= 0)`), a buggy grant handler today could write a negative balance with no DB-level catch AND no test-level catch.

### H-V4-5. `SessionCache` `NoteAuthLost` / `NoteAuthRecovered` / `DropExtendedSessions` untested
**Files:** `Server/Common/src/Net/SessionCache.hpp`; `Server/Common/tests/` (no reference).

`CommonTests` now exists (H-V3-13) but covers only RateLimiter smoke and InternalRpcAuth. The H6 / M-V2-9 force-revalidate-on-recovery state machine — extension-on-auth-loss, drop-extended-on-recovery — is precisely the kind of state machine that benefits most from explicit unit tests. v3 concurrency M-V3-3 noted the `NoteAuthRecovered` lock-free-input contract is comment-only; a test would lock in the contract.

### H-V4-6. `SessionManager::GetValidSession` (H-V2-8) untested
**Files:** `Server/Auth/src/SessionManager.hpp` (GetValidSession single-acquisition helper); `Server/Auth/tests/SessionManagerSmokeTest.cpp` (2 trivial cases — defaults + SetConfig round-trip).

`AuthTests` exists but covers only `SessionConfig` round-tripping. The H-V2-8 fix (single-acquisition session validity check) and the C7-B IP-binding routing it enables both ride on `GetValidSession`'s correctness and have zero test coverage. The `SessionManagerSmokeTest.cpp` header comment explicitly acknowledges this: *"the H-V2-8 GetValidSession single-acquisition / C7-B IP-binding / M9 idle-bump-skip invariants are flagged in the v3 audit's test-coverage section and will be added here incrementally."* — still to be done.

### H-V4-7. `CombatTests` is a one-line `REQUIRE(true)` stub
**Files:** `Server/Combat/tests/SmokeTest.cpp` (`REQUIRE(true)`).

Premake project exists; substance is zero. Combat has no event-sourcing surface and no DB pressure, but `CombatServer` does have lobby join / IP rate-limit / session-validate paths that share the C7-B + M-V2-10 + M-V2-9 contracts. The smoke test header explicitly says "Real Combat tests will follow once the gameplay loop is implemented" — fine as a deferred concession, but flagged here so it doesn't get lost.

---

## MEDIUM

### M-V4-1. C7-A snapshot coverage is still 6 of ~24 fields
**Files:** `Server/Account/tests/Integration/AccountTransactionTest.cpp` (the `[c7]` test at L182-).

v3 TC-V3-3 noted the M-V2-2 X-macro refactor enables a single ~30-line X-macro-driven test that asserts every Snapshot field round-trips through Rollback. Not done. The `[c7]` test still covers `storyLevel`, `storyXp`, `loginStreak`, `wallet.credits`, materials, world_flags — misses the fields that motivated picking Memento over Unit-of-Work (RNG state, pity, guarantee, collection, party, equipment).

### M-V4-2. No property test for QuestClaimsReducer or ProgressionReducer
**Files:** `Server/Account/tests/ReducerTests/QuestClaimsReducerTest.cpp` (3 example-based), `ProgressionReducerTest.cpp` (4 example-based); `Server/Account/tests/PropertyTests/` (only `WalletPropertyTest.cpp` + `PullsPropertyTest.cpp`).

Carry-forward TC-V3-6. Reducer-asymmetry coverage gap. With H-V2-9's deterministic sibling event keys in claim flows now in production, a `QuestClaimsPropertyTest` that exercises sibling-key uniqueness across arbitrary claim sequences would catch H-V3-12 (partition-local dedup) and v3 event-sourcing M-V3-4 (sibling-keys-don't-include-direction) in one shot.

### M-V4-3. No cross-aggregate replay-determinism test
**Files:** none.

v3 TC-V3-6 carry-forward. With four reducers (wallet, pulls, quest_claims, progression) and `BuildClaimEvents` emitting up to 7 events per claim across multiple aggregates, a single "given event sequence X, fold to state S; replay to same S" test across all four reducers would catch v3 cross-cutting M-V3-1 (event_type vocabulary drift between `BuildClaimEvents` and the reducer dispatch table) and lock in the M-V3-2 Snapshot X-macro coverage convention.

### M-V4-4. `IntegrationDbFixture` snapshot-by-MAX strategy has a small race
**Files:** `Server/Account/tests/Integration/IntegrationDbFixture.hpp:50-55,66-69`.

`MAX(account_id)` + `MAX(outbox_id)` snapshotted at fixture construction; rows `> snapshot` deleted at destruction. Two integration tests running in parallel against the same dev DB (e.g., Catch2 `--filter` shards from a CI runner) would race — the second-constructed fixture snapshots a watermark that includes the first fixture's seeded rows, then deletes them all on teardown. Catch2 default is sequential so this is latent today; documenting the single-runner assumption or gating via a single `PG_ADVISORY_LOCK` at the fixture's `kIntegrationFixtureLockId` would close it.

### M-V4-5. `IntegrationDbFixture` cleanup doesn't include `idempotency_cache` directly
**Files:** `Server/Account/tests/Integration/IntegrationDbFixture.hpp:66-69`.

The fixture relies on `ON DELETE CASCADE` from `accounts` to clean up child rows including `idempotency_cache`. Schema check: `idempotency_cache` has `account_id` as part of PK with `REFERENCES accounts(account_id) ON DELETE CASCADE` per `schema.sql`. Confirmed — works. But the comment at L13 lists `idempotency_cache` among cascaded targets; reviewer should verify (this audit did, but a future schema change to drop the FK would silently regress the fixture's cleanup correctness with no test catching it).

### M-V4-6. `EnvelopeShapeTest` pins `kTestV = 1` separately from `APHELYON_ENVELOPE_VERSION`
**Files:** `Server/Common/tests/EnvelopeShapeTest.cpp:29` (`kTestV = 1`), L54 (`REQUIRE(kTestV == APHELYON_ENVELOPE_VERSION)`).

The intent (per the file header comment) is that bumping `APHELYON_ENVELOPE_VERSION` mechanically forces re-pinning the wire snapshot. The current shape achieves this via a `REQUIRE` that fires immediately, which is correct — but means the failure mode at version-bump-time is a REQUIRE failure on line 54, not the more diagnostic wire-mismatch failure on L75. Cosmetic. Recommend a more explicit failure-mode comment on L54 (or use `STATIC_REQUIRE` if `APHELYON_ENVELOPE_VERSION` is constexpr).

### M-V4-7. `InternalRpcAuthRoundTripTest` doesn't cover duplicate-key payload
**Files:** `Server/Common/tests/InternalRpcAuthRoundTripTest.cpp`.

v3 TC-V3-4 explicitly called for "(d) duplicate-key payload is rejected or canonicalized." The new test file covers (a) string-only, (b) integer, (c) float, (d) bool+null, (e) nested, (f) empty, (g) tamper, (h) mismatched method, (i) stale ts, (j) wrong secret, (k) envelope-version mismatch — but no duplicate-key test. `nlohmann::json::parse` silently keeps last-wins on duplicate keys, so an attacker constructing `{"v":1,"v":2,...}` could theoretically slip past version validation if the receive side parses-then-checks. Worth one more case.

### M-V4-8. Wallet `WalletPropertyTest:122` no longer needs `RC_DISCARD()` per L-V3-5 revert — but still has no oracle for negative-balance prevention
**Files:** `Server/Account/tests/PropertyTests/WalletPropertyTest.cpp`.

L-V3-5 reverted correctly. Separately: the property suite doesn't have an oracle for "no event sequence produces a negative balance" — relevant given H-V3-6 (no DB `CHECK (>= 0)` on wallet columns). A property test asserting `state.credits >= 0` for arbitrary event sequences would catch any reducer bug that violates the invariant before DB enforcement is even possible.

### M-V4-9. No `OutboxRelay` retention / DELETE test
**Files:** `Server/Account/tests/Integration/OutboxRelayTest.cpp` (3 cases — dispatch / mark dispatched / one-shot).

v3 H-V3-4 noted `OutboxRelay::PumpOnce` never DELETEs. When that fix lands, it'll need a test asserting dispatched rows older than a retention window are pruned. Pre-flagging here so the fix doesn't ship test-less.

### M-V4-10. Test-velocity meta-gate from TC-V3-M2 not implemented
**Files:** repo root (no pre-commit hook, no CI check).

v3 TC-V3-M2 recommended a soft warning gate that flags commits touching `Server/Account/src/*Handlers.hpp` or `AccountTransaction.hpp` without a corresponding `tests/**.cpp` change. Not implemented. The v3 remediation arc shipped multiple code-only fixes (H-V3-7 destructor reorder, H-V3-8 `[[nodiscard]]`, H-V3-9 `try_emplace`, C-V3-2 `Begin()` move) — exactly the pattern the gate would flag.

---

## LOW

### L-V4-1. `CombatTests` smoke is `REQUIRE(true)`
Trivially true; doesn't even verify Combat header inclusion. Could be promoted to `REQUIRE(GameLoop::TICK_HZ == 60)` or similar one-liner that fails on header drift.

### L-V4-2. `EventsCompileTest.cpp` not enumerated in v3 dimension
The v3 dimension report references 70 cases but doesn't separately call out `EventsCompileTest.cpp:1` (compile-time guard). Worth noting in the inventory for completeness.

### L-V4-3. `test_main.cpp` files duplicated across all four suites
Each test binary has its own Catch2 main; mechanical duplication. Acceptable per-binary, but a shared `test_main.hpp` would deduplicate one line each.

### L-V4-4. `AuthTests` extraIncludes only covers `Auth/src`; cannot reach `Account/src` headers
By design, but worth noting: an "AuthServer drives an AccountServiceClient" cross-service test cannot live in `AuthTests` today. Either a 5th `IntegrationTests` project or per-test `extraIncludes` override would handle it.

### L-V4-5. `IntegrationDbFixture` `kConn` is the literal dev DSN
Same pattern as `M-V3-2` (DB password literal in `AccountServer.hpp`). Worth a `getenv("APHELYON_TEST_DB_URL")` override with the current literal as the dev default — so CI can point at a separate test DB without recompiling.

### L-V4-6. `[c7]` test header comment refers to v2 file:line, not v3
`AccountTransactionTest.cpp:142` references "Audit M2 (2026-06-02)" inside the c7 region. Minor; comment hygiene only.

### L-V4-7. No test for `H-V3-10` startup ordering (`m_dummyPasswordHash` populated before `Register()`)
v3 H-V3-10 recommended a startup `assert(IterationsOfStoredHash == DEFAULT_ITERATIONS)`. Not in code, not in tests. A unit test that constructs `InternalRpcHandlers` with an empty dummy hash and asserts `VerifyCredentials` rejects or burns a real PBKDF2 would close both v3 H-V3-10 and security M-V3-3 (defense-in-depth: burn one PBKDF2 even on parse-failure).

### L-V4-8. `IntegrationDbFixture` doesn't `pg_advisory_lock` the snapshot watermark
M-V4-4 above; tagged Low because Catch2 is sequential by default. Worth a one-line `pg_advisory_lock` if we ever shard tests.

### L-V4-9. No golden-file test for the `claim_quest_reward` event group
v3 idempotency M-V3-4 noted H-V2-9 sibling keys include currency name but not delta direction; the "one-direction-per-currency-per-claim" invariant has no test. A golden-file test pinning the event sequence for a known quest claim would catch event-vocabulary drift (v3 cross-cutting M-V3-1 — `story_xp_gained` vs reducer dispatch).

---

## Verified Closed from v3

| ID | Title | Evidence |
|---|---|---|
| TC-V3-1 | Integration teardown absent | `IntegrationDbFixture.hpp` added; 18 integration tests converted to `TEST_CASE_METHOD(IntegrationDbFixture, ...)` across 6 files (AccountRepositoryTest, AccountTransactionTest, EventStoreRoundTripTest, OutboxRelayTest, RelationalFlushTest, SnapshotWriterTest). M2-pool-of-1 case correctly excluded with explanatory comment + manual cleanup. |
| TC-V3-4 | C7-C MAC wire-format round-trip | `InternalRpcAuthRoundTripTest.cpp` — 11 cases covering string-only, integer, float, bool+null, nested+array, empty, tamper-rejection, method-mismatch, stale-ts, wrong-secret, envelope-version-mismatch. `EnvelopeShapeTest.cpp` adds golden-file pinning of serialized envelope + MAC for fixed inputs (forces `APHELYON_ENVELOPE_VERSION` bump on any wire-shape change). |
| C-V3-1 | `ProgressionReducer` default `difficulty_tier` | Test in `ProgressionReducerTest.cpp:8-13` now asserts `s.difficulty_tier == 0` from a default-constructed `ProgressionState` (was `= 1` in v3). Schema + Account + reducer + test fixture all agree on 0. |
| H-V3-11 | LruCache Peek (no-touch) | `LruCacheTest.cpp` includes Peek-doesn't-touch and attacker-spray-doesn't-evict-legit cases (per v3 TC-V3-8 inventory) — verified 17 Peek references in the file. RateLimiter consumes via the split API. |
| H-V3-13 | Auth / Combat / Common premake projects | `Server/premake5.lua:439-441` adds `aphelyon_test_project("CommonTests", ...)`, `("AuthTests", ...)`, `("CombatTests", ...)`. Substance is uneven (Combat=1 stub, Auth=2 trivial), but the structural premake gap is closed. |
| H-V3-7 | `m_cache` outlives `m_internalEndpoint` detached threads | `AccountServer.hpp:115-119` adds explicit destructor that calls `Stop()` on the internal endpoint BEFORE any derived member destructs. |
| H-V3-8 | `LockFor` `[[nodiscard]]` | `AccountCache.hpp:247` — `[[nodiscard]] std::unique_lock<std::mutex> LockFor(...)`. |
| H-V3-9 | `InsertIfAbsent` uses `try_emplace` | `AccountCache.hpp:273` — `m_accounts.try_emplace(playerId, std::move(account))` replaces `operator[] = std::move`. |
| C-V3-2 | `Begin()` precedes `GetPity` / `GetGuarantee` (CODE only) | `GachaHandlers.hpp:124,369` — `Begin()` now lifted above the accessor calls. Fix verified in code; **no regression test** (see H-V4-1). |

---

## Recommended v4 top-5

Ordered by audit-leverage (closes the most carry-forward findings per test added).

### 1. `AccountCache` stale-erase + reload + `LockFor` + `InsertIfAbsent` direct unit test
Closes **H-V4-2** (TC-V3-2), the v2 top-5 #1 carry-forward, AND locks in H-V3-8 / H-V3-9 contracts. Estimated ~80 LOC across 4-5 cases. Single largest leverage item. Path: `Server/Account/tests/Unit/AccountCacheTest.cpp` (the `AccountTests` premake glob `%{prj.location}/tests/**.cpp` picks it up automatically).

### 2. C-V3-2 / C7-A regression test driving `HandlePull` rollback
Closes **H-V4-1** AND retroactively validates the v2 C7 fix's correctness on the post-extraction code path. The right shape is a focused `AccountTransaction`-level test that constructs an `Account` with empty `m_pityBySlot`, opens a txn, calls `GetPity`, forces commit failure, asserts pity state restored. Same file as `AccountTransactionTest.cpp` — extends the existing `[c7]` group rather than starting a new suite. Estimated ~40 LOC.

### 3. `AccountRepository` populated round-trip (v2 #3 / v3 #3 / H-V4-3)
Single test that inserts a full account (characters + weapons + gear + gear_substats + char_traces + loadouts + party_slots + quest_states + quest_objectives + world_flags + material_inventory + pity_state), `LoadByAccountId`s, and asserts field equality. Lights up all 9 `Load*` helpers in one test. Estimated ~150 LOC of arrange + 50 LOC of assert. Uses `IntegrationDbFixture` — TC-V3-1 unblocked it.

### 4. End-to-end `HandleAddCurrency` through DB (v2 #5 / v3 #4 / H-V4-4)
The canonical wallet-grant path with zero coverage. Build minimal handler context, fire `HandleAddCurrency` with positive + negative + zero deltas, verify the wallet rows update, events append, and outbox enqueues. Also a natural place to add a property-style oracle asserting `credits >= 0` invariant (M-V4-8). Estimated ~100 LOC.

### 5. `SessionCache::NoteAuthLost` / `NoteAuthRecovered` / `DropExtendedSessions` state-machine test
Closes **H-V4-5** in `CommonTests` (which now exists per H-V3-13). The H6 / M-V2-9 invariants are deterministic and lock-friendly — ideal unit-test surface. Estimated ~60 LOC across 4 cases (extension-on-loss, drop-on-recovery, idempotent-double-loss, idempotent-double-recovery). Bonus: while in this file, add `SessionManager::GetValidSession` cases (H-V4-6) to push `AuthTests` past trivial-defaults coverage.

---

## Audit-vs-fix scorecard (v3 → v4)

| Item | Status |
|---|---|
| v3 Criticals (C-V3-1, C-V3-2) | Both closed in code; C-V3-1 has a test; C-V3-2 does not (H-V4-1) |
| v3 Highs (H-V3-1..14) | Code fixes mostly closed; **test coverage shipped only for H-V3-11 (LruCache Peek), H-V3-13 (premake), and the TC-V3 dimension itself**. H-V3-7/8/9 (concurrency fixes) shipped test-less. |
| v3 Test-coverage dimension (TC-V3-1..9) | TC-V3-1 closed (IntegrationDbFixture); TC-V3-4 closed (InternalRpcAuth + EnvelopeShape); TC-V3-5 partial (premake closed, substance still tiny); TC-V3-2/3/6/7 still open. |
| v2 top-5 | #1, #3, #5 still missing. #2 closed via TC-V3-4. New v3 #5 closed via TC-V3-1. |

**Net new tests shipped between v3 and v4 (~12-15 hours):** ~30+ TEST_CASEs and ~80+ assertions distributed across `LruCacheTest` Peek extensions (4 cases per H-V3-11), `InternalRpcAuthRoundTripTest` (11 cases), `EnvelopeShapeTest` (2 cases), `ProgressionReducerTest` default-assert (1 case), `RateLimiterSmokeTest` (3 cases), `SessionManagerSmokeTest` (2 cases), `CombatTests` smoke (1 case), plus 18 integration tests converted to `TEST_CASE_METHOD(IntegrationDbFixture, ...)`. **Compared to the v2 → v3 arc's 1-test-in-22-commits ratio, this is a step change.** The carry-forward gap is now substance and breadth, not absence.

---

## What v1 + v2 + v3 missed (carries forward)

- **TC-V3-M1 — No `tests/INVARIANTS.md`.** Still missing. Each audit re-litigates "what should be tested." A checklist of invariants the suite is *supposed* to gate would change the audit's question from "is X tested?" to "is X on the gated list?".
- **TC-V3-M2 — No commit-time test gate.** Still missing. The v3 → v4 trajectory improved without a hook, which is encouraging, but the underlying policy is still implicit.
- **(New) v4-M1: no audit-test-traceability layer.** Each finding has a recommended test; no mechanical record links the recommendation to the resulting test (or the absence thereof). When v5 runs, every v4 finding will need to be re-walked against the code+test surface to determine status. A simple `<!-- audit:H-V4-1 -->` comment block convention in test files would let `grep -r "audit:H-V4-" tests/` produce a coverage matrix automatically.

---

## Appendix — files cited

- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\IntegrationDbFixture.hpp` (new)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountTransactionTest.cpp` (6 cases — 5 on fixture, 1 pool-of-1 manual)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountRepositoryTest.cpp` (5 cases — populated round-trip still missing)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\EventStoreRoundTripTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\OutboxRelayTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\RelationalFlushTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\SnapshotWriterTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Account\tests\ReducerTests\ProgressionReducerTest.cpp:8-13` (C-V3-1 closure assertion)
- `D:\dev\starworks\Gacha\Server\Account\tests\LruCacheTest.cpp` (17 Peek references — H-V3-11)
- `D:\dev\starworks\Gacha\Server\Common\tests\InternalRpcAuthRoundTripTest.cpp` (11 cases — TC-V3-4)
- `D:\dev\starworks\Gacha\Server\Common\tests\EnvelopeShapeTest.cpp` (2 cases — wire-shape golden file)
- `D:\dev\starworks\Gacha\Server\Common\tests\RateLimiterSmokeTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Auth\tests\SessionManagerSmokeTest.cpp` (2 trivial cases)
- `D:\dev\starworks\Gacha\Server\Combat\tests\SmokeTest.cpp` (1 stub case)
- `D:\dev\starworks\Gacha\Server\premake5.lua:439-441` (H-V3-13 — premake projects)
- `D:\dev\starworks\Gacha\Server\Account\src\Cache\AccountCache.hpp:247,269-273` (H-V3-8 / H-V3-9 closures)
- `D:\dev\starworks\Gacha\Server\Account\src\AccountServer.hpp:115-119` (H-V3-7 closure)
- `D:\dev\starworks\Gacha\Server\Account\src\Handlers\GachaHandlers.hpp:124,369` (C-V3-2 closure in code; H-V4-1 in tests)
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-03-server-persistence-audit-v3.md`
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-03-v3-followup-test-coverage.md`
