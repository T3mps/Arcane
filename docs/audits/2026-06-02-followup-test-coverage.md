# Follow-up Audit: Test Coverage

**Date:** 2026-06-02
**Scope:** `Server/Account/tests/` — full enumeration of TEST_CASEs against the previous audit's gap list and the M/H/C remediation pass shipped this week.
**Auditor:** #8 of 8 (test coverage lens)
**Previous audit:** `docs/superpowers/audits/2026-06-02-server-persistence-audit.md`

---

## Summary

Test suite is currently at **162 assertions across 51 TEST_CASEs** (up from 142/49). The remediation pass added exactly two new tests — `[c7]` (snapshot/restore on Rollback) and `[m2]` (empty-commit lazy lease) — both in `AccountTransactionTest.cpp`. **Every other test coverage gap from the previous audit remains open.**

The highest-priority risk is that **none of the fixes shipped this week have regression coverage** outside C7 and M2: the C1 stale-check, C5 PBKDF2 rehash, C6 version pre-check, H1 stripe-locked VerifyCredentials, H2 ON CONFLICT DO UPDATE, H3 idempotency-key length cap, H4 last-login source-of-truth, H5 pool-exhaustion timeout, H6 IP binding, H8 aggregate_kind dedup, H10 EffectDispatcher local cursor, M4 quest-token HMAC, M7 tri-state idempotency, M8 RateLimiter cap, M13 LoadEventVersions ERROR log — **all are untested**. A regression in any of these would not be caught by the current suite.

The existing 162-assertion suite is correct and meaningful for what it covers (reducers + AccountTransaction lifecycle + the three core DB writer classes), but the coverage frontier sits at the boundary of the persistence stack. End-to-end handler tests, multi-thread concurrency tests, integration-test isolation, and TickQuests/EffectDispatcher coverage are all missing.

---

## Coverage by category

### 1. AccountTransaction lifecycle

**Currently tested:** (`tests/Integration/AccountTransactionTest.cpp`)
- `"AccountTransaction commits events + relational flush atomically"` — basic Commit + denorm row + version cursor + clean dirty bits
- `"AccountTransaction rollback writes nothing and marks account stale"` — basic Rollback + IsStale + DB unchanged
- `"AccountTransaction destructor rolls back if Commit() never called"` — dtor-without-commit
- `"AccountTransaction Commit() throws if called twice"` — double-Commit → `std::logic_error`
- `"AccountTransaction skips DB roundtrip when nothing to flush"` `[m2]` — lazy-lease verified with single-conn pool
- `"AccountTransaction Rollback restores in-memory Account to pre-tx state"` `[c7]` — story_level, story_xp, login_streak, credits, materials, world flags

**Still missing:**
- Empty `Rollback()` after partial-mutation Commit success → ensure no second rollback path runs. Adjacent to `[m2]`.
- `Commit()` exception in the **post-commit** path (M19 fix — try/catch around bookkeeping after `tx_->commit()`). No test exercises the case where `tx_->commit()` succeeded but `MutableDirty().Clear()` throws.
- `[c7]` does not cover: owned characters/weapons/gear, char_traces, loadouts, party_slots, pity state, accounts denorm row fields beyond story/streak (e.g., last_streak_day), quest states/objectives. The Memento is claimed to snapshot the **full** Account, but the test only verifies 6 fields.

**Priority:** MEDIUM — lifecycle skeleton is well-covered; the `[c7]` coverage is the load-bearing gap because a partial Memento (missing one field) would silently leave that field speculatively mutated.

---

### 2. EventStore

**Currently tested:** (`tests/Integration/EventStoreRoundTripTest.cpp`)
- `"EventStore appends and reads back events"` — Append + LoadStream
- `"EventStore rejects duplicate version with 23505"` — ConcurrencyConflict
- `"EventStore treats duplicate idempotency_key as success"` — AppendIdempotent no-op

**Still missing:**
- **H8 regression test:** cross-aggregate idempotency_key reuse. With H8's fix adding `aggregate_kind` to the dedup query, the same key on Wallet vs Pulls should be allowed; the same key on Wallet twice should dedup. No test exists.
- `LoadStream(from_version=N)` with non-zero N — partial replay coverage.
- `LoadStream` ordering invariant (by version asc). Not asserted.
- Cross-account isolation — two accounts with overlapping idempotency_keys.

**Priority:** HIGH for the H8 regression (the fix is surgical to the dedup SQL; trivially regressable). MEDIUM for the rest.

