# v5 Follow-up Audit: Test Coverage

**Date:** 2026-06-03 (same-day successor to v4, after the 20-commit v4 remediation arc)
**Scope:** `Server/Account/tests/`, `Server/Common/tests/`, `Server/Auth/tests/`, `Server/Combat/tests/`, `Server/Account/tests/Integration/IntegrationDbFixture.hpp`, `Server/Account/tests/events/*.json`, plus the production surface the new tests are supposed to gate (handler/reducer/repository/transaction/cache/relay).
**Auditor:** v5 follow-up (read-only)
**Previous audits:**
- `2026-06-03-server-persistence-audit-v4.md` (v4 synthesis)
- `2026-06-03-v4-followup-test-coverage.md` (v4 dimension)
- `2026-06-03-v3-followup-test-coverage.md` (v3 dimension)
- `2026-06-02-followup-test-coverage.md` (v2 dimension)

---

## Verdict

**Posture: improving — but the remediation arc itself introduced new code with zero direct test coverage.** The v4 remediation arc was the first audit cycle whose dominant signal is "the test backlog actually moved." Across 20 commits, **5 are test-only commits** (`39ccfa7`, `5fcb70d`, `9769d37`, `8ff1491`, `6357419`) adding 27 new `TEST_CASE`s and ~297 new assertions. v4's top-5 carry-forwards from v2 (AccountCache stale round-trip, AccountRepository populated round-trip, end-to-end HandleAddCurrency, C-V3-2 regression, idempotency machinery) are **closed**. The single largest carry-forward — "M-V2-3 extracted three classes explicitly to be testable; no direct tests one day after extraction" — is closed at 10 cases / ~67 assertions across three dedicated files. The fix-vs-test ratio is now ~1:1 — every Critical fix in v4 had at least one test-only commit downstream. That is a step change from v3→v4 (~25 of 35 commits added tests but most were piggy-backed onto fixes, not standalone test commits) and a categorical change from v2→v3 (1-of-22).

That said, **three of v4's Critical/High wiring fixes shipped without their own tests.** The pattern that produced C-V4-1 (OutboxRelay never instantiated) and C-V4-2 (`Probe` never inspected response body) repeats here, just shifted up the stack: the *fixes* now have test holes instead of the *bugs* that the audits caught. The connection caps added in H-V4-10 (`TcpServerBase::Start` lines 119-139) have **zero** test coverage of the refusal path. The three OutboxRelay sweepers wired in C-V4-1 (`PruneDispatchedOutbox`, `SweepExpiredIdempotency`, `RunPartmanMaintenance`) have **zero** test coverage. The `Probe` response-body classification fix in C-V4-2 has no direct test of the four reachable outcomes — `EnvelopeShapeTest` verifies the auth contract over the wire but doesn't drive `ServiceClient::Probe()` through a synthetic server that returns the four error shapes. None of these are existential test gaps — but every audit cycle that closes a Critical with code-only changes is a setup for the next cycle to find that the fix needs its own fix.

Two structural items remain open from every prior audit: TC-V3-M1 (no `tests/INVARIANTS.md`) and TC-V3-M2 (no commit-time test-velocity gate). The v4-M1 audit-test traceability convention (`<!-- audit:H-V4-N -->` in test files) was recommended one day ago and adoption is **zero** — `grep -r "audit:H-V" Server/*/tests` returns 0 hits.

No new Criticals.

---

## Test inventory verification

Running each binary on `main` HEAD (commit `d65dbdf`):

| Binary | TEST_CASEs | Assertions |
|---|---|---|
| `AccountTests.exe` | 97 | 739 |
| `CommonTests.exe` | 17 | 33 |
| `AuthTests.exe`   | 2  | 6  |
| `CombatTests.exe` | 1  | 1  |
| **Total** | **117** | **779** |

vs v4 baseline of 70 cases / 442 assertions: **+47 cases / +337 assertions across 4 binaries**.

Of the +47 cases in AccountTests alone:
- AccountCacheTest: 3 (`39ccfa7`, H-V4-5)
- AccountHydratorTest: 2 (`39ccfa7`, H-V4-5)
- InternalRpcHandlersTest: 5 (`39ccfa7`, H-V4-5)
- PityRollbackRegressionTest: 2 (`5fcb70d`, H-V4-6)
- PopulatedRoundTripTest: 1 (`9769d37`, H-V4-7)
- AddCurrencyEndToEndTest: 2 (`9769d37`, H-V4-7)
- IdempotencyMachineryTest: 5 (`8ff1491`, H-V4-8)
- SchemaMigrationTest: +2 new cases (`6357419`, cross-cutting M-V4-2)
- Pre-existing case count: 75 → 97 = +22 new

The numbers reconcile.

---

## CRITICAL

