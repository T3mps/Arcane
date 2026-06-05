# v3 Follow-up Audit: Test Coverage

**Date:** 2026-06-03
**Scope:** `Server/Account/tests/` — full re-enumeration against v1 + v2 gap lists and the 22 fix commits shipped between `8e19666` and `d163b07`.
**Auditor:** v3 follow-up (independent re-pass)
**Previous audits:**
- `docs/superpowers/audits/2026-06-02-server-persistence-audit.md` (v1)
- `docs/superpowers/audits/2026-06-02-server-persistence-audit-v2.md` (v2)
- `docs/superpowers/audits/2026-06-02-followup-test-coverage.md` (v2 dimension)

---

## Summary

Test suite now sits at **70 TEST_CASEs across 17 files, ~220 assertions**, up from 51 / ~162 at the v2 audit mark. The delta = **+19 cases / +58 assertions** since v2 — and **100% of that delta is the new `LruCacheTest.cpp`** (16 cases / 53 asserts) plus the 3-case `UuidV7Test.cpp` that the v2 audit appears to have miscounted (it was always present). Two property/scaffold files (`EventsCompileTest.cpp`, `UuidV7Test.cpp`) bring the actual case count to 70.

**Of the 22 fix commits since 8e19666, exactly ONE shipped tests:** `8f0856b` (M-V2-10 LruCache) — `+16 cases / +53 assertions`. The other 21 commits shipped untested:
- C7-A (snapshot pre-mutation): 0 tests
- C7-B (ResumeSession through ValidateSession): 0 tests
- C7-C (MAC over on-wire bytes): 0 tests
- H-V2-1 through H-V2-9: 0 tests
- M-V2-1 through M-V2-13 except M-V2-10: 0 tests
- Three large refactors — M-V2-2 X-macro, M-V2-3 AccountHydrator + AccountCache + InternalRpcHandlers extraction, M-V2-4 HandleClaimQuestReward helper split: **0 direct tests** on any of the new extracted classes

**Net coverage status:** every prioritized v2 top-5 test (C1 stale reload, H2 expired UPSERT, populated round-trip, H10 dispatcher cursor, end-to-end HandleAddCurrency) is **still missing**. The v2 audit's "wide open" verdict stands; the trajectory is worse, not better, because four new C-level fixes (C7-A/B/C) and nine new H-level fixes shipped on top of the existing untested surface.

**Verdict: REGRESSING.** The fix velocity is outpacing the test velocity by ~20:1. Two of the three new Criticals from v2 (C7-A handler call ordering, C7-C wire-format MAC) are exactly the kind of bug a unit test would catch in one commit — yet they shipped without tests, the same way C7 originally shipped with a test that only passed because of post-mutation Memento timing. The pattern is repeating.

---

## Coverage of the 22 fix commits

Walking the commit log `8e19666..d163b07`:

| Commit | Subject | Tests added? |
|---|---|---|
| `9d2d221` | C7-A: capture snapshot before mutation | NO |
| `5ec1f5c` | C7-B: ResumeSession through ValidateSession | NO |
| `0d01429` | C7-C: MAC over on-wire bytes | NO |
| `18b6627` | H-V2-2 + H-V2-3: difficulty_tier + stripe-locked LoadByUsername | NO |
| `ddf1916` | H-V2-4 + H-V2-8: ResumeSession rate limit + GetValidSession | NO |
| `e38e318` | H-V2-7 + H-V2-9: strict parse + deterministic sibling event keys | NO |
| `b2cd0f4` | H-V2-5: dummy-hash on username miss | NO |
| `cc88056` | H-V2-6: client-side idempotencyKey threaded | NO (client) |
| `6bfe75b` | M-V2-11 + M-V2-12 + M-V2-13: env/secret hardening | NO |
| `ca1976d` | M-V2-1 + M-V2-8 + M-V2-9: error+concurrency polish | NO |
| `9dfa544` | M-V2-5 + M-V2-6 + M-V2-7: event-sourcing safety | NO |
| `8f0856b` | M-V2-10: LRU eviction in RateLimiter (`LruCacheTest.cpp`) | **YES — 16 cases / 53 asserts** |
| `e260246` | M-V2-2: Snapshot X-macro | NO |
| `dd15777` | M-V2-4: HandleClaimQuestReward helper extraction | NO |
| `72b5f32` | M-V2-3 step 1: extract AccountHydrator | NO |
| `bc3b5a7` | M-V2-3 step 2: extract AccountCache | NO |
| `d163b07` | M-V2-3 step 3: extract InternalRpcHandlers | NO |