---

### 3. AccountRepository

**Currently tested:** (`tests/Integration/AccountRepositoryTest.cpp`)
- `"AccountRepository::Create returns AccountData with assigned IDs"`
- `"AccountRepository::Create rejects duplicate username"`
- `"AccountRepository::Save round-trips story state through DB"` — only `storyLevel`/`storyXp`/`loginStreak`/`publicUid`
- `"AccountRepository::LoadByUsername round-trip"` — only IDs
- `"AccountRepository::Delete soft-deletes (sets deleted_at)"`

**Still missing:**
- **LoadById round-trip over a populated account** — the 9 `Load*` helpers (characters, weapons, gear, gear_substats, char_traces, loadouts, party_slots, pity_state, world_flags, quest_states/objectives, material_inventory, event-versions) are not exercised. Only 4 scalars verified.
- **`FindIdempotency` tri-state** — M7's `{Hit, Miss, Error}` triad. No test calls `FindIdempotency` at all.
- `BumpLastLogin` — H4's fix moves `last_login` writes here. Not tested.
- `UpdatePasswordHash` — C5's lazy-rehash path. Not tested.
- `LoadEventVersions` — populates `cached_wallet_version`, `cached_pulls_version`, etc. No hydration test exists.

**Priority:** HIGH — Repository is the persistence boundary. Most of these helpers are silent functions that fail in subtle ways (wrong column order, JSON parse mismatch on enum). A populated-account round-trip is the single highest-value addition.

---

### 4. RelationalFlush sub-methods

**Currently tested:** (`tests/Integration/RelationalFlushTest.cpp`)
- `"RelationalFlush updates accounts.story_level when dirty"` — only `FlushAccountsRow` story_level path
- `"RelationalFlush UPSERTs a party slot"` — `FlushPartySlots`
- `"RelationalFlush no-ops when dirty is empty"` — null-flush

**Still missing (per previous audit, all confirmed still gone):**
- FlushAccountsRow wallet/stats columns (credits, universalCredits, tickets, limitedTickets, scrap)
- FlushAccountsRow last_login (H4 — verify it is NOT touched on idle-evict flush)
- FlushAccountsRow last_streak_day (H11 timezone)
- FlushOwnedCharacters
- FlushCharTracesAdded / FlushCharTracesRemoved
- FlushOwnedWeapons (incl. refinement)
- FlushOwnedGear
- FlushGearRemovals
- FlushLoadouts
- FlushMaterials (incl. wipe-and-rewrite semantics)
- FlushWorldFlagAdds / FlushWorldFlagRemoves
- FlushQuests (TickQuests interaction)
- FlushQuestObjectives (wipe-and-rewrite, M15 slot_idx renumber)
- FlushPity (per-banner)

**Priority:** HIGH — `RelationalFlush::Flush*` is the funnel through which every mutation reaches DB. Story_level coverage is ~5% of the surface. Any silent SQL typo in the other 12 methods is undetectable.

---

### 5. Reducers (4)

**Currently tested:**
- `WalletReducer` (`tests/ReducerTests/WalletReducerTest.cpp`): 4 TEST_CASEs. Property tests in `tests/PropertyTests/WalletPropertyTest.cpp`: 4 properties (triple-replay, snapshot equivalence, balances non-negative, would-go-negative throws).
- `PullsReducer` (`tests/ReducerTests/PullsReducerTest.cpp`): 6 TEST_CASEs. Property tests in `tests/PropertyTests/PullsPropertyTest.cpp`: 2 properties (triple-replay, slot isolation).
- `QuestClaimsReducer` (`tests/ReducerTests/QuestClaimsReducerTest.cpp`): 3 TEST_CASEs (claim records ID, rewards emit effects, double-claim throws).
- `ProgressionReducer` (`tests/ReducerTests/ProgressionReducerTest.cpp`): 3 TEST_CASEs (level advance updates state, no-extra-effect on difficulty unlock, overflow_credits emits GrantCurrencyEffect).

**Still missing:**
- QuestClaimsReducer property tests (replay determinism, claim-set monotonicity).
- ProgressionReducer property tests (xp accumulation monotonicity, level transitions never lose XP).
- **Cross-aggregate replay determinism** property test interleaving wallet+pulls+claims+progression events — confirms reducer purity across the full event log.

**Priority:** MEDIUM. The four reducers all have unit coverage; property tests for the two unfair-share reducers (QuestClaims + Progression) would close the asymmetry.

---