*(None. Test coverage Criticals are rare; the v4 remediation arc demonstrably moved the needle.)*

---

## HIGH

### H-V5-1. `TcpServerBase` connection-cap refusal path has zero test coverage
**Files:** `Server/Common/src/Net/TcpServerBase.hpp:119-139` (H-V4-10 fix); `Server/Common/tests/` (no file references `MAX_CONNECTIONS_TOTAL` or `MAX_CONNECTIONS_PER_IP`); grep returns 0 hits.
**Status:** NEW (v4 fix without test).

H-V4-10 added per-IP and total connection caps inside the accept loop. The caps are exactly the kind of defense the audit's threat model relies on — slow-loris fan-out from 1000 IPs is the motivating attack — and the refusal branches (lines 121-127 total cap, 129-138 per-IP cap) have no automated regression. A future refactor that, e.g., increments `perIpCount` before the cap check (introducing a permanent off-by-one ban for the attacker's IPs) would land silently. The cap values themselves (`2048` total, `16` per-IP) are constants in `TcpSocket.hpp:48-49` with no test asserting their plausibility.

**Recommended:** a unit-style test that instantiates a minimal `TcpServerBase` subclass on `localhost:0`, opens 17 connections from `127.0.0.1`, and asserts the 17th gets closed within ~10 ms with no thread-spawn side effect. Same shape for the total cap with a smaller `MAX_CONNECTIONS_TOTAL` override (or a runtime-injectable cap if the constants stay compile-time).

### H-V5-2. OutboxRelay's three sweep methods have zero direct test coverage
**Files:** `Server/Account/src/Db/OutboxRelay.hpp:115-152` (`PruneDispatchedOutbox`, `SweepExpiredIdempotency`, `RunPartmanMaintenance`); `Server/Account/tests/Integration/OutboxRelayTest.cpp` covers only `PumpOnce` (3 cases — dispatch / handler-returns-false / no-handler-registered).
**Status:** NEW (C-V4-1 wire-fix without test).

C-V4-1's headline was that the **class** existed but was never instantiated — operational consequence was three months of unbounded `outbox`/`idempotency_cache` growth + `events` partition exhaustion at ~12 months post-launch. `f24b3ca` wired the relay onto `AccountServer`. But the three sweep methods that justify the C-rating — they're literally the reason the audit raised it from H to C — have no tests asserting they actually delete the right rows. A regression where, e.g., `PruneDispatchedOutbox` swaps `<` and `>` in the interval comparison (deleting recent rows, keeping ancient ones) would land silently. The `OutboxRelayTest` already uses `IntegrationDbFixture`, so the path to coverage is short.

**Recommended:** three integration cases extending `OutboxRelayTest.cpp` —
1. Seed two outbox rows (one with `dispatched_at = now() - INTERVAL '25 hours'`, one with `dispatched_at = now() - INTERVAL '23 hours'`); call `PruneDispatchedOutbox(24)` via a friend declaration or by exposing it on the public API; assert only the 25h row is gone.
2. Seed two idempotency_cache rows (one `expires_at < now()`, one `> now()`); call `SweepExpiredIdempotency`; assert only the expired row is gone.
3. Call `RunPartmanMaintenance()`; assert `partman.part_config_sub.last_run` advanced for the `events` parent (proves `CALL partman.run_maintenance_proc()` actually ran). pg_partman is installed in the test DB via `Dockerfile.postgres`, so this test runs against the same fixture as the rest.

The cadence-counter behavior (every 120/130/7200 pumps) is harder to test deterministically without exposing the counter; that's a Low item, not part of this finding.

### H-V5-3. `ServiceClient::Probe` response-body classification has no direct test
**Files:** `Server/Common/src/Net/ServiceClient.hpp:289-325` (C-V4-2 fix); `Server/Common/tests/` (no file references `Probe`, `ProbeOutcome`, `AuthFailed`, or `EnvelopeVersionMismatch` outside `InternalRpcAuthRoundTripTest` which only round-trips the MAC, not the probe).
**Status:** NEW (C-V4-2 fix without test).

C-V4-2 fixed `Probe()` to inspect `rpc.value["error"]` so that `AuthFailed` and `EnvelopeVersionMismatch` outcomes are reachable. The fix is verified in code (`ServiceClient.hpp:299-305`) but no test drives `Probe` against a synthetic `ServiceEndpoint` (or a mock) that returns each of the four error shapes:
- `{"error": "unsupported_envelope_version", "expected_v": N}` → `EnvelopeVersionMismatch`
- `{"error": "Authentication failed"}` → `AuthFailed`
- `{"error": "something else"}` → `UnknownError`
- `{}` (missing `"result"`) → `UnknownError`
- `{"result": ...}` → `Ok`