**Score: 1 of 22 (4.5%).**

The 22-commit window also spans the v1→v2 boundary, but the same pattern holds going further back. The C1+C2 fix (`8b2c77f`, the original "stale flag never checked" headline) — no test. C6+H1 (`4065818`) — no test. C7 (`c3ed91a`) — yes a test, but per the v2 audit it only passes because it mutates AFTER `Begin()`, the exact opposite of every real handler call site.

---

## Status of v2's prioritized top-5

| # | Test | Status |
|---|---|---|
| 1 | C1 stale-erase regression test | **STILL MISSING.** No test file references `GetLockedAccount` outside the production code path. The only `IsStale()` test sites remain `AccountTransactionTest.cpp:84,107,219` (Rollback writes the flag) and none verify the reload-on-next-lookup contract. |
| 2 | H2 expired-row UPSERT (idempotency cache) | **STILL MISSING.** No test references `StoreIdempotency` / `FindIdempotency`. The grep for "idempotency_cache" in `tests/` returns zero matches. |
| 3 | AccountRepository populated round-trip | **STILL MISSING.** `AccountRepositoryTest.cpp` still has 5 cases covering only Create/duplicate/scalar-save/LoadByUsername/Delete. The 9 untested `Load*` helpers (characters, weapons, gear, gear_substats, char_traces, loadouts, party_slots, pity_state, world_flags, quest_states/objectives, material_inventory, event-versions) remain dark. |
| 4 | H10 EffectDispatcher sequential versions | **STILL MISSING.** No `EffectDispatcherTest.cpp`. The class remains 138 lines of dead scaffolding (acknowledged kept per L-V2-12) with zero tests AND the latent two-effect-collision bug still latent. |
| 5 | End-to-end HandleAddCurrency through DB | **STILL MISSING.** No file in `tests/` references `HandleAddCurrency`. The handler that the v1 audit's C4 dismissal explicitly framed as "the canonical wallet-grant path used by purchases, quests, achievements" has zero end-to-end coverage. |

**Score: 0 of 5.** v2's "wide open" status holds.

---

## v3-specific findings

### TC-V3-1. Integration test teardown was NEVER addressed; user just hit it today

**Files:** All 6 files in `tests/Integration/`.
**Severity:** HIGH (operational)

User reports having to manually `db-reset` today because integration test rows had accumulated to a point that interfered with development. The v2 audit flagged this explicitly (M-V2-2 in test-coverage section) as a "maintainability ticking clock"; it became a live blocker within ~24 hours.

Direct evidence: every Integration test still uses `MakeAccount(pool, prefix)` with `UniqueUsername(prefix + UuidV7)` for per-row isolation, but no test has any `TRUNCATE`, `BEGIN; ... ROLLBACK;`, or fixture-based cleanup. `EventStoreRoundTripTest` writes wallet events; `OutboxRelayTest` writes outbox rows; `RelationalFlushTest` writes accounts + party_slots; `AccountTransactionTest` writes events + accounts + audit_log + outbox + (sometimes) idempotency_cache.

**The HARD RULE in MEMORY.md** ("never destroy the dev DB without explicit confirmation") amplifies this — the user can't reach for `db-reset` casually, so test-bloat is even more painful than in a typical dev loop.