### 6. Concurrency

**Currently tested:** None. Zero tests in `tests/Integration/` spawn multiple threads against AccountServer/AccountCache/AccountTransaction.

**Still missing (all from previous audit):**
- Stripe lock ordering — two-load-race for the same player should serialize.
- C1 stale-check + reload — after Rollback, GetLockedAccount must erase from map and reload. **C1's fix is unverified.**
- Eviction-during-handler — CleanupIdleAccounts mid-handler must not yank the Account.
- M1 OnStopped two-phase stripe locks — Save under map mutex without stripe lock = race.
- H1 VerifyCredentials stripe lock — internal RPC must take stripe lock first.

**Priority:** HIGH — C1 is the entire executive-summary "stale flag never checked" fix. Without a concurrency test asserting the stale erase+reload pattern, the fix is structurally invisible.

---

### 7. Idempotency end-to-end

**Currently tested:** None at the AccountTransaction layer. The EventStore-level idempotency (AppendIdempotent) is tested but that's a different cache.

**Still missing:**
- **Cache hit returns cached payload byte-exact** — handler retry with same idempotency_key gets the cached JSON, not a fresh execution.
- **Expired entry triggers fresh execution + UPSERT (H2)** — old payload replaced.
- **Atomic rollback** — failed Commit must not leave a `success=true` row in `idempotency_cache`.
- **H3 oversized key rejected** — `clientKey.size() > 128` returns empty / handler rejects.
- **M7 tri-state {Hit, Miss, Error}** — verify each path produces the documented behavior.

**Priority:** HIGH — H2 and H3 are this week's fixes; both directly affect double-charging behavior. A regression of either is a billing-class bug.

---

### 8. TickQuests::Apply

**Currently tested:** None.

**Still missing (per previous audit, confirmed):**
- Unlock-on-load (story_level prereq met → quest transitions to Active).
- Expire/recycle/auto-complete interaction for daily quests.
- Idempotence under repeated Apply (running Apply twice on the same Account is a no-op the second time).
- Interaction with FlushQuests (verifying dirty bits flow through).

**Priority:** HIGH — TickQuests is invoked from LoadAccountFromData on every cache miss. A regression silently breaks quest unlocks for an entire login cycle.

---

### 9. EffectDispatcher

**Currently tested:** None.

**Still missing:**
- **H10 regression: two GrantCurrencyEffect calls produce sequential versions** — the fix added a local cursor; the test should call dispatch twice and assert the two events have versions N+1 and N+2 (not both N+1).
- General contract test — each Effect variant produces the expected event(s).

**Priority:** MEDIUM. The previous audit noted EffectDispatcher is **vestigial** (not invoked from handlers). If it stays vestigial, deleting is the better fix than testing. If it gets wired in, H10 regression coverage is mandatory.

---

### 10. End-to-end handler tests (HandleSetParty / HandleAddCurrency / HandlePull / HandleClaimQuestReward)

**Currently tested:** None.

**Still missing:**
- HandleSetParty: encode request → handler → DB inspection on `party_slots`.
- HandleAddCurrency: full path through rate limiter → idempotency → event → flush → outbox → audit.
- HandlePull: pity bump, guarantee flip, wallet decrement all atomic.
- HandleClaimQuestReward: reward grants, claimed-set update, login-streak side effect.

**Priority:** HIGH — handlers are the API surface. Every M/H fix from this week intersects at least one handler. End-to-end smoke would catch protocol-id drift, JSON-key drift, and idempotency-scope drift.

---

### 11. Security path tests

**Currently tested:** None.