A future refactor that reads `value.at("error")` instead of `value.value("error", "")` (would throw on missing-key) breaks all five paths simultaneously, and only an integration test that probes a misconfigured peer in dev catches it. Probe is meant to be the canary; the canary has no test.

**Recommended:** in `CommonTests`, a test that constructs an in-process `ServiceEndpoint` with a method that returns the four error shapes (or use `InvokeForTest` to simulate them inline), then drives `Probe` and asserts the `ProbeOutcome` value. Tightly coupled enough to be brittle if the error strings change — that's the point.

### H-V5-4. `ServiceEndpoint::InvokeForTest` is a public test backdoor with no usage discipline
**Files:** `Server/Common/src/Net/ServiceEndpoint.hpp:66-77` (added in `39ccfa7`); production grep shows zero callers outside `InternalRpcHandlersTest.cpp:53,80,103,144,164`.
**Status:** NEW (test backdoor; risk is *production* mis-use, not test mis-use).

`InvokeForTest` is documented as test-only in its comment — "Production paths never call this — real dispatch goes through the verified accept loop in HandleConnection." But the method is `public`, has no `#ifdef CATCH_CONFIG_RUNNER` guard, and exists on every `ServiceEndpoint` instance in Release builds. A future production change that adds `endpoint.InvokeForTest(...)` to skip envelope MAC verification (e.g., to make a "loopback admin call" path) bypasses the entire authenticated-RPC contract. The Common premake builds this into every consumer.