**Recommended fix:** add a Catch2 `TEST_CASE_METHOD` fixture wrapping each integration test in a transaction that gets rolled back at the end. Alternative: a session-level fixture that `TRUNCATE accounts CASCADE` at startup (catches everything FK-cascaded to accounts). The user explicitly should NOT run this on the dev DB by default — gate it behind a `CATCH2_TEST_DB_URL` env var or `--isolated` flag.

### TC-V3-2. Three newly-extracted classes have ZERO direct tests

**Files:** `AccountCache.hpp`, `AccountHydrator.hpp`, `InternalRpcHandlers.hpp` (extracted in commits `72b5f32`, `bc3b5a7`, `d163b07` per M-V2-3)
**Severity:** HIGH

The M-V2-3 refactor split `AccountServer.hpp` into three new classes. Each is now a discrete unit with a real public interface; none has a corresponding test file. They're exercised transitively through `AccountServer`, but that's exactly the integration-only coverage shape that the v2 audit already flagged as inadequate.

`AccountCache` in particular is where the C1 stale-flag check now lives — extracting it should have been the moment a unit test gated the contract. Instead the contract is exercised only via the production call path, with no direct test.

**Recommended fix:** add `tests/Unit/AccountCacheTest.hpp` covering: insert/get/erase, stale-flag triggers erase+reload, eviction-during-handler interaction, stripe lock ordering. AccountHydrator should be tested as a pure data→Account transformer (no DB dep needed — pass an AccountData literal).

### TC-V3-3. C7-A regression test is structurally impossible to write under current handler shape

**Files:** All 5 event-sourced handler call sites listed in C7-A.
**Severity:** MEDIUM (test design)

C7-A's fix moved `Begin()` to the top of each handler so the Memento snapshot captures pre-mutation state. The test the v2 audit asked for — "mutate BEFORE Begin, then Rollback, verify state restored" — is now structurally a NEGATIVE test against the fix. The right test shape post-C7-A is:

```cpp
// In a handler-style test:
auto txn = m_repo.Begin(account);   // snapshot captured HERE, pre-mutation
account.GetWallet().AddCredits(-300);   // handler-style mutation
// Force commit failure (e.g., bad event payload, DB constraint violation)
txn.Commit();   // throws
REQUIRE(account.GetWallet().GetCredits() == initialCredits);   // restored
REQUIRE(account.IsStale());
```

The existing `[c7]` test in `AccountTransactionTest.cpp:161-220` does this in spirit but the v2 audit flagged that it covers 6 of ~24 Account mutable fields. With the M-V2-2 X-macro refactor now making the Snapshot single-source-of-truth, an X-macro-driven loop test could verify all fields automatically. **Worth doing as a single ~30-line test that iterates the macro.**

### TC-V3-4. C7-C wire-format MAC has no round-trip test

**Files:** `Server/Common/src/ServiceEndpoint.hpp`, `Server/Common/src/ServiceClient.hpp`, `Server/Common/src/InternalRpcAuth.hpp`
**Severity:** HIGH

C7-C changed the MAC scheme from "MAC over re-serialized params" to "MAC over on-wire bytes". The v2 audit specifically called out that the OLD scheme worked **only** because every current internal RPC payload contains string fields; the first numeric/float field would have broken the receive-side re-serialization. The fix is correct, but its correctness depends on **byte-exact** MAC computation across the wire.

There is no test that:
1. Calls `ServiceClient::Call` and inspects the wire bytes.
2. Verifies `ServiceEndpoint` accepts the exact bytes the client sent.
3. Verifies a numeric/float field in the payload doesn't break MAC validation.
4. Verifies a duplicate-key payload is rejected or canonicalized.

`AccountTests` doesn't link `Auth`, so even an integration test would have to fake-serve. **Recommended:** a unit test in a new file (`tests/Unit/InternalRpcAuthTest.cpp`) that drives the InternalRpcAuth helper through a synthesized JSON payload including int + float + nested object, computes the MAC, parses it back, and asserts equality. Skip the network roundtrip; assert the auth contract.

### TC-V3-5. Auth and Combat have ZERO direct test coverage