**Still missing (all this week's fixes):**
- **C5 PBKDF2 lazy rehash** on weak-iteration password — Login with old 10k hash must succeed and write back a 200k hash.
- **C5 PBKDF2 iteration count** — newly-created accounts must use 200k.
- **H6 IP binding rejection** — token created from IP_A must reject IP_B request when binding enabled.
- **M3 Internal RPC HMAC** — good MAC accepted, bad MAC rejected, stale timestamp rejected. (Not yet implemented per remediation list, but listed as a gap.)
- **M4 quest token HMAC + constant-time compare** — `Crypto::HMAC_SHA256` swapped in; ensure `ConstantTimeCompare` is the comparison.
- **C3 rate-limiter default-enabled** — assert `g_rateLimitingEnabled == true` in Release builds.
- **M8 RateLimiter cap behavior** — assert refusal at `MAX_RECORDS`.
- **Pool exhaustion timeout (H5)** — `acquire()` throws `PoolExhausted` after configured timeout; does not deadlock.

**Priority:** HIGH — security regressions are silent until exploited. Every one of these is a yes/no behavior that a single test can pin down.

---

### 12. Cross-aggregate replay determinism

**Currently tested:** None. Property tests exist per-reducer (Wallet + Pulls) but never interleave events from multiple aggregates against a multi-aggregate fold.

**Still missing:**
- Generate a mixed event log {WalletEvent | PullsEvent | QuestClaimsEvent | ProgressionEvent}, fold three times, assert deterministic final state.

**Priority:** MEDIUM — would catch ordering-dependent reducer side effects. Adjacent property tests would likely catch the most egregious cases.

---

### 13. Integration test isolation

**Status (verified):** Each test calls `MakeAccount(pool, prefix)` with `UniqueUsername(prefix + UuidV7)`, so per-test accounts don't collide. **But:** events, snapshots, outbox, audit_log, idempotency_cache rows accumulate forever — `Server/Account/tests/` has no teardown, no transaction-scoped fixture, no truncate. Repeated suite runs grow the test schema's tables indefinitely.

Specifically:
- `EventStoreRoundTripTest` leaves wallet events behind on every run.
- `SnapshotWriterTest` leaves rows in `snapshots`.
- `OutboxRelayTest` leaves rows in `outbox` (some dispatched, some not).
- `RelationalFlushTest` leaves rows in `accounts` + `party_slots`.
- `AccountTransactionTest` writes events + accounts row + audit_log + outbox per test.

**Risk:** Slow tests over time. Latent risk of cross-test interference if any future test uses non-unique IDs. The `partman` rollover on `events` partially mitigates the events bloat but not the others.

**Still missing:** A `TestFixture` that wraps each TEST_CASE in a `BEGIN; ... ROLLBACK;` or a `TRUNCATE accounts CASCADE` at test entry.

**Priority:** MEDIUM — not a correctness bug today; a maintainability ticking clock.

---

### 14. WalletPropertyTest empty-input behavior

**Status (verified):** `Server/Account/tests/PropertyTests/WalletPropertyTest.cpp:122` still reads `if (built.events.empty()) return;` — silent skip, **no `RC_DISCARD()`**. rapidcheck counts this as a passing trial, so the test passes silently when rapidcheck happens to roll empty sequences. The actual snapshot-equivalence property is under-exercised by however-many discarded trials.

**Still missing:** Replace `return;` with `RC_DISCARD();` so rapidcheck retries with a non-empty generator.

**Priority:** LOW — the test still detects regressions when it doesn't skip; just less coverage than nominal.

---

## Highest-priority test gaps

Top 5 tests that should be written next, ranked by remediation-pass risk.

### 1. C1 stale-flag-checked-in-GetLockedAccount integration test

**File:** new `tests/Integration/StaleReloadTest.cpp` `[integration][concurrency][c1]`

**Test case name:** `"GetLockedAccount erases stale account and reloads from DB"`

**Setup:**
- Create account with `MakeAccount`.
- Load via AccountServer, mutate `story_level=50`, force `Rollback()` via failed Commit.
- Without a fresh request, verify `account.IsStale() == true`.
- Issue a second `GetLockedAccount(accountId)`.
- Verify the returned account has `story_level == 1` (DB default) — proving the stripe-locked stale check fired.

**Assertions:**
- `REQUIRE(reloaded.GetStoryLevel() == 1);   // not 50`
- `REQUIRE_FALSE(reloaded.IsStale());`
- `REQUIRE(m_accounts.count(accountId) == 1);   // re-populated`

**Why HIGH:** C1 is in the executive summary as the single largest unfixed-bug class. Its fix is structurally invisible without this test.

---

### 2. H2 idempotency UPSERT-on-expired-row integration test

**File:** new `tests/Integration/IdempotencyTest.cpp` `[integration][idempotency][h2]`

**Test case name:** `"Expired idempotency row is replaced by ON CONFLICT DO UPDATE"`

**Setup:**
- `MakeAccount`. Run `HandleAddCurrency` with `idempotency_key="ic_h2"`.
- Direct SQL: `UPDATE idempotency_cache SET expires_at = now() - interval '1 day' WHERE scoped_key LIKE 'ic_h2%'`.
- Re-run `HandleAddCurrency` same key, but with `amount=200` this time.
- Read `idempotency_cache` row and the cached response.

**Assertions:**
- `REQUIRE(row.response_payload contains "200")   // refreshed`
- `REQUIRE(row.expires_at > now())                // refreshed TTL`
- `REQUIRE(wallet.credits == initial + 100 + 200) // second exec ran`

**Why HIGH:** H2 closes the **silent double-charge** path. Long-tail retries are easy to test, hard to detect in production.

---

### 3. AccountRepository populated round-trip

**File:** extend `tests/Integration/AccountRepositoryTest.cpp` `[integration][repo][round_trip]`

**Test case name:** `"AccountRepository::LoadById restores full collection state"`

**Setup:** Build an Account in code with:
- 3 owned characters with traces
- 2 owned weapons (one refined)
- 1 owned gear with substats
- party `[c1,c2,"",""]`
- 2 materials
- 3 world flags
- pity state on 2 banners
- 4 quest states with mixed objectives
- 1 loadout

Call `repo.Save(account)`. Reload via `repo.LoadById`.

**Assertions:** Field-by-field equality with the constructed Account. Particularly:
- `REQUIRE(reloaded.cached_wallet_version == account.Dirty().cached_wallet_version);`
- `REQUIRE(reloaded.GetParty() == party);`
- `REQUIRE(reloaded.GetQuestStates().size() == 4);`
- Per-quest objective ordering preserved (M15).

**Why HIGH:** This is the single test that gates 9 untested `Load*` helpers. The previous audit called it out explicitly.

---

### 4. EffectDispatcher H10 local-cursor test

**File:** new `tests/Unit/EffectDispatcherTest.cpp` `[unit][effect_dispatcher][h10]`

**Test case name:** `"EffectDispatcher assigns sequential versions across multiple GrantCurrencyEffect calls"`

**Setup:**
- Account with `cached_wallet_version = 5`.
- Dispatch `GrantCurrencyEffect{credits, 100}`, then `GrantCurrencyEffect{credits, 50}`.
- Inspect events appended.

**Assertions:**
- `REQUIRE(events[0].version == 6);`
- `REQUIRE(events[1].version == 7);   // NOT 6 again`

**Why HIGH:** H10's fix is one line; regression is one line away. Also forces a decision on the dispatcher's vestigial status (L4) — if no handler calls it, deleting is preferable to testing.

---

### 5. End-to-end HandleAddCurrency through DB inspection

**File:** new `tests/Integration/HandleAddCurrencyTest.cpp` `[integration][handler][add_currency]`

**Test case name:** `"HandleAddCurrency atomically writes event + wallet + outbox + audit_log + idempotency"`

**Setup:**
- `MakeAccount`. Construct an `AddCurrency` request JSON with `idempotency_key="ic_add_001"`, amount=500, currency="credits".
- Invoke handler via `AccountHandlers::HandleAddCurrency` (or whatever the entry path is).
- Inspect 5 tables: `events`, `accounts` (denorm), `outbox`, `audit_log`, `idempotency_cache`.

**Assertions:**
- 1 row in `events` with `aggregate_kind='wallet'` and `version=N+1`.
- `accounts.wallet_credits = initial + 500`.
- 1 row in `outbox` for the wallet destination.
- 1 row in `audit_log` for this player.
- 1 row in `idempotency_cache` with `success=true` and a non-empty payload.

Then re-issue same idempotency_key, assert: still 1 of each row (no duplicates), response is the cached payload byte-exact.

**Why HIGH:** Single test covers H2, H3, H7, M7, denorm-sync (`516553a`), and AccountTransaction commit ordering all at once. End-to-end happy path on the most-exercised mutating handler.

---

## New tests added this pass

### `[c7]` — `"AccountTransaction Rollback restores in-memory Account to pre-tx state"`

**Claim:** "the Memento snapshot captured in the AccountTransaction constructor must move back into the live Account on Rollback so a handler whose Commit failed can't read or return speculatively-mutated state."

**What it actually verifies:**
- `story_level`, `story_xp`, `login_streak` restored ✓
- `wallet.credits` restored ✓
- `material_inventory["iron_ore"]` restored, speculative material `"ash"` gone ✓
- `world_flags.intro_seen` restored, speculative `speculative_flag` gone ✓
- `IsStale()` remains true ✓

**What it does NOT verify:**
- Owned characters / weapons / gear (the bulk of the Memento payload)
- Char traces (added or removed)
- Loadouts
- Party slots
- Pity state
- Quest states + objectives
- Cached event-version cursors

**Verdict:** Partial coverage. The 6 verified fields are a meaningful slice but the Memento has a much larger surface and the test doesn't gate the bulk of it. **Recommend extending [c7]** with a "full state" variant once an AccountBuilder helper exists.

### `[m2]` — `"AccountTransaction skips DB roundtrip when nothing to flush"`

**Claim:** "an empty transaction (no events appended, no dirty bits set) must not acquire a pool connection."

**What it actually verifies:**
- ConnectionPool sized to 1, lone connection held externally.
- Empty `Commit()` succeeds (would block on `acquire()` if the lazy lease weren't in place).
- Account stays clean, no stale flag.

**Verdict:** Tight, well-designed test. Pool-exhaustion-by-construction is a clever way to prove the negative ("did NOT acquire"). **Honest coverage of the contract.**

---

## Test infrastructure

### Integration test isolation
**Status:** Per-test data isolation via `UniqueUsername(prefix)` works (no row-level collisions). **No table-level teardown anywhere** — all integration tests accumulate rows in `accounts`, `events`, `snapshots`, `outbox`, `audit_log`, `idempotency_cache`, `party_slots`, etc. across runs.

Risk: slow tests over time; latent risk of cross-test interference if any future test uses a non-unique key. No `BEGIN; ... ROLLBACK;` fixture pattern, no per-test `TRUNCATE`.

**Recommendation:** Add a Catch2 `TEST_CASE_METHOD` fixture that wraps each test in a transaction that gets rolled back, OR a session-level fixture that truncates `accounts` (CASCADE) at the start of the test session.

### Property test infrastructure (rapidcheck)
Integrated via `rapidcheck/catch.h` (Catch2 macro `RC_ASSERT` works inside `TEST_CASE`). Two property suites — `WalletPropertyTest.cpp` (4 properties), `PullsPropertyTest.cpp` (2 properties). No `QuestClaimsPropertyTest.cpp` or `ProgressionPropertyTest.cpp`. No cross-aggregate property suite.

**Caveat:** `WalletPropertyTest.cpp:122` uses `return;` instead of `RC_DISCARD();` — rapidcheck counts these as silent passes. **Not a regression-blocker but degrades trial-count fidelity.**

### Golden file infrastructure
4 v1 golden JSONs in `tests/events/`:
- `v1_credits_spent.json`
- `v1_pull_performed.json`
- `v1_quest_reward_claimed.json`
- `v1_story_level_advanced.json`

One test per file in `tests/GoldenFile/SchemaMigrationTest.cpp`. Each test reads the v1 JSON, runs `FromJson` deserializer, folds through the appropriate reducer, asserts expected output. **Schema-version migration is well-gated by these 4 tests for the currently-shipped events.** A 5th aggregate (or a v2 schema for any existing event) would need a corresponding golden + test added; this is not yet automated.

### Tests that look like they pass but don't fully exercise their claim

1. **`[c7]` Rollback restores in-memory state** — claims "full state restoration" but exercises 6 of ~24 fields. (See "New tests added" above.)
2. **`"AccountRepository::Save round-trips story state through DB"`** — name says "story state" but Account has ~9 collections beyond story state; nothing else is round-tripped. The previous audit calls this out explicitly.
3. **`WalletPropertyTest.cpp:122`** — silent `return;` instead of `RC_DISCARD()`. Test "passes" on every empty-sequence trial without exercising the property.
4. **`"EventStore treats duplicate idempotency_key as success"`** — verifies the no-op return but does NOT verify the cross-aggregate dimension (H8's fix). Cross-aggregate key reuse is the exact scenario H8 closed; that scenario is not tested.

---

## Appendix — Files cited

- `D:\dev\starworks\Gacha\docs\superpowers\audits\2026-06-02-server-persistence-audit.md`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountTransactionTest.cpp` (lines 130-220 for [m2] and [c7])
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\AccountRepositoryTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\EventStoreRoundTripTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\RelationalFlushTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\OutboxRelayTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\Integration\SnapshotWriterTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\PropertyTests\WalletPropertyTest.cpp` (line 122 — silent return)
- `D:\dev\starworks\Gacha\Server\Account\tests\PropertyTests\PullsPropertyTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\ReducerTests\*ReducerTest.cpp` (4 files)
- `D:\dev\starworks\Gacha\Server\Account\tests\GoldenFile\SchemaMigrationTest.cpp`
- `D:\dev\starworks\Gacha\Server\Account\tests\events\v1_*.json` (4 golden inputs)