**Recommended:** either (a) move `InvokeForTest` behind a `#ifdef APHELYON_TEST_BUILD` guard (define in the test premake's `defines` block and gate this method), or (b) rename to a stronger marker like `UNSAFE_DispatchSkippingAuth_TestsOnly_DoNotCallInProduction` so a code reviewer can't miss it, or (c) make it a `friend` of the test fixture rather than `public`. Option (a) is the right one — keeps the test file unchanged and prevents accidental production use at link time.

This is High, not Critical, because the *current* call sites are all tests; the risk is forward-looking. The same shape of mistake the v4 audit caught with `OutboxRelay` (well-implemented class never wired correctly — except the inverse here, well-implemented method wired *too* permissively).

### H-V5-5. `outbox.account_id` population (H-V4-3) has no test asserting the FK actually populates
**Files:** `Server/Account/src/Cache/AccountTransaction.hpp:228-232` (H-V4-3 fix); `Server/Account/schema.sql` outbox table; `Server/Account/tests/Integration/AddCurrencyEndToEndTest.cpp` and `OutboxRelayTest.cpp` (neither asserts `outbox.account_id IS NOT NULL` post-commit).
**Status:** NEW (H-V4-3 fix without test).

H-V4-3 added `account_id BIGINT REFERENCES accounts(account_id) ON DELETE CASCADE` to the outbox table and updated `AccountTransaction::Commit()`'s outbox INSERT to populate it. The fix is verified in code. But no test asserts:
1. A `txn.EmitToOutbox(...)` actually lands a non-NULL `account_id` in the outbox row.
2. Deleting an account cascades (deletes the outbox rows referencing it).
3. Raw infrastructure inserts (the `OutboxRelayTest` path that bypasses `AccountTransaction`) correctly land NULL `account_id` and aren't FK-rejected.

`AddCurrencyEndToEndTest` already opens a pool connection to inspect `events`; one more query against `outbox` would pin (1). For (2), an integration test that creates an account, emits an outbox row, deletes the account, and asserts no surviving outbox rows close the GDPR-pathway claim of the fix.

### H-V5-6. `Account` Snapshot X-macro round-trip test still not written
**Files:** `Server/Account/src/State/Account.hpp:117-148` (X-macro listing 14+ direct fields + 2 indirect); `Server/Account/tests/Integration/AccountTransactionTest.cpp` (the `[c7]` test still only covers 6 of ~24 mutable fields).
**Status:** REPEAT (v3 TC-V3-3, v4 M-V4-1).

The X-macro made this writable as a single ~30-line test that iterates all direct fields, mutates, then rolls back and asserts restoration. The `5fcb70d` PityRollbackRegressionTest is the right shape but covers exactly two fields (`m_pityBySlot`, `m_guaranteeBySlot`) via two whole TEST_CASEs. The v3-TC-V3-3 recommendation of one parameterized walk over the macro hasn't landed; the cost of touching this is roughly free now that the contract is clear (Pity test proved the snapshot mechanism works); the leverage is closing a 3-audit-cycle carry-forward.

**Recommended:** one ~50-LOC test that mutates one canonically-different value per direct field, calls `Rollback()`, and asserts restoration. The X-macro itself drives the assertion loop, so adding a new snapshot field auto-extends test coverage.

### H-V5-7. `SessionCache::NoteAuthLost` / `NoteAuthRecovered` / `DropExtendedSessions` untested
**Files:** `Server/Common/src/Net/SessionCache.hpp`; `Server/Common/tests/` (no file).
**Status:** REPEAT (v4-H5 test-coverage dimension, v3 implied via TC-V3-5).

`CommonTests/test_main.cpp:4` lists SessionCache among the targets but no test file exists. The H6 / M-V2-9 force-revalidate-on-recovery state machine is the highest-leverage untested unit-testable target in the suite — pure lock-friendly state machine, no DB dep. Carry-forward from v4.

### H-V5-8. `SessionManager::GetValidSession` (H-V2-8) untested
**Files:** `Server/Auth/src/SessionManager.hpp`; `Server/Auth/tests/SessionManagerSmokeTest.cpp` (still 2 trivial cases — defaults + SetConfig round-trip).
**Status:** REPEAT (v4-H6 test-coverage dimension, v3 TC-V3-5 subset).

The header comment in `SessionManagerSmokeTest.cpp` still acknowledges the gap: *"the H-V2-8 GetValidSession single-acquisition / C7-B IP-binding / M9 idle-bump-skip invariants are flagged in the v3 audit's test-coverage section and will be added here incrementally."* The carry-forward is now 3 audit cycles deep.

---

## MEDIUM

### M-V5-1. Auth/Combat substance still tiny (2 + 1 cases)
**Files:** `Server/Auth/tests/SessionManagerSmokeTest.cpp` (2 cases); `Server/Combat/tests/SmokeTest.cpp` (1 stub case).
**Status:** REPEAT (v3 TC-V3-5, v4-H7 test-coverage).

Premake structural gap is closed (`H-V3-13`); substance is still nominal. CombatTests remains a single `REQUIRE(true)` stub. AuthTests covers `SessionConfig::defaults` and `SetConfig` round-trip; doesn't touch `IsValidToken`, `ValidateAndExtendOrInvalidate`, `IssueToken`, or the session map. Combined "Auth + Combat" suite contributes 7 assertions vs Account's 739.

### M-V5-2. No `tests/INVARIANTS.md` written; no v4-M1 audit-test traceability comments
**Files:** None.
**Status:** REPEAT (v3 TC-V3-M1, v4-M1 cross-cutting).

`grep -r "audit:H-V" Server/Account/tests/` returns 0 hits. v4-M1's recommendation was a soft convention — comment block `// Audit: H-V4-5` in the test file body, mechanically searchable. The new test files added by v4 remediation (`39ccfa7`, `5fcb70d`, `9769d37`, `8ff1491`) all have natural-language audit references in their headers ("Audit H-V4-5", "Audit H-V4-6", etc.) but **don't** use the proposed machine-grep convention. Each future audit will re-walk all findings to determine coverage status manually.

**Recommended:** establish the convention now while the trail is fresh. One-pass sweep through the v4-arc test files (5 files) adding `// audit:H-V4-N` comments costs ~10 minutes; backfilling later when files have grown is the painful version.

### M-V5-3. No `tests/INVARIANTS.md`
**Files:** None.
**Status:** REPEAT (v3 TC-V3-M1, v4 test-coverage tail).

Same shape as M-V5-2 but separately tracked because it's a different artifact. Recommendation unchanged from v3 — a list of contracts the suite is supposed to gate (e.g., "C1: failed Commit's mutations don't leak into next cache lookup", "C7-A: Begin() captures pre-mutation state", "MAC validation rejects bit-flipped envelopes"), checkbox-style. Each new audit's findings can then be assessed against the list rather than re-litigating "should this be tested?"

### M-V5-4. No commit-time test-velocity gate (TC-V3-M2)
**Files:** None.
**Status:** REPEAT (v3 TC-V3-M2, v4 test-coverage tail).

Soft warning gate that flags commits touching `Server/Account/src/*Handlers.hpp` or `AccountTransaction.hpp` or `*Cache*.hpp` without a corresponding `tests/**.cpp` change. The v3→v4 trajectory improved without a hook, encouraging — but the v4 arc's three Critical/High wiring fixes (C-V4-1, C-V4-2, H-V4-10) shipped without tests anyway. The implicit policy is still "tests are optional for wiring fixes."

### M-V5-5. C7-C / wire-format MAC + envelope test doesn't drive a real `ServiceClient` ↔ `ServiceEndpoint` round-trip
**Files:** `Server/Common/tests/InternalRpcAuthRoundTripTest.cpp` (12 cases — all on the auth helper directly), `Server/Common/tests/EnvelopeShapeTest.cpp` (2 cases — golden file).
**Status:** REPEAT (v4 M-V4-7, sharpened post-C-V4-2).

The test suite verifies (a) the MAC helper round-trip and (b) the wire-shape pin. It does NOT verify (c) a real `ServiceClient::Call` against a real local `ServiceEndpoint` actually validates end-to-end — including the receive-side parse of the framed bytes, the timestamp window, and the version check. The closest existing surface is the `Probe` integration H-V5-3 above asks for; if that test is built right, it subsumes this one. Tagged Medium because the helper-level coverage is strong; the gap is "no one test goes through both layers."

### M-V5-6. No property test for `QuestClaimsReducer` or `ProgressionReducer`
**Status:** REPEAT (v4 M-V4-2, v3 TC-V3-6).

Two reducers still example-based only. A QuestClaimsPropertyTest exercising sibling-key uniqueness across arbitrary claim sequences would catch H-V3-12 (partition-local dedup) regressions; a ProgressionPropertyTest with an oracle for "no event sequence produces story_xp < 0 or story_level outside [1, MAX]" would lock the StoryLevelAdvanced apply path.

### M-V5-7. No cross-aggregate replay-determinism test
**Status:** REPEAT (v4 M-V4-3, v3 TC-V3-6).

Four reducers (wallet, pulls, quest_claims, progression). `BuildClaimEvents` emits up to 7 events per claim across multiple aggregates. A "given event sequence X, fold to state S; replay to same S" test across all four reducers would catch event_type vocabulary drift (the same class of bug the new `credits_added`/`story_xp_gained` golden fixtures partially close).

### M-V5-8. `IntegrationDbFixture` cleanup race + DSN hardcoding
**Files:** `Server/Account/tests/Integration/IntegrationDbFixture.hpp:42-43` (literal DSN), `:50-55,66-69` (MAX-based snapshot).
**Status:** REPEAT (v4 M-V4-4 + L-V4-5).

`MAX(account_id)` + `MAX(outbox_id)` snapshot is race-prone under parallel test execution (Catch2 sequential default keeps it latent). DSN is the dev literal — `getenv("APHELYON_TEST_DB_URL")` override would let CI point at a separate test DB without recompiling. Carry-forward unchanged from v4.

### M-V5-9. `WalletPropertyTest` still lacks "no negative balance" oracle
**Files:** `Server/Account/tests/PropertyTests/WalletPropertyTest.cpp`.
**Status:** REPEAT (v4 M-V4-8).

H-V3-6 added DB `CHECK (>= 0)` constraints on wallet columns. A reducer property test asserting `state.credits >= 0` for arbitrary event sequences would catch any reducer bug that violates the invariant before DB enforcement fires. The DB constraint is the safety net; the property test is the unit-level oracle.

### M-V5-10. `InternalRpcAuthRoundTripTest` still missing duplicate-key payload coverage
**Files:** `Server/Common/tests/InternalRpcAuthRoundTripTest.cpp` (12 cases).
**Status:** REPEAT (v4 M-V4-7).

v3 TC-V3-4 explicitly listed "(d) duplicate-key payload is rejected or canonicalized" as required. `nlohmann::json::parse` silently keeps last-wins on duplicate keys; an attacker constructing `{"v":1,"v":2,...}` could in principle slip past version validation if the receive-side parses-then-checks. One additional test case closes this.

---

## LOW / OBSERVATION

### L-V5-1. `H-V4-6` (PityRollbackRegressionTest) test rationale depends on `m_rawPity` AND `m_pityBySlot` both being snapshotted
**Files:** `Server/Account/tests/Integration/PityRollbackRegressionTest.cpp:64-75`.
The test asserts `account.GetPityState().count(slotId) == 0` post-Rollback. `GetPityState()` (Account.hpp:240) merges `m_rawPity` and `m_pityBySlot`. The X-macro at Account.hpp:117-148 snapshots both, so this is correct. A future refactor that splits these into `direct` / `lazy` maps and only snapshots one would silently invalidate the regression signal — the test would still pass on the lazy side via the unsnapshotted side. Observation only; not actionable today.

### L-V5-2. `InternalRpcHandlersTest` constructs `ServiceEndpoint` on port 0 and never calls `Start()`
**Files:** `Server/Account/tests/Integration/InternalRpcHandlersTest.cpp:37,45,71,93,127,158`.
Working as intended — the test uses `InvokeForTest` precisely to avoid binding a socket. But this means the bind/accept path is exercised by no test (H-V5-1 above for the cap path; the regular bind path is exercised only at runtime).

### L-V5-3. `AccountHydratorTest` covers identity + scalar state but skips collection/quest/dirty round-trips
**Files:** `Server/Account/tests/Integration/AccountHydratorTest.cpp`.
The two tests cover wallet + story scalar fields. AccountHydrator's full surface includes 9+ collection sub-objects (characters, weapons, gear, traces, party, weapon equipment, quest states, materials, world flags). The PopulatedRoundTripTest covers the full round-trip via the repo but goes through `Save+LoadById`, not through `AccountHydrator::FromData` directly. A "FromData with populated AccountData" unit case would close the hydrator-level coverage without DB.

### L-V5-4. `AddCurrencyEndToEndTest` doesn't assert outbox row landed
**Files:** `Server/Account/tests/Integration/AddCurrencyEndToEndTest.cpp:91-103`.
The test inspects `events` (3 fields) and `accounts.credits` (1 field) and `FindIdempotency` (2 fields), but doesn't query `outbox` to assert a row landed. HandleAddCurrency doesn't emit outbox today, so this is correct — but the test would be the natural place to add an outbox assertion if the handler ever emits one. Observation; pre-flagged so the test doesn't drift.

### L-V5-5. `EnvelopeShapeTest`'s `kTestV = 1` decoupling from `APHELYON_ENVELOPE_VERSION`
**Files:** `Server/Common/tests/EnvelopeShapeTest.cpp:29,54`.
Carry-forward (v4 M-V4-6). Working as intended (the `REQUIRE(kTestV == APHELYON_ENVELOPE_VERSION)` forces re-pin on bump); the failure mode at version-bump-time is just less diagnostic than a wire-mismatch failure on L75. Cosmetic.

### L-V5-6. `OutboxRelayTest`'s 50ms poll interval + 2s deadline brittle on slow CI
**Files:** `Server/Account/tests/Integration/OutboxRelayTest.cpp:46-49`.
Working today, but the 2-second deadline is tight if CI runs Postgres under load. Bumping the deadline to 5s costs nothing and reduces flake risk. Observation only.

### L-V5-7. `IdempotencyMachineryTest`'s "ON CONFLICT DO UPDATE" case sleeps 1 second
**Files:** `Server/Account/tests/Integration/IdempotencyMachineryTest.cpp:113-118`.
The test sleeps ≥1s so `now()` advances past truncated-second precision. Documented in the test comment. Acceptable but adds ~1s to the suite. A future optimization: capture the exact `INSERT now()` returned timestamp via `RETURNING expires_at` and verify the second insert strictly greater, no sleep needed.

### L-V5-8. `PopulatedRoundTripTest` is one giant `TEST_CASE`
**Files:** `Server/Account/tests/Integration/PopulatedRoundTripTest.cpp` (1 case, 7 assertion blocks).
H-V4-7 closure shape is "single test case loaded with all sub-systems." If it fails, the operator gets one failed case and has to read the body to identify which sub-system regressed. SECTION-based decomposition would localize the failure signal at zero functional cost. Cosmetic / Catch2 idiom.

### L-V5-9. `EventsCompileTest.cpp` compile-time guard isn't documented in audit lineage
**Files:** `Server/Account/tests/EventsCompileTest.cpp` (1 case).
Worth a one-line comment cross-referencing whatever class of bugs (event header drift, namespace breakage, etc.) it's gating against. Observation only.

### L-V5-10. `WalletPropertyTest.cpp:130` silent early-return for empty event list
**Files:** `Server/Account/tests/PropertyTests/WalletPropertyTest.cpp:130`.
v3 L-V3-5 reverted correctly (the empty case is vacuously true). The header comment explains this. Observation; carries forward.

---

## Verified Closed from v4

| ID | Title | Evidence |
|---|---|---|
| H-V4-5 | M-V2-3 extractions (AccountCache / AccountHydrator / InternalRpcHandlers) zero direct tests | `39ccfa7` — `AccountCacheTest.cpp` (3 cases), `AccountHydratorTest.cpp` (2 cases), `InternalRpcHandlersTest.cpp` (5 cases). 10 cases / ~67 assertions. AccountCache stale-flag round-trip uses **value-based** comparison (`GetPasswordHash() == originalHash`, line 64) — not the flaky address-based earlier version. |
| H-V4-6 | C-V3-2 pity/guarantee regression test | `5fcb70d` — `PityRollbackRegressionTest.cpp` (2 cases). Structurally sound: snapshot via X-macro captures both `m_pityBySlot` AND `m_rawPity` (Account.hpp:117-148); test asserts `GetPityState().count(slotId) == 0` post-Rollback (correctly sees both maps empty). If Begin() were moved below GetPity, snapshot would capture post-lazy-insert state, restore would fail to clear, test would fail. Regression signal confirmed. |
| H-V4-7 | AccountRepository populated round-trip + HandleAddCurrency E2E | `9769d37` — `PopulatedRoundTripTest.cpp` (1 case, ~25 assertions covering characters/weapons/gear/party/materials/world flags/weapon equip/story scalar), `AddCurrencyEndToEndTest.cpp` (2 cases asserting events row + accounts.credits column + idempotency cache). |
| H-V4-8 | Idempotency machinery direct tests | `8ff1491` — `IdempotencyMachineryTest.cpp` (5 cases — Store/Find round-trip, ON CONFLICT refresh semantics, sibling-key construction, Scoped input validation, cross-account isolation). |
| M-V4-2 cross-cutting | `credits_added` + `story_xp_gained` golden fixtures | `6357419` — 2 new SchemaMigrationTest cases + 2 fixture files (`v1_credits_added.json`, `v1_story_xp_gained.json`). Vocabulary gap closed. |
| C-V4-1 | OutboxRelay wired | `f24b3ca` — instantiated; **sweep methods themselves untested** (H-V5-2). |
| C-V4-2 | Probe response-body classification | `25a81c1` — code fix verified at ServiceClient.hpp:299-305; **no direct test of the four outcomes** (H-V5-3). |
| H-V4-1 | Begin() above TickQuests in HandleClaimQuestReward | `62ad8df` — code fix; no regression test (analogous to v4's H-V4-6 gap, but smaller blast radius). |
| H-V4-2 | preserve RETURNING created_at | `b6f9f27` — code fix; no test. |
| H-V4-3 | outbox.account_id FK + index | `c35d1a6` — schema + Commit() populates; **no test asserting non-NULL population** (H-V5-5). |
| H-V4-4 | Debug-only longer ServiceClient timeout | `992fd06` — config-only. |
| H-V4-9 | M-V2-7 doc enumeration | `3752b6a` — docs commit. |
| H-V4-10 | TcpServerBase connection caps | `534233c` — code fix; **refusal paths untested** (H-V5-1). |
| TC-V3-1 follow-on | IntegrationDbFixture | `9129a68` (predates v4); v4 tests use it extensively. |

---

## Audit-vs-fix scorecard (v4 → v5)

| Metric | v3→v4 | v4→v5 |
|---|---|---|
| Total commits in arc | 35 | 20 |
| Commits adding tests | ~25 (most piggy-backed onto fixes) | **5 (all test-only)** |
| Net new test cases | +25 | **+27** |
| Net new assertions | +157 | **+297** |
| Fix-vs-test ratio (test commits / total) | ~71% (many fix+test) | **25% test-only commits, but they target the audit-recommended top-5** |
| Critical fixes with test coverage | 2/2 (C-V3-1 yes test; C-V3-2 no test) | **0/2 directly tested** (C-V4-1 sweep methods H-V5-2; C-V4-2 H-V5-3) |
| High fixes with test coverage | 4/14 (LruCache, premake, MAC round-trip, Envelope) | **5/10** (H-V4-5/6/7/8/9 tested; H-V4-1/2/3/4/10 untested) |
| v4 prioritized top-5 status | 2/5 closed (TC-V3-1, TC-V3-4) | **5/5 closed** (H-V4-5/6/7/8 + M-V4-2 cross-cutting) |

**Observation:** the v4 arc adopted the v4 prioritized top-5 as a literal checklist and shipped one test-only commit per item. **Every one of v4's top-5 recommended tests landed.** That is the first time in the audit lineage where this happened. Simultaneously, the v4 arc's *non-test* fixes (C-V4-1 wiring, C-V4-2 classification, H-V4-3 outbox.account_id, H-V4-10 caps) shipped without tests — the same pattern v3→v4 exhibited but inverted: code-only fixes vs the test-only commits for the audit's top-5. The net is positive (test backlog moved decisively) but the new code is still under-protected.

---

## What v1 + v2 + v3 + v4 missed (carries forward)

- **TC-V3-M1 / v4-tail / now M-V5-3 — No `tests/INVARIANTS.md`.** Five audit cycles deep.
- **TC-V3-M2 / v4-tail / now M-V5-4 — No commit-time test gate.** Each audit's arc improves without it; each audit's arc also ships fix-without-test commits.
- **v4-M1 / now M-V5-2 — Audit-test traceability convention.** Zero adoption one day after recommendation.

The pattern of these three is the same: they're meta-policies that would change the audit's question from "is X tested?" to "is X on the gated list?" or "does this commit advance both code AND tests?" Each audit cycle re-discovers the same gaps because there's no mechanical record linking previous findings to test files.

---

## Recommended v5 top-5

Ordered by audit-leverage (closes the most carry-forward findings per test added).

### 1. OutboxRelay sweep methods integration tests
**Closes:** H-V5-2 (new), and validates C-V4-1's headline operational claim.
**Path:** Extend `OutboxRelayTest.cpp` with 3 new cases via `IntegrationDbFixture`. Best landed before next deploy because the operational stakes (12-month partition exhaustion) are the entire reason C-V4-1 was Critical.
**Estimated:** 60-80 LOC.

### 2. Probe response-body classification test in CommonTests
**Closes:** H-V5-3 (new); closes the loop on C-V4-2's "tells you four things" claim.
**Path:** Drive `Probe()` against synthetic `ServiceEndpoint` returning each of the four error shapes; assert each maps to the correct `ProbeOutcome`. Use the same `InvokeForTest` shim or directly construct `RpcCallResult` in a mock.
**Estimated:** 40-60 LOC.

### 3. SessionCache state-machine test in CommonTests
**Closes:** H-V5-7 (carry-forward), plus partially M-V5-1 (Common substance still tiny).
**Path:** `Server/Common/tests/SessionCacheTest.cpp` — 4 cases (extension-on-loss, drop-on-recovery, idempotent-double-loss, idempotent-double-recovery). Lock-friendly state machine; no DB needed.
**Estimated:** 60-80 LOC.

### 4. `InvokeForTest` hardening (#ifdef gate)
**Closes:** H-V5-4 (new). Not a test per se — a one-line build-time guard that turns a forward-looking risk into a structural impossibility.
**Path:** wrap `InvokeForTest` in `#ifdef APHELYON_TEST_BUILD` block; define `APHELYON_TEST_BUILD` in the test premake's `defines = { "APHELYON_TEST_BUILD" }`.
**Estimated:** 5 LOC + premake edit.

### 5. TcpServerBase connection-cap refusal test
**Closes:** H-V5-1 (new); locks in H-V4-10's defense.
**Path:** Subclass `TcpServerBase` in a test, open localhost connections beyond the per-IP cap, assert refusal. Doable in `CommonTests` if the test can plumb a smaller cap, or `AuthTests` against `AuthServer` if not.
**Estimated:** 80-100 LOC (need a no-op subclass).

### 6 (bonus). Snapshot X-macro round-trip
**Closes:** H-V5-6 (3-audit-cycle carry-forward).
**Path:** parameterized loop over the X-macro fields; mutate-snapshot-rollback-assert.
**Estimated:** 50 LOC.

---

## Appendix — files cited

- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\IntegrationDbFixture.hpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountCacheTest.cpp` (3 cases, H-V4-5)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountHydratorTest.cpp` (2 cases, H-V4-5)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\InternalRpcHandlersTest.cpp` (5 cases, H-V4-5)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\PityRollbackRegressionTest.cpp` (2 cases, H-V4-6)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\PopulatedRoundTripTest.cpp` (1 case, H-V4-7)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AddCurrencyEndToEndTest.cpp` (2 cases, H-V4-7)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\IdempotencyMachineryTest.cpp` (5 cases, H-V4-8)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\OutboxRelayTest.cpp` (3 cases — PumpOnce only)
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountTransactionTest.cpp` (6 cases — `[c7]` 6 of ~24 fields)
- `D:\dev\starworks\Gacha\Server\Account\tests\GoldenFile\SchemaMigrationTest.cpp` (6 cases including new credits_added + story_xp_gained, M-V4-2 cross-cutting)
- `D:\dev\starworks\Gacha\Server\Account\tests\events\v1_credits_added.json`
- `D:\dev\starworks\Gacha\Server\Account\tests\events\v1_story_xp_gained.json`
- `D:\dev\starworks\Gacha\Server\Common\tests\InternalRpcAuthRoundTripTest.cpp` (12 cases — `Probe` only via `DebugProbe` helper, not the real Probe() method)
- `D:\dev\starworks\Gacha\Server\Common\tests\EnvelopeShapeTest.cpp` (2 cases)
- `D:\dev\starworks\Gacha\Server\Common\tests\RateLimiterSmokeTest.cpp` (3 cases)
- `D:\dev\starworks\Gacha\Server\Auth\tests\SessionManagerSmokeTest.cpp` (2 cases — defaults + SetConfig)
- `D:\dev\starworks\Gacha\Server\Combat\tests\SmokeTest.cpp` (1 case — `REQUIRE(true)`)
- `D:\dev\starworks\Gacha\Server\Common\src\Net\ServiceEndpoint.hpp:66-77` (`InvokeForTest`, H-V5-4)
- `D:\dev\starworks\Gacha\Server\Common\src\Net\ServiceClient.hpp:289-325` (`Probe`, H-V5-3)
- `D:\dev\starworks\Gacha\Server\Common\src\Net\TcpServerBase.hpp:119-139` (cap refusal, H-V5-1)
- `D:\dev\starworks\Gacha\Server\Common\src\Net\TcpSocket.hpp:48-49` (cap constants)
- `D:\dev\starworks\Gacha\Server\Account\src\Db\OutboxRelay.hpp:115-152` (3 sweep methods, H-V5-2)
- `D:\dev\starworks\Gacha\Server\Account\src\Cache\AccountTransaction.hpp:228-232` (outbox.account_id population, H-V5-5)
- `D:\dev\starworks\Gacha\Server\Account\src\State\Account.hpp:117-148` (Snapshot X-macro, H-V5-6)
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-03-server-persistence-audit-v4.md`
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-03-v4-followup-test-coverage.md`
- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-03-v3-followup-test-coverage.md`