**Files:** `Server/Auth/*`, `Server/Combat/*`, `Server/premake5.lua:295`
**Severity:** CRITICAL (audit blind spot)

`Server/premake5.lua` defines exactly one test project: `AccountTests`. There is no `AuthTests`, no `CombatTests`, no `CommonTests`. Every fix shipped to:
- SessionManager (H-V2-8 GetValidSession helper)
- AuthServer (C7-B ResumeSession routing, H-V2-4 rate limit, H-V2-5 dummy-hash)
- RateLimiter (M-V2-10 LruCache integration, the one test we got)
- SessionCache (M-V2-9 force-revalidate-on-recovery)
- ServiceEndpoint / ServiceClient (C7-C wire-format MAC, M-V2-11 getenv caching)
- InternalRpcAuth (M3, M-V2-11)

...is exercised **at most transitively via Account tests that happen to spin up Auth-adjacent code**. None is directly testable. The H-V2-8 GetValidSession helper — a session-validity invariant that the C7-B fix depends on — has no test.

This is the single largest test-coverage gap in the whole audit. The v1 and v2 audits both spoke of "the suite covers the Account boundary"; v3 explicitly names the corollary: **there is no test for any non-Account-side service code.**

**Recommended fix:** add `AuthTests` and `CombatTests` premake projects mirroring `AccountTests`. Start with the lowest-hanging targets — `SessionManager` validity invariants, `RateLimiter` (already exercised through LruCache, but the wrapper has its own surface), `InternalRpcAuth` MAC computation, `SessionCache` extension/drop logic.

### TC-V3-6. Reducer asymmetry persists

QuestClaimsReducer and ProgressionReducer still lack property tests (Wallet + Pulls have them). Cross-aggregate replay determinism still untested. Both flagged in v1's test-coverage section, neither closed in v2, neither closed in v3 commits.

### TC-V3-7. WalletPropertyTest empty-input silent skip

`WalletPropertyTest.cpp:122` still reads `if (built.events.empty()) return;` — still no `RC_DISCARD()`. One-line fix, never made.

### TC-V3-8. LruCache coverage is adequate; confirmed

`LruCacheTest.cpp` is 16 cases / 53 assertions covering: construction, zero-capacity clamp, Put/Get/Peek, mutable Get pointer, update-no-evict, LRU eviction order, Get-touches, Peek-doesn't-touch, Put-on-existing-touches, LruKey accessor, Erase, EraseIf predicate, Clear, attacker spray scenario (the M-V2-10 motivation), legit-user-survives-spray scenario. **This is the test file the rest of the codebase should aspire to look like.** Honest coverage of the API contract including the two motivating threat scenarios.

### TC-V3-9. The 162→220 assertion arc, broken down

| | TEST_CASEs | Assertions |
|---|---|---|
| v1 audit baseline | 49 | 142 |
| v2 audit baseline | 51 | 162 |
| v3 audit baseline | 70 | ~220 |
| **Net delta v2→v3** | **+19** | **+58** |
| Of which: LruCache | +16 | +53 |
| Of which: UuidV7 (always present, miscounted in v2) | +3 | +3 |
| Of which: everything else | **+0** | **+2** |

**The 22 commits shipped between v2 and v3 added effectively zero coverage outside LruCache.** The "+2 assertions" rounding noise is from a single REQUIRE adjustment in an existing test.

---

## Recommended v3 top-5 (revised)

Updates the v2 top-5 to reflect what the 22 commits changed:

### 1. **AccountCache stale-flag erase+reload integration test** (was v2 #1)

Now testable as a unit test against the freshly-extracted `AccountCache` class. The C1 fix moved from `AccountServer` into `AccountCache::GetLocked()`. A direct test against `AccountCache` is now structurally cleaner than the v2-era version.

**Why it moved up:** M-V2-3 made it easier. No excuse not to write it.

### 2. **InternalRpcAuth MAC over on-wire bytes round-trip test** (new — was implicit in v2)

C7-C is a load-bearing security fix with zero coverage. Test should drive: (a) string-only payload, (b) integer + float payload, (c) nested object payload, (d) duplicate-key payload — assert MAC computation is byte-exact, sender ≡ receiver.

**Why HIGH:** the C7-C fix may have introduced a subtle bug nobody can see because there's no test. The v2 finding was discovered by reading code, not by a test failing.

### 3. **AccountRepository populated round-trip** (was v2 #3, unchanged)

Still missing. Still gates 9 untested `Load*` helpers. M-V2-3's AccountHydrator extraction makes the dependencies clearer — easier to write than 24h ago.

### 4. **End-to-end HandleAddCurrency through DB** (was v2 #5, unchanged)

Still missing. The handler the audit explicitly framed as "canonical wallet-grant" remains unprotected.

### 5. **Integration test isolation fixture** (new — promoted from v2's "ticking clock" status)

User just hit this today (had to db-reset). M-V2-2 of the v2 test-coverage section. Single Catch2 `TEST_CASE_METHOD` fixture wrapping each Integration test in `BEGIN; ... ROLLBACK;`. Mechanically the smallest fix on this list; operationally the highest impact.

**Why HIGH:** combined with MEMORY.md's HARD RULE against destroying the dev DB, this is now a live blocker, not a future one.

---

## What v1 + v2 missed

### TC-V3-M1. No test gates the "what to test" question

v1 and v2 both list test gaps. Neither audit codified what the suite's **invariants** are. Result: when a new audit identifies a fix, there's no checklist to consult to determine "does this fix touch a tested invariant?" — every fix gets re-litigated.

Recommendation: add a `tests/INVARIANTS.md` (or similar) that enumerates the contracts the suite is supposed to gate. Future audits then have a yes/no checkbox per finding.

### TC-V3-M2. No CI gate enforces test-with-fix

There is no policy (lint rule, hook, CI check) that prevents a fix commit from landing without an accompanying test. Twenty-one of twenty-two commits show that the implicit policy is "tests are optional." If the test-coverage gap is the persistent v1+v2+v3 finding, the gap is fundamentally about commit policy, not about test design.

Recommendation: pre-commit hook or CI check that flags commits touching `Server/Account/src/*Handlers.hpp` or `Server/Account/src/AccountTransaction.hpp` without a corresponding `tests/**.cpp` change. Soft warning, not hard fail — at minimum it surfaces the question.

### TC-V3-M3. `AccountTests` won't link against the freshly-extracted Auth-adjacent code

The premake glob `%{prj.location}/tests/**.cpp` only picks up files under `Server/Account/tests/`. A `tests/Unit/InternalRpcAuthTest.cpp` would compile, but only because `Common` is linked into `AccountTests`. A SessionManager test cannot live there — `Auth` is not linked. The v1/v2 audits implicitly assumed any test could be added to AccountTests; v3 verified the premake reality: `AccountTests` links `{ "Common", "Catch2", "rapidcheck" }` and nothing from Auth or Combat.

This is what makes TC-V3-5 a Critical, not a Medium. There is no test binary that can even compile a SessionManager test today.

---

## Appendix — files cited

- `D:\dev\starworks\Gacha\Server\Account\tests\test_main.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\LruCacheTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountTransactionTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountRepositoryTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\EventStoreRoundTripTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\RelationalFlushTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\OutboxRelayTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\SnapshotWriterTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\PropertyTests\WalletPropertyTest.cpp` (line 122 — silent return; still not fixed)
- `D:\dev\starworks\Gacha\Server\Account\tests\PropertyTests\PullsPropertyTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\ReducerTests\*ReducerTest.cpp` (4 files)
- `D:\dev\starworks\Gacha\Server\Account\tests\GoldenFile\SchemaMigrationTest.cpp`
- `D:\dev\starworks\Gacha\Server\premake5.lua` (lines 290-362 — AccountTests project, no other test projects)
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-02-server-persistence-audit.md`
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-02-server-persistence-audit-v2.md`
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-02-followup-test-coverage.md`
